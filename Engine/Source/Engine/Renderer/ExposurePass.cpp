#include "EnginePCH.h"
#include "Engine/Renderer/ExposurePass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/AutoExposure.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

// The Exposure program (Passes/Exposure.slang) runs the steps of ExposurePass.h; its settings are push constants
// (ExposureConstants), so one pipeline per entry serves every view. CSHistogram's absent bloom chain is replaced in its
// binding set by SceneColor, which the shader then never samples (ExposureFlagBloom is clear), as TonemapPass does.

namespace Engine {

	static_assert(ExposureHistogramBinCount == ExposureHistogramBins, "the shared histogram has AutoExposure.h's bins");

	namespace Utils {

		constexpr std::string_view ExposureProgram = "Exposure";
		// CSHistogram's [numthreads(16, 16, 1)].
		constexpr uint32_t ExposureHistogramGroupSize = 16;
		// Set 0 of Passes/Exposure.slang: the push constants (slot 0), t0 SceneColor, t1 Bloom, s0 LinearClamp, u0 Histogram,
		// u1 the state.
		constexpr uint32_t ExposurePushConstantsSlot = 0;
		constexpr uint32_t ExposureSceneColorSlot = 0;
		constexpr uint32_t ExposureBloomSlot = 1;
		constexpr uint32_t ExposureHistogramSlot = 0;
		constexpr uint32_t ExposureStateSlot = 1;

		static nvrhi::BindingLayoutDesc MakeExposureLayout(std::vector<nvrhi::BindingLayoutItem> items)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = std::move(items);
			return layout;
		}

		static PipelineLayoutDescription MakeHistogramDescription()
		{
			return {
				.Name = "ExposureHistogram",
				.Program = std::string(ExposureProgram),
				.Entries = { "CSHistogram" },
				.BindingLayouts = { MakeExposureLayout({
					nvrhi::BindingLayoutItem::PushConstants(ExposurePushConstantsSlot, sizeof(ExposureConstants)),
					nvrhi::BindingLayoutItem::Texture_SRV(ExposureSceneColorSlot),
					nvrhi::BindingLayoutItem::Texture_SRV(ExposureBloomSlot),
					nvrhi::BindingLayoutItem::Sampler(0),
					nvrhi::BindingLayoutItem::StructuredBuffer_UAV(ExposureHistogramSlot),
				}) },
			};
		}

		static PipelineLayoutDescription MakeAdaptDescription()
		{
			return {
				.Name = "ExposureAdapt",
				.Program = std::string(ExposureProgram),
				.Entries = { "CSAdapt" },
				.BindingLayouts = { MakeExposureLayout({
					nvrhi::BindingLayoutItem::PushConstants(ExposurePushConstantsSlot, sizeof(ExposureConstants)),
					nvrhi::BindingLayoutItem::StructuredBuffer_UAV(ExposureHistogramSlot),
					nvrhi::BindingLayoutItem::StructuredBuffer_UAV(ExposureStateSlot),
				}) },
			};
		}

		// `value` when finite, else `fallback`.
		static float FiniteOr(float value, float fallback)
		{
			return std::isfinite(value) ? value : fallback;
		}

