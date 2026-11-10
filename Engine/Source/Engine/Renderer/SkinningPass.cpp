#include "EnginePCH.h"
#include "Engine/Renderer/SkinningPass.h"

#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <algorithm>
#include <utility>

// The Skinning pass (SkinningPass.h): its pipeline, buffer descriptions and the items' dispatches. The source vertices, the
// skin and the morph deltas are raw buffers (their elements have no std430 layout), the palette and the morph weights
// structured buffers; a mesh without a skin or without morph targets binds its source vertices in that slot instead, which
// the shader then never reads (its count is 0).

namespace Engine {

	namespace Utils {

		// Checks one item's inputs and finds or creates its binding set for `pipeline`, recording nothing. Errors: Record's.
		[[nodiscard]] static Result<nvrhi::IBindingSet*> PrepareSkinningItem(GraphicsDevice& device, const ComputePipeline& pipeline, PassBindingCache& bindings,
			const SkinningPassInputs& inputs)
		{
			const uint64_t vertexCount = inputs.VertexCount;
			const auto isShorter = [](const nvrhi::IBuffer* buffer, uint64_t bytes)
			{
				return buffer == nullptr || buffer->getDesc().byteSize < bytes;
			};
			if (inputs.SourceVertices == nullptr || inputs.PaletteBuffer == nullptr || inputs.MorphWeightBuffer == nullptr || inputs.Vertices == nullptr
				|| inputs.PreviousPositions == nullptr)
				return MakeError(ErrorCode::InvalidArgument, "the Skinning pass needs the source vertices, the palette and morph weight buffers and the output buffers");
			if (inputs.VertexCount == 0)
				return MakeError(ErrorCode::InvalidArgument, "the Skinning pass has no vertex to deform");
			if (inputs.SkinJointCount == 0 && inputs.MorphTargetCount == 0)
				return MakeError(ErrorCode::InvalidArgument, "the Skinning pass has nothing to deform: no skin joint and no morph target");
			if (inputs.SkinJointCount > MaxMeshSkinJoints || inputs.Palette.size() < inputs.SkinJointCount)
			{
				return MakeError(ErrorCode::InvalidArgument, "the Skinning pass's palette holds {} matrices for a skin of {} joints (at most {})", inputs.Palette.size(),
					inputs.SkinJointCount, MaxMeshSkinJoints);
			}
			if (inputs.MorphTargetCount > MaxMeshMorphTargets || inputs.MorphWeights.size() != inputs.MorphTargetCount)
			{
				return MakeError(ErrorCode::InvalidArgument, "the Skinning pass has {} morph weights for {} morph targets (at most {})", inputs.MorphWeights.size(),
					inputs.MorphTargetCount, MaxMeshMorphTargets);
			}
			if (isShorter(inputs.SourceVertices, vertexCount * sizeof(MeshVertex)) || isShorter(inputs.Vertices, vertexCount * sizeof(MeshVertex))
				|| isShorter(inputs.PreviousPositions, vertexCount * sizeof(glm::vec4))
				|| (inputs.SkinJointCount > 0
					&& (isShorter(inputs.Skin, vertexCount * sizeof(MeshSkinVertex)) || isShorter(inputs.PaletteBuffer, inputs.SkinJointCount * sizeof(glm::mat4))))
				|| (inputs.MorphTargetCount > 0 && isShorter(inputs.Morphs, inputs.MorphTargetCount * vertexCount * sizeof(MeshMorphDelta)))
				|| isShorter(inputs.MorphWeightBuffer, SkinningPass::GetMorphWeightsDesc().byteSize))
			{
				return MakeError(ErrorCode::InvalidArgument, "a buffer of the Skinning pass is missing or too small for the item's {} vertices", vertexCount);
			}

			// A slot the item does not read binds the source vertices, a valid raw buffer.
			nvrhi::IBuffer* skin = inputs.SkinJointCount > 0 ? inputs.Skin : inputs.SourceVertices;
			nvrhi::IBuffer* morphs = inputs.MorphTargetCount > 0 ? inputs.Morphs : inputs.SourceVertices;
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::PushConstants(0, sizeof(SkinningConstants)),
				nvrhi::BindingSetItem::RawBuffer_SRV(0, inputs.SourceVertices),
				nvrhi::BindingSetItem::RawBuffer_SRV(1, skin),
				nvrhi::BindingSetItem::RawBuffer_SRV(2, morphs),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(3, inputs.PaletteBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(4, inputs.MorphWeightBuffer),
				nvrhi::BindingSetItem::RawBuffer_UAV(0, inputs.Vertices),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(1, inputs.PreviousPositions),
			};
			return bindings.GetOrCreate(device, desc, *pipeline.BindingLayouts.front());
		}

