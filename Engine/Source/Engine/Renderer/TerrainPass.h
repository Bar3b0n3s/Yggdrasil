#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Shared/TerrainConstants.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

// The terrain passes (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 13, 22 and 29; Architecture §8.3,
// §8.5): the snapshot's Terrains drawn as LOD chunks (Renderer/TerrainLod.h) by three pipelines of the program Terrain
// (Passes/Terrain.slang): TerrainDepthNormal's prepass and picking prepass, recorded right after the mesh prepass into the
// DepthNormal stage's final targets (SceneDepth written; SceneNormals, SceneVelocity and SceneRoughness, and EntityId when
// picking, in SceneTargetFormats.h's attachment order), so GTAO, SSR and picking see the terrain; and TerrainForward, recorded
// after ForwardOpaque and before the skybox into SceneColor, depth-tested against the prepass without writing, with the mesh
// forward passes' set 0 (the lights, shadows, environment, GTAO and probes) as it is.
//   - Each frame (Prepare, from the DepthNormal stage): every snapshot terrain whose heightfield mirrors
//     (GpuResourceCache::GetHeightfield) with a finite world matrix, a positive finite Size and HeightScale, gets its
//     levels from SelectTerrainLods at the camera's position in its local space (the LodBias clamped to [0.25, 4], 1 when not
//     finite), each chunk's neighbours' levels (a missing neighbour as the chunk's own) and the chunks that meet the view
//     (IsOutsideView of the chunk's cells under every height of [0, HeightScale]); its TerrainConstants (MakeTerrainConstants)
//     are written into a constant buffer of the view's, and its set 1 (the constants, the heightfield, the splat map or the
//     White stand-in, and per layer its base colour map or White and its normal map or FlatNormal, through the cache) is
//     made. A terrain without layers draws one white layer (TerrainLayerItem's defaults).
//   - The chunk meshes: BuildTerrainChunkMesh of a chunk, less its first sample, is the same for every chunk of one size at
//     one level with one set of coarser neighbours, so the view keeps one vertex buffer (RG16_UINT chunk-local samples) and
//     one index buffer per (cells along X and Z, level, coarser sides) in TerrainViewResources::Meshes, made and uploaded the
//     first time a render draws it and released when a render draws it no more. A chunk draws its key's mesh with its first
//     sample in the push constants (TerrainDrawConstants), each terrain's chunks in their key's order and then row-major.
//   - The prepass writes the camera's motion alone to SceneVelocity (a terrain has no previous world: a moving terrain
//     ghosts under TAA, a kept limit, Backlog §6) and the forward pass's specular inputs to SceneRoughness; a terrain casts no
//     shadows (a kept limit) and is drawn neither by the Overdraw data view nor by the wireframe's edges, while its prepass
//     occludes them as it occludes everything behind it. The material debug views shade it with TerrainConstants::DebugView.
// Deterministic for a snapshot on a device: the levels, the chunk order and every value written are functions of the
// snapshot. Its GPU objects follow the placement rule (ADR 0029 decision 27): the pipelines and the binding layouts are the
// pass's, created by SceneRendererPipelines::CreatePipelines (place 1); the chunk meshes, the constant buffers and the
// binding sets are a SceneRenderer's TerrainViewResources (place 2), released with its state on a device loss and made again
// by the next render; the heightfields and the layers' textures are the cache's mirrors (place 3). Main thread only.

namespace Engine {

	class GpuResourceCache;
	class GraphicsDevice;

	// One chunk mesh of a view's library: the chunk's cells along local X and Z (TerrainChunkCells, or fewer for the last
	// chunks), its level and its coarser sides (bit i for TerrainNeighbourLods' side i: -X, +X, -Z, +Z).
	struct TerrainChunkMeshKey
	{
		uint32_t CellsX = 0;
		uint32_t CellsZ = 0;
		uint32_t Lod = 0;
		uint32_t Stitch = 0;

		std::strong_ordering operator<=>(const TerrainChunkMeshKey&) const = default;
		bool operator==(const TerrainChunkMeshKey&) const = default;
	};

	// A chunk mesh on the GPU: its chunk-local samples (RG16_UINT) and its triangle list (R32_UINT).
	struct TerrainChunkMeshBuffers
	{
		nvrhi::BufferHandle Vertices{};
		nvrhi::BufferHandle Indices{};
		uint32_t IndexCount = 0;
		bool IsUsed = false; // drawn by the current render
	};

	// One chunk the render draws: its mesh and its first sample.
	struct TerrainChunkDraw
	{
		TerrainChunkMeshKey Mesh{};
		glm::uvec2 Origin{ 0 };
	};

	// One terrain the render draws, prepared by TerrainPass::Prepare.
	struct TerrainDraw
	{
		size_t SnapshotIndex = 0;
		nvrhi::IBindingSet* Set = nullptr; // its set 1, held by TerrainViewResources::Bindings until the next Prepare
		std::vector<TerrainChunkDraw> Chunks{};
	};

