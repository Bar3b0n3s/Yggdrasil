#include "EnginePCH.h"
#include "Engine/Renderer/TonemapPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ExposureConstants.h"
#include "Shared/TonemapConstants.h"
#include "Shared/ViewConstants.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

// The Tonemap program (Passes/Tonemap.slang) applies the steps of TonemapPass.h; its settings are push constants
// (TonemapConstants), so one pipeline serves every tonemapper and flag. Absent optional 2D inputs are replaced in the binding
// set by SceneColor, which the shader then never reads (its flag is clear): every binding of the layout always holds a valid
// descriptor, and no placeholder texture is needed for them. The adapted exposure (t8, M26) is a buffer, so its stand-in is the
// pass's own one-element buffer, which the shader never reads either (TonemapFlagAutoExposure is clear) and which is
// therefore never written. The LUT (t9, M26) is a 3D texture, which SceneColor cannot stand in for, so the pass keeps a
// 1-texel 3D texture of its own for the records without one.

namespace Engine {

	static_assert(std::to_underlying(RenderTonemapper::AgX) == TonemapperAgx && std::to_underlying(RenderTonemapper::Aces) == TonemapperAces
			&& std::to_underlying(RenderTonemapper::PbrNeutral) == TonemapperPbrNeutral && std::to_underlying(RenderTonemapper::Linear) == TonemapperLinear,
		"TonemapConstants::Tonemapper takes RenderTonemapper's values unchanged");

	namespace Utils {

		// The Tonemap program's [numthreads(8, 8, 1)].
		constexpr uint32_t TonemapGroupSize = 8;
		constexpr std::string_view TonemapProgram = "Tonemap";
		// Set 0 of Passes/Tonemap.slang: b0 ViewConstants, the push constants (slot 1, beside b0), t0 SceneColor, t1 Bloom,
		// t7 BlueNoise (§8.4's register of the blue noise), t8 the adapted exposure (M26), t9 Lut (M26,
		// Shared/TonemapConstants.h), s0 LinearClamp, u0 LdrColor.
		constexpr uint32_t PushConstantsSlot = 1;
		constexpr uint32_t SceneColorSlot = 0;
		constexpr uint32_t BloomSlot = 1;
		constexpr uint32_t BlueNoiseSlot = 7;
		constexpr uint32_t AdaptedExposureSlot = 8;
		constexpr uint32_t LutSlot = 9;