		// Records one checked item's palette and morph weight writes and its dispatch with `set`.
		static void RecordSkinningDispatch(nvrhi::ICommandList& commandList, const ComputePipeline& pipeline, const SkinningPassInputs& inputs, nvrhi::IBindingSet& set)
		{
			if (inputs.SkinJointCount > 0)
				commandList.writeBuffer(inputs.PaletteBuffer, inputs.Palette.data(), static_cast<size_t>(inputs.SkinJointCount) * sizeof(glm::mat4));
			if (inputs.MorphTargetCount > 0)
				commandList.writeBuffer(inputs.MorphWeightBuffer, inputs.MorphWeights.data(), static_cast<size_t>(inputs.MorphTargetCount) * sizeof(float));
			nvrhi::ComputeState compute;
			compute.pipeline = pipeline.Pipeline;
			compute.bindings = { &set };
			commandList.setComputeState(compute);
			SkinningConstants constants{};
			constants.Counts = UInt4(inputs.VertexCount, inputs.SkinJointCount, inputs.MorphTargetCount, inputs.FirstDeform ? SkinningFlagFirstDeform : 0U);
			commandList.setPushConstants(&constants, sizeof(constants));
			// Rows of at most SkinningRowGroups groups along x, as many rows along y as the vertices need (SkinningConstants.h).
			const uint64_t groups = (static_cast<uint64_t>(inputs.VertexCount) + SkinningPass::ThreadGroupSize - 1) / SkinningPass::ThreadGroupSize;
			const auto columns = static_cast<uint32_t>(std::min<uint64_t>(groups, SkinningRowGroups));
			const auto rows = static_cast<uint32_t>((groups + SkinningRowGroups - 1) / SkinningRowGroups);
			commandList.dispatch(columns, rows);
		}

	}

	struct SkinningPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
	};

	SkinningPass::SkinningPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SkinningPass::~SkinningPass() = default;

	Result<Scope<SkinningPass>> SkinningPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<SkinningPass> pass = CreateScope<SkinningPass>(ConstructionKey());
		pass->m_State->Device = &device;
		std::vector<PipelineLayoutDescription> descriptions = GetLayoutDescriptions();
		ENGINE_TRY_ASSIGN(pass->m_State->Pipeline, pipelines.CreateComputePipeline({ .Layout = std::move(descriptions.front()) }));
		return pass;
	}

	std::vector<PipelineLayoutDescription> SkinningPass::GetLayoutDescriptions()
	{
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::Compute;
		layout.registerSpace = 0;
		layout.registerSpaceIsDescriptorSet = true;
		layout.bindings = {
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkinningConstants)),
			nvrhi::BindingLayoutItem::RawBuffer_SRV(0),
			nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
			nvrhi::BindingLayoutItem::RawBuffer_SRV(2),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
			nvrhi::BindingLayoutItem::RawBuffer_UAV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
		};
		return { {
			.Name = "Skinning",
			.Program = "Skinning",
			.Entries = { "CSMain" },
			.BindingLayouts = { layout },
		} };
	}

	nvrhi::BufferDesc SkinningPass::GetVerticesDesc(uint32_t vertexCount)
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = static_cast<uint64_t>(std::max(vertexCount, 1U)) * sizeof(MeshVertex);
		desc.isVertexBuffer = true;
		desc.canHaveUAVs = true;
		desc.canHaveRawViews = true;
		desc.initialState = nvrhi::ResourceStates::VertexBuffer;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.DeformedVertices";
		return desc;
	}

	nvrhi::BufferDesc SkinningPass::GetPreviousPositionsDesc(uint32_t vertexCount)
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = static_cast<uint64_t>(std::max(vertexCount, 1U)) * sizeof(glm::vec4);
		desc.structStride = sizeof(glm::vec4);
		desc.canHaveUAVs = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.PreviousPositions";
		return desc;
	}

	nvrhi::BufferDesc SkinningPass::GetPaletteDesc(uint32_t jointCount)
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = static_cast<uint64_t>(std::max(jointCount, 1U)) * sizeof(glm::mat4);
		desc.structStride = sizeof(glm::mat4);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.JointPalette";
		return desc;
	}

	nvrhi::BufferDesc SkinningPass::GetMorphWeightsDesc()
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = static_cast<uint64_t>(MaxMeshMorphTargets) * sizeof(float);
		desc.structStride = sizeof(float);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.MorphWeights";
		return desc;
	}

	std::vector<Status> SkinningPass::Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
		std::span<const SkinningPassInputs> items)
	{
		State& state = *m_State;
		std::vector<Status> results(items.size());
		std::vector<nvrhi::IBindingSet*> sets(items.size(), nullptr);
		uint32_t dispatches = 0;
		for (size_t index = 0; index < items.size(); ++index)
		{
			Result<nvrhi::IBindingSet*> set = Utils::PrepareSkinningItem(*state.Device, state.Pipeline, bindings, items[index]);
			if (!set.has_value())
			{
				results[index] = std::unexpected(std::move(set).error());
				continue;
			}
			sets[index] = *set;
			++dispatches;
		}
		if (dispatches == 0)
			return results;

		// One scope for the whole pass, however many items it deforms: GpuProfiler times at most MaxScopesPerFrame scopes a
		// frame, so a scope per dispatch would leave the later passes of a frame with many deformed items untimed.
		recording.BeginPass("Skinning", &commandList);
		for (size_t index = 0; index < items.size(); ++index)
		{
			if (sets[index] != nullptr)
				Utils::RecordSkinningDispatch(commandList, state.Pipeline, items[index], *sets[index]);
		}
		recording.EndPass({ .Dispatches = dispatches });
		return results;
	}

}
