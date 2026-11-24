#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SkinningPass.h"
#include "Shared/InstanceRecord.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Engine {

	class GpuResourceCache;
	class GraphicsDevice;

	// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): one submesh draw of the selection mask, resolved
	// once per render by SelectionPass::PrepareDraws. It holds its mesh's buffers and a Mask material's objects for the render.
	struct SelectionDraw
	{
		nvrhi::BufferHandle Vertices{};
		nvrhi::BufferHandle Attributes{}; // M23 (C-MESH2): vertex binding 1, the mesh's attribute stream (GpuMesh::AttributeBuffer)
		nvrhi::BufferHandle Wind{};       // M46: vertex binding 3, the mesh's wind stream (GpuMesh::WindBuffer)
		nvrhi::BufferHandle Indices{};
		uint32_t IndexOffset = 0;
		uint32_t IndexCount = 0;
		uint32_t Variant = 0; // {Opaque or Blend, Mask} x {CullBack, CullFront, CullNone}
		// A Mask material's alpha test (its constants and base colour map); null otherwise (Blend uses geometric coverage).
		nvrhi::BufferHandle MaterialConstants{};
		nvrhi::TextureHandle BaseColorMap{};
		uint32_t Instance = 0; // its instance record's index in the view's Instances buffer
	};

	// The selection mask's draws (SelectionPass::PrepareDraws).
	struct SelectionDrawSet
	{
		std::vector<SelectionDraw> Draws{};
		// The instanced draws: maximal runs of adjacent Draws with equal keys (GroupMeshInstances), or one per draw without
		// instancing (RenderQualitySettings::Instancing false).
		std::vector<MeshInstanceBatch> Batches{};
	};

	struct SelectionRenderInputs
	{
		const RenderSnapshot* Snapshot = nullptr; // borrowed only for Record: its camera, its selection and the validation
		// M23: the snapshot's prepared draws (PrepareDraws) and the view's Instances structured buffer holding their records,
		// borrowed only for Record; both are required when a selection is drawn.
		const SelectionDrawSet* Draws = nullptr;
		nvrhi::IBuffer* Instances = nullptr;
		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G): the renderer's WindConstants at b8, which
		// displace a selected wind draw as the prepass does; null binds the pass's own calm constants.
		nvrhi::IBuffer* Wind = nullptr;
		nvrhi::ITexture* SceneDepth = nullptr;
		nvrhi::ITexture* Mask = nullptr;                      // R8_UNORM, full view extent; 1 visible, 0.5 occluded
		nvrhi::ITexture* Scratch = nullptr;                   // R8_UNORM dilation intermediate, distinct from Mask
		nvrhi::ITexture* LdrColor = nullptr;                  // RGBA8_UNORM composite target
		uint32_t Radius = 2;                                  // framebuffer pixels, [1,8]
		glm::vec4 Color = glm::vec4(1.0f, 0.55f, 0.1f, 1.0f); // linear/straight alpha; encode before composite
	};

	// Shared immutable pipelines, per-view targets/cache; main thread. Mask preserves M8 alpha/cull parity for Opaque
	// and Mask materials; selected Blend geometry uses geometric coverage. No stencil. Occluded edges dim to half alpha.
	class SelectionPass
	{
	public:
		static constexpr uint32_t PipelineCount = 8; // 6 mask variants + separable dilation + edge composite
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SelectionPass;
		};
		explicit SelectionPass(ConstructionKey key);
		~SelectionPass();
		SelectionPass(const SelectionPass&) = delete;
		SelectionPass& operator=(const SelectionPass&) = delete;
		[[nodiscard]] static Result<Scope<SelectionPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] static nvrhi::TextureDesc GetMaskDesc(uint32_t width, uint32_t height);
		// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): resolves the submesh draws of `snapshot`'s
		// selected items (a selected entity's items with a mesh whose buffers exist and a finite world matrix; a Mask
		// material with its constants and base colour map), in snapshot and submesh order, through the cache's mirrors;
		// appends one instance record per draw to `instances` (MakeMeshInstanceRecord's, RenderPrepare.h: {World,
		// PreviousWorld or World, PickId, the mirroring flag}) and groups the draws into batches (one per draw when
		// `instancing` is false). Nothing for a snapshot without a camera or a selection. Main thread; the caller writes
		// `instances` into the buffer Record reads. M23 (C23.3, decision 27): `sources`, empty or one per snapshot item, are
		// the vertices each item's draws bind (SkinningPass.h): a deformed item's draws bind its deformed vertices and key
		// its VertexStream, so they batch with no other item's; a Skipped item draws no mask.
		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3E and 3G): each selected item draws the
		// level the camera's view holds most of its pixels with (SelectViewMeshLod's Level), unfaded, and nothing beyond its
		// CullDistance; with `wind` (the snapshot has a Wind), a draw whose material has wind gets DrawFlagWind and its
		// WindResponse, so its mask covers the prepass's displaced surface.
		[[nodiscard]] static SelectionDrawSet PrepareDraws(GpuResourceCache& cache, const RenderSnapshot& snapshot, bool instancing,
			std::vector<InstanceRecord>& instances, std::span<const DeformedVertexSource> sources = {}, bool wind = false);
		// Empty selection clears mask and does no composite; invalid/deleted ids are ignored. InvalidArgument for invalid
		// radius/descriptors, or a selection without its prepared draws or instance buffer; Gpu from resources/bindings.
		// commandList open asserted; all inputs borrowed for this call. Every mask draw is instanced (Selection.slang reads its
		// world matrix from its instance record), and the SelectionMask row's DrawCalls counts the instanced draws.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
			const SelectionRenderInputs& inputs);
		// Record opens/closes SelectionMask, SelectionDilateHorizontal, SelectionDilateVertical, SelectionComposite.
		// Borrow recording only for this call; disabled work has no row, errors still close scopes and report recorded counters.
	private:
		struct State;
		Scope<State> m_State;
	};

}
