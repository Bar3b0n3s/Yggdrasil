#include "EnginePCH.h"
#include "Engine/Renderer/LightClusterPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Shared/ViewConstants.h"

#include <algorithm>
#include <utility>

namespace Engine {

	struct LightClusterPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
	};

	uint32_t GetLightClusterCount(const LightClusterGrid& grid)
	{
		return grid.TilesX * grid.TilesY * grid.Slices;
	}

	LightClusterConstants MakeLightClusterConstants(const LightClusterGrid& grid, uint32_t mode)
	{
		LightClusterConstants constants{};
		constants.Grid = UInt4(grid.TilesX, grid.TilesY, grid.Slices, grid.WordCount);
		constants.Slicing = Float4(grid.Near, grid.Far, grid.SliceScale, grid.SliceBias);
		constants.Mode = UInt4(mode, 0, 0, 0);
		return constants;
	}

	LightClusterPass::LightClusterPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	LightClusterPass::~LightClusterPass() = default;

	Result<Scope<LightClusterPass>> LightClusterPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<LightClusterPass> pass = CreateScope<LightClusterPass>(ConstructionKey());
		pass->m_State->Device = &device;
		std::vector<PipelineLayoutDescription> descriptions = GetLayoutDescriptions();
		ENGINE_TRY_ASSIGN(pass->m_State->Pipeline, pipelines.CreateComputePipeline({ .Layout = std::move(descriptions.front()) }));
		return pass;
	}

	std::vector<PipelineLayoutDescription> LightClusterPass::GetLayoutDescriptions()
	{
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::Compute;
		layout.registerSpace = 0;
		layout.registerSpaceIsDescriptorSet = true;
		layout.bindings = {
			nvrhi::BindingLayoutItem::ConstantBuffer(0),
			nvrhi::BindingLayoutItem::ConstantBuffer(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
		};
		return { {
			.Name = "LightClusters",
			.Program = "LightClusters",
			.Entries = { "CSMain" },
			.BindingLayouts = { layout },
			.ConstantBuffers = {
				{ .Set = 0, .Register = 0, .ByteSize = sizeof(ViewConstants) },
				{ .Set = 0, .Register = 1, .ByteSize = sizeof(LightClusterConstants) },
			},
		} };
	}

	nvrhi::BufferDesc LightClusterPass::GetConstantsDesc()
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = sizeof(LightClusterConstants);
		desc.isConstantBuffer = true;
		desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.LightClusterConstants";
		return desc;
	}

	nvrhi::BufferDesc LightClusterPass::GetMaskBufferDesc(uint32_t clusterCount, uint32_t wordCount)
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = static_cast<uint64_t>(std::max(clusterCount, 1U)) * std::max(wordCount, 1U) * sizeof(uint32_t);
		desc.structStride = sizeof(uint32_t);
		desc.canHaveUAVs = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.LightClusterMasks";
		return desc;
	}

	Result<nvrhi::IBindingSet*> LightClusterPass::PrepareSet(PassBindingCache& bindings, const LightClusterPassInputs& inputs) const
	{
		const uint32_t clusterCount = GetLightClusterCount(inputs.Grid);
		if (inputs.ViewConstants == nullptr || inputs.ClusterConstants == nullptr || inputs.Lights == nullptr || inputs.Masks == nullptr
			|| clusterCount == 0 || inputs.Grid.WordCount == 0 || inputs.Grid.WordCount > MaxLightClusterWordCount
			|| inputs.Masks->getDesc().byteSize < GetMaskBufferDesc(clusterCount, inputs.Grid.WordCount).byteSize)
		{
			return MakeError(ErrorCode::InvalidArgument,
				"the LightClusters pass needs its buffers, a grid of 1 to {} mask words per cluster and a mask buffer for the {} clusters of its grid",
				MaxLightClusterWordCount, clusterCount);
		}
		const State& state = *m_State;
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, inputs.ViewConstants),
			nvrhi::BindingSetItem::ConstantBuffer(1, inputs.ClusterConstants),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(0, inputs.Lights),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, inputs.Masks),
		};
		return bindings.GetOrCreate(*state.Device, desc, *state.Pipeline.BindingLayouts.front());
	}

	void LightClusterPass::RecordDispatch(nvrhi::ICommandList& commandList, nvrhi::IBindingSet& set, const LightClusterPassInputs& inputs,
		RenderPassCounters& counters) const
	{
		const State& state = *m_State;
		const LightClusterConstants constants = MakeLightClusterConstants(inputs.Grid, LightClusterModeClustered);
		commandList.writeBuffer(inputs.ClusterConstants, &constants, sizeof(constants));
		nvrhi::ComputeState compute;
		compute.pipeline = state.Pipeline.Pipeline;
		compute.bindings = { &set };
		commandList.setComputeState(compute);
		// x: every tile's words of one slice (Passes/LightClusters.slang), y: the slices.
		const uint32_t threads = inputs.Grid.TilesX * inputs.Grid.TilesY * inputs.Grid.WordCount;
		commandList.dispatch((threads + ThreadGroupSize - 1) / ThreadGroupSize, inputs.Grid.Slices);
		++counters.Dispatches;
	}

	Status LightClusterPass::Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
		const LightClusterPassInputs& inputs)
	{
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, PrepareSet(bindings, inputs));
		recording.BeginPass("LightClusters", &commandList);
		RenderPassCounters counters;
		RecordDispatch(commandList, *set, inputs, counters);
		recording.EndPass(counters);
		return {};
	}

	Status LightClusterPass::RecordInRow(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const LightClusterPassInputs& inputs,
		RenderPassCounters& counters) const
	{
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, PrepareSet(bindings, inputs));
		RecordDispatch(commandList, *set, inputs, counters);
		return {};
	}

}
