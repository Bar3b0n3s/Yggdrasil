#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderStats.h"
#include "Shared/SkinningConstants.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The Skinning pass (M23, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27): the Deform stage's one
// pass, the first GPU work of a frame. One compute pipeline of the program Skinning (Passes/Skinning.slang, sorted into
// Shaders.json by stream C) deforms one item's vertices per dispatch, one thread per vertex, by Asset/MeshDeform.h's rule
// (morph, then skin; the CPU reference DeformMeshVertices, which the pass equals within the tolerance its GPU case states),
// into the item's own vertex buffer of MeshVertex layout, which every later pass binds as vertex binding 0 instead of the
// mesh's (the attribute stream, binding 1, is the mesh's own), so the 24 mesh pipelines stay and no vertex layout changes.
// Before it writes a vertex, each thread copies the vertex's position from the previous deformation into the previous
// positions (or, on the first deformation, the new position): the skinned previous positions M26's motion vectors read.
//
// The pass is created by SceneRendererPipelines::CreatePipelines (the placement rule's place 1, ADR 0029 decision 27); each
// deformed item's vertices, previous positions, palette and morph weights are each SceneRenderer's (place 2:
// SceneRenderer::State::DeformedMeshes), created by its Render and dropped with its state by a device loss's release; the
// mesh's skin and morph buffers are the GpuResourceCache's (place 3: GpuMesh::SkinBuffer and MorphBuffer). Main thread; no
// borrowed pointer survives Record. Frozen by the M23 contract C23.3; stream C implements it.

namespace Engine {

	class GraphicsDevice;

	// The code of the Warn line a SceneRenderer logs once (SceneRenderer::State::SkinMismatchLogged, kept across a device
	// loss's release and recreation) when a skinned item's JointPalette is shorter than its mesh's skin, so it draws its bind
	// pose (decision 27): the renderer's side of the AnimationSystem's ANIMATION_SKELETON_MISMATCH.
	inline constexpr std::string_view RenderSkinMismatchCode = "RENDER_SKIN_MISMATCH";

	// What one Record reads and writes, borrowed for the call.
	struct SkinningPassInputs
	{
		nvrhi::IBuffer* SourceVertices = nullptr;    // the mesh's vertices (GpuMesh::VertexBuffer), VertexCount MeshVertex
		nvrhi::IBuffer* Skin = nullptr;              // GpuMesh::SkinBuffer, or null when SkinJointCount is 0
		nvrhi::IBuffer* Morphs = nullptr;            // GpuMesh::MorphBuffer, or null when MorphTargetCount is 0
		std::span<const glm::mat4> Palette{};        // MeshDrawItem::JointPalette: at least SkinJointCount matrices with a skin
		std::span<const float> MorphWeights{};       // MorphTargetCount weights: ResolveMorphWeights of the item's MorphWeights
		nvrhi::IBuffer* PaletteBuffer = nullptr;     // GetPaletteDesc of at least SkinJointCount joints: Record writes Palette
		nvrhi::IBuffer* MorphWeightBuffer = nullptr; // GetMorphWeightsDesc: Record writes MorphWeights
		nvrhi::IBuffer* Vertices = nullptr;          // GetVerticesDesc of at least VertexCount: the deformed vertices
		nvrhi::IBuffer* PreviousPositions = nullptr; // GetPreviousPositionsDesc of at least VertexCount
		uint32_t VertexCount = 0;
		uint32_t SkinJointCount = 0;   // 0: no skinning
		uint32_t MorphTargetCount = 0; // 0: no morphing
		bool FirstDeform = false;      // SkinningFlagFirstDeform: the previous positions take the new positions
	};

	class SkinningPass
	{
	public:
		static constexpr uint32_t PipelineCount = 1;
		// The threads of one group of the pass's dispatch, along x (one per vertex).
		static constexpr uint32_t ThreadGroupSize = 64;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SkinningPass;
		};

		explicit SkinningPass(ConstructionKey key);
		~SkinningPass();

		SkinningPass(const SkinningPass&) = delete;
		SkinningPass& operator=(const SkinningPass&) = delete;

		// Creates the pass's pipeline through `pipelines`, its layout checked against the reflection. `device` is a documented
		// back-reference that outlives the pass. Errors: those of PipelineFactory (a Gpu error is an out-of-memory creation).
		[[nodiscard]] static Result<Scope<SkinningPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		// The layout of the pass's one pipeline: set 0 with the push constants (SkinningConstants), t0 the source vertices, t1
		// the skin, t2 the morph deltas, t3 the palette, t4 the morph weights, u0 the deformed vertices and u1 the previous
		// positions.
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		// The deformed vertices of `vertexCount` vertices (at least one): a vertex buffer of MeshVertex (48 bytes each) that is
		// also a raw UAV, kept in VertexBuffer between command lists.
		[[nodiscard]] static nvrhi::BufferDesc GetVerticesDesc(uint32_t vertexCount);
		// The previous positions of `vertexCount` vertices (at least one): a structured buffer of float4 (w 1) with UAVs, kept in
		// ShaderResource between command lists, which M26's motion vectors read.
		[[nodiscard]] static nvrhi::BufferDesc GetPreviousPositionsDesc(uint32_t vertexCount);
		// The palette of `jointCount` joints (at least one): a structured buffer of float4x4, kept in ShaderResource.
		[[nodiscard]] static nvrhi::BufferDesc GetPaletteDesc(uint32_t jointCount);
		// The morph weights: a structured buffer of MaxMeshMorphTargets floats, kept in ShaderResource.
		[[nodiscard]] static nvrhi::BufferDesc GetMorphWeightsDesc();

		// Records the Skinning row's dispatch of one item: writes inputs.Palette and inputs.MorphWeights into their buffers,
		// then deforms inputs.VertexCount vertices into inputs.Vertices and inputs.PreviousPositions. The caller resolves the
		// item's weights (the Prepare stage: ResolveMorphWeights against GpuMesh::DeformBounds.DefaultWeights), so the pass
		// applies exactly what the CPU reference applies. Errors, with nothing recorded: InvalidArgument for a missing buffer,
		// a vertex count of 0, a palette shorter than SkinJointCount, a morph target count above MaxMeshMorphTargets, morph
		// weights not exactly MorphTargetCount, or nothing to deform (SkinJointCount and MorphTargetCount both 0); those of the
		// binding set's creation (Gpu). Main thread; the command list is open.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
			const SkinningPassInputs& inputs);
	private:
		struct State;
		Scope<State> m_State;
	};

}