		static PipelineLayoutDescription MakeTonemapDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::ConstantBuffer(0),
				nvrhi::BindingLayoutItem::PushConstants(PushConstantsSlot, sizeof(TonemapConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(SceneColorSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(BloomSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(BlueNoiseSlot),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(AdaptedExposureSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(LutSlot),
				nvrhi::BindingLayoutItem::Sampler(0),
				nvrhi::BindingLayoutItem::Texture_UAV(0),
			};
			return {
				.Name = "Tonemap",
				.Program = std::string(TonemapProgram),
				.Entries = { "CSMain" },
				.BindingLayouts = { layout },
				.StorageImages = { { .Set = 0, .Register = 0, .Format = LdrColorFormat } },
				.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = sizeof(ViewConstants) } },
			};
		}

		// M26 (C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decision 15, stream X-B): the LUT step's constants of a record
		// with a LUT: TonemapFlagLut, its share (PostProcessSettings::LutIntensity, documented in [0, 1], clamped like the bloom's,
		// a non-finite one grading nothing) and the coordinate's scale and offset, which fold the domain and the texel centres
		// (Shared/TonemapConstants.h), computed in double and rounded once.
		static void ApplyLutConstants(const TonemapPassInputs& inputs, TonemapConstants& constants)
		{
			if (inputs.Lut == nullptr)
				return;
			const nvrhi::TextureDesc& lut = inputs.Lut->getDesc();
			ENGINE_CORE_ASSERT(lut.dimension == nvrhi::TextureDimension::Texture3D && lut.width == lut.height && lut.width == lut.depth && lut.width >= 2,
				"TonemapPass::Record needs a cubic 3D LUT of at least 2 entries a side");
			constants.Flags |= TonemapFlagLut;
			constants.LutIntensity = std::isfinite(inputs.LutIntensity) ? std::clamp(inputs.LutIntensity, 0.0f, 1.0f) : 0.0f;
			const double size = lut.width;
			for (glm::length_t channel = 0; channel < 3; ++channel)
			{
				const double low = inputs.LutDomainMin[channel];
				const double high = inputs.LutDomainMax[channel];
				ENGINE_CORE_ASSERT(std::isfinite(low) && std::isfinite(high) && low < high, "TonemapPass::Record needs a LUT domain with its minimum below its maximum");
				const double scale = (size - 1.0) / (size * (high - low));
				constants.LutScale[channel] = static_cast<float>(scale);
				constants.LutOffset[channel] = static_cast<float>(0.5 / size - low * scale);
			}
		}

		// A 1x1x1 RGBA16_FLOAT 3D texture of zeros, bound at t9 when a record has no LUT (the shader reads it only with
		// TonemapFlagLut). A startup object of the pass, not an asset upload: a render target (so the injected texture
		// out-of-memory, which strikes sampled-only textures, leaves it alone) cleared once by one command list executed at
		// once, kept in ShaderResource.
		static Result<nvrhi::TextureHandle> CreateEmptyLut(GraphicsDevice& device)
		{
			nvrhi::TextureDesc desc;
			desc.dimension = nvrhi::TextureDimension::Texture3D;
			desc.width = 1;
			desc.height = 1;
			desc.depth = 1;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isShaderResource = true;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "TonemapPass.EmptyLut";
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle texture, device.CreateTexture(desc));
			// Not an immediate command list: a caller may have the frame's immediate list open (NVRHI's validation allows one).
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
				device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			CommandListRecording recording(device, *commandList);
			commandList->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
			recording.Execute();
			return texture;
		}

		// The push constants of one record: the flags follow the inputs that are present (TonemapPass.h).
		static TonemapConstants MakeTonemapConstants(const TonemapPassInputs& inputs)
		{
			TonemapConstants constants;
			constants.Tonemapper = std::to_underlying(inputs.Settings.Tonemapper);
			if (inputs.Bloom != nullptr)
				constants.Flags |= TonemapFlagBloom;
			if (inputs.Settings.EncodeSrgb)
				constants.Flags |= TonemapFlagEncodeSrgb;
			if (inputs.Settings.Dither && inputs.BlueNoise != nullptr)
				constants.Flags |= TonemapFlagDither;
			// M26 (R-AEX, stream X-A): the Exposure pass's adapted exposure multiplies the view's.
			if (inputs.AdaptedExposure != nullptr)
				constants.Flags |= TonemapFlagAutoExposure;
			// PostProcessSettings::BloomIntensity is documented in [0, 1]; a value from outside (a scene file) is clamped, and
			// a non-finite one composites no bloom.
			constants.BloomIntensity = std::isfinite(inputs.BloomIntensity) ? std::clamp(inputs.BloomIntensity, 0.0f, 1.0f) : 0.0f;
			ApplyLutConstants(inputs, constants);
			return constants;
		}

	}

