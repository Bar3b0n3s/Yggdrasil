#include "EnginePCH.h"
#include "Engine/Renderer/TonemapPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ViewConstants.h"

#include <string>
#include <string_view>

// The walking skeleton's tonemap (exposure, Linear, the sRGB OETF), moved here from SceneRenderer.cpp by the M8 contract
// unchanged in behaviour, so the scene renderer keeps its output while stream C implements the pass (TonemapPass.h). The
// tonemappers, the bloom input, the dither and EncodeSrgb are not applied yet.

namespace Engine {

	namespace Utils {

		// The Tonemap program's [numthreads(8, 8, 1)].
		constexpr uint32_t TonemapGroupSize = 8;
		constexpr std::string_view TonemapProgram = "Tonemap";

		static PipelineLayoutDescription MakeTonemapDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::ConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(0),
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

	}

	struct TonemapPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
		uint32_t PipelineCount = 0;
	};

	TonemapPass::TonemapPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	TonemapPass::~TonemapPass() = default;

	Result<Scope<TonemapPass>> TonemapPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		ENGINE_CONTRACT_STUB();
		Scope<TonemapPass> pass = CreateScope<TonemapPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateComputePipeline({ .Layout = Utils::MakeTonemapDescription(), .Specializations = {} }));
		state.PipelineCount = 1;
		return pass;
	}

	std::vector<PipelineLayoutDescription> TonemapPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return { Utils::MakeTonemapDescription() };
	}

	uint32_t TonemapPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status TonemapPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TonemapPassInputs& inputs)
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(inputs.ViewConstants != nullptr && inputs.SceneColor != nullptr && inputs.LdrColor != nullptr,
			"TonemapPass::Record needs ViewConstants, SceneColor and LdrColor");
		const nvrhi::TextureDesc& target = inputs.LdrColor->getDesc();
		ENGINE_CORE_ASSERT(inputs.SceneColor->getDesc().width == target.width && inputs.SceneColor->getDesc().height == target.height,
			"TonemapPass::Record needs SceneColor and LdrColor of the same size");
		State& state = *m_State;

		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, inputs.ViewConstants),
			nvrhi::BindingSetItem::Texture_SRV(0, inputs.SceneColor),
			nvrhi::BindingSetItem::Texture_UAV(0, inputs.LdrColor, LdrColorFormat),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *state.Pipeline.BindingLayouts[0]));
		nvrhi::ComputeState compute;
		compute.pipeline = state.Pipeline.Pipeline;
		compute.bindings = { set };

		commandList.beginMarker("Tonemap");
		commandList.setComputeState(compute);
		commandList.dispatch((target.width + Utils::TonemapGroupSize - 1) / Utils::TonemapGroupSize,
			(target.height + Utils::TonemapGroupSize - 1) / Utils::TonemapGroupSize);
		commandList.endMarker();
		return {};
	}

}
