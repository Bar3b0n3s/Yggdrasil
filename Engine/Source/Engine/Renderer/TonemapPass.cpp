#include "EnginePCH.h"
#include "Engine/Renderer/TonemapPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/TonemapConstants.h"
#include "Shared/ViewConstants.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

// The Tonemap program (Passes/Tonemap.slang) applies the steps of TonemapPass.h; its settings are push constants
// (TonemapConstants), so one pipeline serves every tonemapper and flag. Absent optional inputs are replaced in the binding
// set by SceneColor, which the shader then never reads (its flag is clear): every binding of the layout always holds a valid
// descriptor, and no placeholder texture is needed.

namespace Engine {

	static_assert(std::to_underlying(RenderTonemapper::AgX) == TonemapperAgx && std::to_underlying(RenderTonemapper::Aces) == TonemapperAces
			&& std::to_underlying(RenderTonemapper::PbrNeutral) == TonemapperPbrNeutral && std::to_underlying(RenderTonemapper::Linear) == TonemapperLinear,
		"TonemapConstants::Tonemapper takes RenderTonemapper's values unchanged");

	namespace Utils {

		// The Tonemap program's [numthreads(8, 8, 1)].
		constexpr uint32_t TonemapGroupSize = 8;
		constexpr std::string_view TonemapProgram = "Tonemap";
		// Set 0 of Passes/Tonemap.slang: b0 ViewConstants, the push constants (slot 1, beside b0), t0 SceneColor, t1 Bloom,
		// t7 BlueNoise (§8.4's register of the blue noise), s0 LinearClamp, u0 LdrColor.
		constexpr uint32_t PushConstantsSlot = 1;
		constexpr uint32_t SceneColorSlot = 0;
		constexpr uint32_t BloomSlot = 1;
		constexpr uint32_t BlueNoiseSlot = 7;

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
			// PostProcessSettings::BloomIntensity is documented in [0, 1]; a value from outside (a scene file) is clamped, and
			// a non-finite one composites no bloom.
			constants.BloomIntensity = std::isfinite(inputs.BloomIntensity) ? std::clamp(inputs.BloomIntensity, 0.0f, 1.0f) : 0.0f;
			return constants;
		}

	}

	struct TonemapPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
		nvrhi::SamplerHandle LinearClamp{};
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
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, inputs.ViewConstants),
			nvrhi::BindingSetItem::PushConstants(Utils::PushConstantsSlot, sizeof(TonemapConstants)),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SceneColorSlot, inputs.SceneColor),
			nvrhi::BindingSetItem::Texture_SRV(Utils::BloomSlot, bloom, nvrhi::Format::UNKNOWN, bloomSubresources),
			nvrhi::BindingSetItem::Texture_SRV(Utils::BlueNoiseSlot, blueNoise),
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