	struct TonemapPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
		nvrhi::SamplerHandle LinearClamp{};
		// M26 (R-AEX, stream X-A): t8's stand-in without an adapted exposure (see the file comment).
		nvrhi::BufferHandle NoAdaptedExposure{};
		nvrhi::TextureHandle EmptyLut{}; // M26: t9 of the records without a LUT (Utils::CreateEmptyLut)
		uint32_t PipelineCount = 0;
	};

	TonemapPass::TonemapPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	TonemapPass::~TonemapPass() = default;

	Result<Scope<TonemapPass>> TonemapPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<TonemapPass> pass = CreateScope<TonemapPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateComputePipeline({ .Layout = Utils::MakeTonemapDescription(), .Specializations = {} }));
		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(state.LinearClamp, device.CreateSampler(sampler));
		nvrhi::BufferDesc noAdaptedExposure;
		noAdaptedExposure.byteSize = sizeof(ExposureState);
		noAdaptedExposure.structStride = sizeof(ExposureState);
		noAdaptedExposure.initialState = nvrhi::ResourceStates::ShaderResource;
		noAdaptedExposure.keepInitialState = true;
		noAdaptedExposure.debugName = "TonemapPass.NoAdaptedExposure";
		ENGINE_TRY_ASSIGN(state.NoAdaptedExposure, device.CreateBuffer(noAdaptedExposure));
		ENGINE_TRY_ASSIGN(state.EmptyLut, Utils::CreateEmptyLut(device));
		state.PipelineCount = PipelineCount;
		return pass;
	}

	std::vector<PipelineLayoutDescription> TonemapPass::GetLayoutDescriptions()
	{
		return { Utils::MakeTonemapDescription() };
	}

	uint32_t TonemapPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status TonemapPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TonemapPassInputs& inputs)
	{
		ENGINE_CORE_ASSERT(inputs.ViewConstants != nullptr && inputs.SceneColor != nullptr && inputs.LdrColor != nullptr,
			"TonemapPass::Record needs ViewConstants, SceneColor and LdrColor");
		const nvrhi::TextureDesc& target = inputs.LdrColor->getDesc();
		ENGINE_CORE_ASSERT(inputs.SceneColor->getDesc().width == target.width && inputs.SceneColor->getDesc().height == target.height,
			"TonemapPass::Record needs SceneColor and LdrColor of the same size");
		State& state = *m_State;
		const TonemapConstants constants = Utils::MakeTonemapConstants(inputs);

		// The bloom chain is read at its mip 0 only, where BloomPass leaves the bloom.
		nvrhi::ITexture* bloom = inputs.Bloom != nullptr ? inputs.Bloom : inputs.SceneColor;
		const nvrhi::TextureSubresourceSet bloomSubresources = inputs.Bloom != nullptr ? nvrhi::TextureSubresourceSet(0, 1, 0, 1) : nvrhi::AllSubresources;
		nvrhi::ITexture* blueNoise = (constants.Flags & TonemapFlagDither) != 0 ? inputs.BlueNoise : inputs.SceneColor;
		nvrhi::IBuffer* adaptedExposure = inputs.AdaptedExposure != nullptr ? inputs.AdaptedExposure : state.NoAdaptedExposure.Get();
		nvrhi::ITexture* lut = (constants.Flags & TonemapFlagLut) != 0 ? inputs.Lut : state.EmptyLut.Get();
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, inputs.ViewConstants),
			nvrhi::BindingSetItem::PushConstants(Utils::PushConstantsSlot, sizeof(TonemapConstants)),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SceneColorSlot, inputs.SceneColor),
			nvrhi::BindingSetItem::Texture_SRV(Utils::BloomSlot, bloom, nvrhi::Format::UNKNOWN, bloomSubresources),
			nvrhi::BindingSetItem::Texture_SRV(Utils::BlueNoiseSlot, blueNoise),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::AdaptedExposureSlot, adaptedExposure),
			nvrhi::BindingSetItem::Texture_SRV(Utils::LutSlot, lut),
			nvrhi::BindingSetItem::Sampler(0, state.LinearClamp),
			nvrhi::BindingSetItem::Texture_UAV(0, inputs.LdrColor, LdrColorFormat),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *state.Pipeline.BindingLayouts[0]));
		nvrhi::ComputeState compute;
		compute.pipeline = state.Pipeline.Pipeline;
		compute.bindings = { set };

		commandList.beginMarker("Tonemap");
		commandList.setComputeState(compute);
		commandList.setPushConstants(&constants, sizeof(constants));
		commandList.dispatch((target.width + Utils::TonemapGroupSize - 1) / Utils::TonemapGroupSize,
			(target.height + Utils::TonemapGroupSize - 1) / Utils::TonemapGroupSize);
		commandList.endMarker();
		return {};
	}

}