	// A SceneRenderer's terrain state (place 2; see the file comment): its chunk meshes, its terrains' constant buffers (one
	// per terrain the render draws, grown and kept), their sets, and the render's prepared terrains.
	struct TerrainViewResources
	{
		std::map<TerrainChunkMeshKey, TerrainChunkMeshBuffers> Meshes{};
		std::vector<nvrhi::BufferHandle> Constants{};
		PassBindingCache Bindings{};
		std::vector<TerrainDraw> Draws{};
	};

	// One frame's inputs, borrowed for the call.
	struct TerrainPassInputs
	{
		const RenderSnapshot* Snapshot = nullptr; // never null
		const CameraData* Camera = nullptr;       // the frame's camera, its viewport at the scene resolution; never null
		RenderDebugView DebugView = RenderDebugView::Lit;
	};

	// The constants of `terrain` drawn with a heightfield of `resolution` samples a side in `debugView`
	// (Shared/TerrainConstants.h): its World, its first MaxTerrainShaderLayers layers (one white layer without any) with their
	// tints, the inverse of their Tiling, their Roughness and Metallic, and z 1 for the layers whose normal map `twoChannel`
	// flags (bit i for layer i), CellSize and InverseSize from Size, the flags (ReceiveShadows, `splatMap`, and a mirroring
	// World) and PickId. Pure.
	[[nodiscard]] TerrainConstants MakeTerrainConstants(const TerrainItem& terrain, uint32_t resolution, RenderDebugView debugView, bool splatMap,
		uint32_t twoChannel);

	class TerrainPass
	{
	public:
		// The prepass and the picking prepass (TerrainDepthNormal), and the forward pass (TerrainForward).
		static constexpr uint32_t DepthNormalPipelineCount = 2;
		static constexpr uint32_t ForwardPipelineCount = 1;
		static constexpr uint32_t PipelineCount = DepthNormalPipelineCount + ForwardPipelineCount;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class TerrainPass;
		};

		// Use Create.
		explicit TerrainPass(ConstructionKey key);
		~TerrainPass();

		TerrainPass(const TerrainPass&) = delete;
		TerrainPass& operator=(const TerrainPass&) = delete;

		// Creates the three pipelines (checked against their reflection) and their binding layouts: the prepass's set 0 and the
		// terrains' set 1, which both passes share, and for the forward pass's set 0 `forwardViewLayout`, the mesh forward
		// passes' (Private/ForwardPipelines.h), so a forward set of the view serves both. `device` is a documented
		// back-reference that outlives the pass. Errors: those of PipelineFactory (a Gpu error is an out-of-memory creation,
		// FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<TerrainPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines, nvrhi::IBindingLayout& forwardViewLayout);

		// The layout descriptions of the three pipelines, the prepass's, the picking prepass's, then the forward pass's, for
		// "Shaders: LayoutsMatchReflection": the prepass's set 0 holds b0 ViewConstants, t5 the DFG LUT, s0 LinearClamp, s2
		// AnisoWrap and the push constants (at the mesh pipelines' slot); the forward pass's set 0 is the mesh forward
		// passes'; set 1 holds b0 TerrainConstants, t0 the heights, t1 the splat map, t2 to t5 the layers' base colour maps
		// and t6 to t9 their normal maps.
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();

		// The pipelines Create made (PipelineCount).
		[[nodiscard]] uint32_t GetPipelineCount() const;
		// The prepass's set-0 layout, which a view's prepass set is made for.
		[[nodiscard]] nvrhi::IBindingLayout& GetPrepassViewLayout() const;

		// Prepares the render's terrains into resources.Draws (see the file comment), recording the new chunk meshes' and the
		// terrains' constants' uploads into `commandList` (open). The first error is returned and the render goes on: a
		// terrain whose values cannot be drawn (a non-finite World, a Size or HeightScale not positive and finite) is left
		// out (InvalidArgument, naming its entity); a buffer or a set that cannot be created leaves out its terrain or chunk
		// (Gpu). Main thread.
		[[nodiscard]] Status Prepare(nvrhi::ICommandList& commandList, GpuResourceCache& cache, TerrainViewResources& resources, const TerrainPassInputs& inputs);

		// Records resources.Draws' chunks into `framebuffer` (the prepass's, or with `picking` the picking prepass's) with the
		// view's prepass set `viewSet` (made for GetPrepassViewLayout), adding their draws and triangles to `counters`.
		void RecordDepthNormal(nvrhi::ICommandList& commandList, const TerrainViewResources& resources, nvrhi::IFramebuffer& framebuffer, bool picking,
			nvrhi::IBindingSet& viewSet, RenderPassCounters& counters) const;
		// Records resources.Draws' chunks into `framebuffer` (SceneColor with SceneDepth) with the view's forward set
		// `forwardSet` (made for the mesh forward passes' set-0 layout), adding to `counters`.
		void RecordForward(nvrhi::ICommandList& commandList, const TerrainViewResources& resources, nvrhi::IFramebuffer& framebuffer,
			nvrhi::IBindingSet& forwardSet, RenderPassCounters& counters) const;

		// Drops the chunk meshes and the sets the last render did not use and forgets its terrains, ending the render.
		static void ReleaseUnused(TerrainViewResources& resources);
	private:
		// The back-reference, the pipelines and the layouts (TerrainPass.cpp).
		struct State;
		Scope<State> m_State;
	};

}