		// A speed or a duration the adaptation multiplies: a negative or non-finite one counts as 0.
		static float NonNegativeOrZero(float value)
		{
			return std::isfinite(value) && value > 0.0f ? value : 0.0f;
		}

	}

	ExposureConstants MakeExposureConstants(const ExposurePassInputs& inputs, uint32_t width, uint32_t height)
	{
		const PostProcessSettings defaults;
		ExposureConstants constants;
		constants.ImageSize = UInt2(width, height);
		constants.InverseImageSize = 1.0f / glm::vec2(static_cast<float>(std::max(width, 1U)), static_cast<float>(std::max(height, 1U)));
		const float bloomIntensity = std::isfinite(inputs.BloomIntensity) ? std::clamp(inputs.BloomIntensity, 0.0f, 1.0f) : 0.0f;
		if (inputs.Reset)
			constants.Flags |= ExposureFlagReset;
		if (inputs.Bloom != nullptr && bloomIntensity > 0.0f)
		{
			constants.Flags |= ExposureFlagBloom;
			constants.BloomIntensity = bloomIntensity;
		}
		constants.MinEV = Utils::FiniteOr(inputs.MinEV, defaults.AutoExposureMinEV);
		constants.MaxEV = Utils::FiniteOr(inputs.MaxEV, defaults.AutoExposureMaxEV);
		constants.SpeedUp = Utils::NonNegativeOrZero(inputs.SpeedUp);
		constants.SpeedDown = Utils::NonNegativeOrZero(inputs.SpeedDown);
		constants.DeltaTime = Utils::NonNegativeOrZero(inputs.DeltaTime);
		return constants;
	}

	struct ExposurePass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Histogram{};
		ComputePipeline Adapt{};
		nvrhi::SamplerHandle LinearClamp{};
		uint32_t PipelineCount = 0;
	};

	ExposurePass::ExposurePass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ExposurePass::~ExposurePass() = default;

	Result<Scope<ExposurePass>> ExposurePass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<ExposurePass> pass = CreateScope<ExposurePass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Histogram, pipelines.CreateComputePipeline({ .Layout = Utils::MakeHistogramDescription() }));
		ENGINE_TRY_ASSIGN(state.Adapt, pipelines.CreateComputePipeline({ .Layout = Utils::MakeAdaptDescription() }));
		ENGINE_TRY_ASSIGN(state.LinearClamp,
			device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)));
		state.PipelineCount = PipelineCount;
		return pass;
	}

	std::vector<PipelineLayoutDescription> ExposurePass::GetLayoutDescriptions()
	{
		return { Utils::MakeHistogramDescription(), Utils::MakeAdaptDescription() };
	}

	nvrhi::BufferDesc ExposurePass::GetHistogramDesc()
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = static_cast<uint64_t>(ExposureHistogramBinCount) * sizeof(uint32_t);
		desc.structStride = sizeof(uint32_t);
		desc.canHaveUAVs = true;
		desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.ExposureHistogram";
		return desc;
	}

	nvrhi::BufferDesc ExposurePass::GetStateDesc()
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = sizeof(ExposureState);
		desc.structStride = sizeof(ExposureState);
		desc.canHaveUAVs = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.ExposureState";
		return desc;
	}

	uint32_t ExposurePass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status ExposurePass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ExposurePassInputs& inputs,
		RenderPassCounters& counters)
	{
		if (inputs.SceneColor == nullptr || inputs.Histogram == nullptr || inputs.State == nullptr
			|| inputs.Histogram->getDesc().byteSize < GetHistogramDesc().byteSize || inputs.State->getDesc().byteSize < GetStateDesc().byteSize)
			return MakeError(ErrorCode::InvalidArgument, "the Exposure pass needs SceneColor, a histogram buffer and a state buffer of their descriptions' sizes");
		State& state = *m_State;
		const nvrhi::TextureDesc& image = inputs.SceneColor->getDesc();
		const ExposureConstants constants = MakeExposureConstants(inputs, image.width, image.height);

		// The bloom chain is read at its mip 0 only, where BloomPass leaves the bloom.
		const bool bloom = (constants.Flags & ExposureFlagBloom) != 0;
		nvrhi::ITexture* bloomTexture = bloom ? inputs.Bloom : inputs.SceneColor;
		const nvrhi::TextureSubresourceSet bloomSubresources = bloom ? nvrhi::TextureSubresourceSet(0, 1, 0, 1) : nvrhi::AllSubresources;
		nvrhi::BindingSetDesc histogramDesc;
		histogramDesc.bindings = {
			nvrhi::BindingSetItem::PushConstants(Utils::ExposurePushConstantsSlot, sizeof(ExposureConstants)),
			nvrhi::BindingSetItem::Texture_SRV(Utils::ExposureSceneColorSlot, inputs.SceneColor),
			nvrhi::BindingSetItem::Texture_SRV(Utils::ExposureBloomSlot, bloomTexture, nvrhi::Format::UNKNOWN, bloomSubresources),
			nvrhi::BindingSetItem::Sampler(0, state.LinearClamp),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(Utils::ExposureHistogramSlot, inputs.Histogram),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * histogramSet, bindings.GetOrCreate(*state.Device, histogramDesc, *state.Histogram.BindingLayouts[0]));
		nvrhi::BindingSetDesc adaptDesc;
		adaptDesc.bindings = {
			nvrhi::BindingSetItem::PushConstants(Utils::ExposurePushConstantsSlot, sizeof(ExposureConstants)),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(Utils::ExposureHistogramSlot, inputs.Histogram),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(Utils::ExposureStateSlot, inputs.State),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * adaptSet, bindings.GetOrCreate(*state.Device, adaptDesc, *state.Adapt.BindingLayouts[0]));

		commandList.beginMarker("Exposure");
		commandList.clearBufferUInt(inputs.Histogram, 0);
		nvrhi::ComputeState histogram;
		histogram.pipeline = state.Histogram.Pipeline;
		histogram.bindings = { histogramSet };
		commandList.setComputeState(histogram);
		commandList.setPushConstants(&constants, sizeof(constants));
		commandList.dispatch((image.width + Utils::ExposureHistogramGroupSize - 1) / Utils::ExposureHistogramGroupSize,
			(image.height + Utils::ExposureHistogramGroupSize - 1) / Utils::ExposureHistogramGroupSize);
		nvrhi::ComputeState adapt;
		adapt.pipeline = state.Adapt.Pipeline;
		adapt.bindings = { adaptSet };
		commandList.setComputeState(adapt);
		commandList.setPushConstants(&constants, sizeof(constants));
		commandList.dispatch(1, 1);
		commandList.endMarker();
		counters.Dispatches += 2;
		return {};
	}

}
