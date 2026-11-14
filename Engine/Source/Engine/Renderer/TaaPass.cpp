#include "EnginePCH.h"
#include "Engine/Renderer/TaaPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

// The Taa program (Passes/Taa.slang) runs TaaPass.h's resolve; its settings are push constants (TaaConstants), so one
// pipeline serves every view. A reset's absent history is replaced in its binding set by SceneColor, which the shader then
// never samples (TaaFlagReset), as ExposurePass does with its bloom.

namespace Engine {

	namespace Utils {

		constexpr std::string_view TaaProgram = "Taa";
		// CSMain's [numthreads(8, 8, 1)].
		constexpr uint32_t TaaGroupSize = 8;
		// Set 0 of Passes/Taa.slang: the push constants (slot 0), t0 SceneColor, t1 SceneVelocity, t2 the history, s0
		// LinearClamp, u0 the resolved image.
		constexpr uint32_t TaaPushConstantsSlot = 0;
		constexpr uint32_t TaaSceneColorSlot = 0;
		constexpr uint32_t TaaVelocitySlot = 1;
		constexpr uint32_t TaaHistorySlot = 2;
		constexpr uint32_t TaaResolvedSlot = 0;

		static PipelineLayoutDescription MakeTaaDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::PushConstants(TaaPushConstantsSlot, sizeof(TaaConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(TaaSceneColorSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(TaaVelocitySlot),
				nvrhi::BindingLayoutItem::Texture_SRV(TaaHistorySlot),
				nvrhi::BindingLayoutItem::Sampler(0),
				nvrhi::BindingLayoutItem::Texture_UAV(TaaResolvedSlot),
			};
			return {
				.Name = "Taa",
				.Program = std::string(TaaProgram),
				.Entries = { "CSMain" },
				.BindingLayouts = { std::move(layout) },
				.StorageImages = { { .Set = 0, .Register = TaaResolvedSlot, .Format = TaaHistoryFormat } },
			};
		}

		static bool HasSize(const nvrhi::ITexture* texture, uint32_t width, uint32_t height)
		{
			return texture->getDesc().width == width && texture->getDesc().height == height;
		}

	}

	TaaConstants MakeTaaConstants(const TaaPassInputs& inputs, uint32_t width, uint32_t height)
	{
		TaaConstants constants;
		constants.ImageSize = UInt2(width, height);
		constants.InverseImageSize = 1.0f / glm::vec2(static_cast<float>(std::max(width, 1U)), static_cast<float>(std::max(height, 1U)));
		constants.Feedback = TaaFeedback;
		constants.Flags = inputs.Reset ? TaaFlagReset : 0U;
		return constants;
	}

	struct TaaPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Resolve{};
		nvrhi::SamplerHandle LinearClamp{};
		uint32_t PipelineCount = 0;
	};

	TaaPass::TaaPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	TaaPass::~TaaPass() = default;

	Result<Scope<TaaPass>> TaaPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<TaaPass> pass = CreateScope<TaaPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Resolve, pipelines.CreateComputePipeline({ .Layout = Utils::MakeTaaDescription() }));
		ENGINE_TRY_ASSIGN(state.LinearClamp,
			device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)));
		state.PipelineCount = PipelineCount;
		return pass;
	}

	std::vector<PipelineLayoutDescription> TaaPass::GetLayoutDescriptions()
	{
		return { Utils::MakeTaaDescription() };
	}

	nvrhi::TextureDesc TaaPass::GetHistoryDesc(uint32_t width, uint32_t height)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = TaaHistoryFormat;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.isShaderResource = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.TaaHistory";
		return desc;
	}

	uint32_t TaaPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status TaaPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TaaPassInputs& inputs, RenderPassCounters& counters)
	{
		if (inputs.SceneColor == nullptr || inputs.Velocity == nullptr || inputs.Resolved == nullptr || (!inputs.Reset && inputs.History == nullptr)
			|| inputs.History == inputs.Resolved)
			return MakeError(ErrorCode::InvalidArgument, "the Taa pass needs SceneColor, SceneVelocity, a resolved image and, without a reset, a history other than it");
		const nvrhi::TextureDesc& image = inputs.SceneColor->getDesc();
		if (!Utils::HasSize(inputs.Velocity, image.width, image.height) || !Utils::HasSize(inputs.Resolved, image.width, image.height)
			|| (!inputs.Reset && !Utils::HasSize(inputs.History, image.width, image.height)))
			return MakeError(ErrorCode::InvalidArgument, "the Taa pass's motion vectors, history and resolved image must have SceneColor's size {}x{}", image.width,
				image.height);
		State& state = *m_State;
		const TaaConstants constants = MakeTaaConstants(inputs, image.width, image.height);
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::PushConstants(Utils::TaaPushConstantsSlot, sizeof(TaaConstants)),
			nvrhi::BindingSetItem::Texture_SRV(Utils::TaaSceneColorSlot, inputs.SceneColor),
			nvrhi::BindingSetItem::Texture_SRV(Utils::TaaVelocitySlot, inputs.Velocity),
			nvrhi::BindingSetItem::Texture_SRV(Utils::TaaHistorySlot, inputs.Reset ? inputs.SceneColor : inputs.History),
			nvrhi::BindingSetItem::Sampler(0, state.LinearClamp),
			nvrhi::BindingSetItem::Texture_UAV(Utils::TaaResolvedSlot, inputs.Resolved),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *state.Resolve.BindingLayouts[0]));

		commandList.beginMarker("TAA");
		nvrhi::ComputeState compute;
		compute.pipeline = state.Resolve.Pipeline;
		compute.bindings = { set };
		commandList.setComputeState(compute);
		commandList.setPushConstants(&constants, sizeof(constants));
		commandList.dispatch((image.width + Utils::TaaGroupSize - 1) / Utils::TaaGroupSize, (image.height + Utils::TaaGroupSize - 1) / Utils::TaaGroupSize);
		commandList.copyTexture(inputs.SceneColor, nvrhi::TextureSlice(), inputs.Resolved, nvrhi::TextureSlice());
		commandList.endMarker();
		counters.Dispatches += 1;
		return {};
	}

}
