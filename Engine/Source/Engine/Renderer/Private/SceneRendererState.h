#pragma once

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/BloomPass.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/DepthPyramidPass.h"
#include "Engine/Renderer/FxaaPass.h"
#include "Engine/Renderer/GtaoPass.h"
#include "Engine/Renderer/LightClusterPass.h"
#include "Engine/Renderer/LightClusters.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/PointShadowMaps.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/SkinningPass.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/SpotShadowAtlas.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Engine/Renderer/UpscalePass.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/InstanceRecord.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ViewConstants.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Engine {

	namespace Detail {

		// One submesh draw of the frame, resolved by pass 1.
		struct SceneRendererDraw
		{
			nvrhi::IBuffer* VertexBuffer = nullptr; // held by the GpuResourceCache for the render
			// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decision 9): vertex binding 1, the mesh's attribute stream
			// or the cache's shared default stream (GpuMesh::AttributeBuffer), a function of the mesh; held by the cache.
			nvrhi::IBuffer* AttributeBuffer = nullptr;
			nvrhi::IBuffer* IndexBuffer = nullptr;
			uint32_t IndexOffset = 0;
			uint32_t IndexCount = 0;
			nvrhi::IBindingSet* Material = nullptr; // held by the renderer's material cache for the render
			uint32_t ForwardVariant = 0;
			uint32_t PrepassVariant = 0; // opaque draws only
			// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): the draw's instance record (its world matrix,
			// pick id and draw flags) at index Instance of the view's Instances buffer, and the instanced draw it belongs to: a
			// draw that starts one has Batch, the number of its draws (itself and the next Batch - 1 draws of its list, whose
			// records follow its own); the others have 0. Set by the Prepare stage after sorting (GroupMeshInstances, or 1 for
			// every draw without instancing).
			uint32_t Instance = 0;
			uint32_t Batch = 1;
			// M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 27; stream C): 0 when VertexBuffer is the mesh's own
			// vertices; for a deformed item's draw, the VertexStream of its SceneRendererDeformedMesh, whose Vertices it binds
			// (MeshInstanceKey::VertexStream, so a deformed draw never shares an instanced draw with another item's).
			uint32_t VertexStream = 0;
			// Sort keys (§8.3 pass 1).
			AssetHandle MaterialHandle{};
			AssetHandle Mesh{};
			UUID Entity{};
			uint32_t Submesh = 0;
			size_t SnapshotIndex = 0;
			float ViewDepth = 0.0f; // transparent draws: the view depth of the submesh bounds' centre
		};

		// A material of the frame, resolved once per render: its set, or null when its draws are skipped.
		struct SceneRendererMaterial
		{
			nvrhi::IBindingSet* Set = nullptr;
			Engine::AlphaMode Mode = Engine::AlphaMode::Opaque; // the type is spelled Engine:: (GCC's "changes meaning")
			bool DoubleSided = false;
		};

		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27; stream C): one deformed item's GPU
		// state, kept by its SceneRenderer across renders (the placement rule's place 2, SceneRenderer::State::DeformedMeshes),
		// keyed by the item's Entity and Mesh, which is how its previous positions survive from one render to the next: each
		// deformed item of a snapshot carries its own Entity (render extraction gives every item its entity, and a snapshot
		// built by hand gives each deformed item one, as the tests do), so two deformed items never share an entry, and each
		// entry's VertexStream keeps its draws out of every other item's instanced draw (each has its own pose). The Deform
		// stage creates it at the item's first deformation, grows it with the mesh's vertex and joint counts, and writes it
		// each render that draws the item; one that a render did not use is released at that render's end (with
		// ReleaseUnusedBindings), so an item that leaves the view deforms afresh, its previous positions its new ones, when it
		// returns. A device loss's release drops every entry with the state.
		struct SceneRendererDeformedMesh
		{
			UUID Entity{};
			AssetHandle Mesh{};
			uint64_t MeshVersion = 0;                // the GpuMesh's Version it was made for; another version deforms afresh
			nvrhi::BufferHandle Vertices{};          // SkinningPass::GetVerticesDesc: the deformed vertices, vertex binding 0 of its draws
			nvrhi::BufferHandle PreviousPositions{}; // SkinningPass::GetPreviousPositionsDesc: the last render's deformed positions
			nvrhi::BufferHandle Palette{};           // SkinningPass::GetPaletteDesc: the item's JointPalette
			nvrhi::BufferHandle MorphWeights{};      // SkinningPass::GetMorphWeightsDesc: the item's MorphWeights
			uint32_t VertexStream = 0;               // never 0: SceneRendererDraw::VertexStream of its draws, unique per renderer
			uint64_t LastRender = 0;                 // the render that last deformed it
			bool Deformed = false;                   // a deformation was recorded since its creation (otherwise FirstDeform)
		};

		// Which mesh pass RecordMeshPass records.
		enum class SceneRendererMeshPass : uint8_t
		{
			Prepass,
			ForwardOpaque,
			ForwardTransparent,
			Picking,
			Overdraw
		};

		// The size-dependent targets of a renderer.
		struct SceneRendererTargets
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			nvrhi::TextureHandle SceneDepth{};
			nvrhi::TextureHandle SceneNormals{};
			nvrhi::TextureHandle SceneColor{};
			std::array<nvrhi::TextureHandle, 2> Ldr{}; // LdrColor and its FXAA ping-pong partner
			nvrhi::TextureHandle EntityId{};
			nvrhi::TextureHandle ViewDepth{};
			nvrhi::TextureHandle Occlusion{};
			nvrhi::TextureHandle AoScratch{};
			nvrhi::TextureHandle Cascades{};
			nvrhi::TextureHandle SpotAtlas{};
			std::vector<nvrhi::FramebufferHandle> CascadeFramebuffers{}; // ShadowPass::CreateFramebuffers(Cascades)
			nvrhi::FramebufferHandle SpotAtlasFramebuffer{};
			// M23 (C23.1, Docs/Decisions/0034-m23-contract.md decision 3; stream B): the PointShadowMaps cube array (6 x
			// MaxPointShadowLights layers of GetPointShadowFaceSize texels, the Shadows declaration's depth format), created
			// while a point light is planned, and one framebuffer per face layer.
			nvrhi::TextureHandle PointShadowMaps{};
			std::vector<nvrhi::FramebufferHandle> PointShadowFramebuffers{};
			nvrhi::TextureHandle SelectionMask{};
			nvrhi::TextureHandle SelectionScratch{};
			nvrhi::TextureHandle Overdraw{};
			nvrhi::FramebufferHandle PickingFramebuffer{};
			nvrhi::FramebufferHandle OverdrawFramebuffer{};
			nvrhi::TextureHandle Bloom{}; // BloomPass::GetChainDesc
			nvrhi::FramebufferHandle PrepassFramebuffer{};
			nvrhi::FramebufferHandle ForwardFramebuffer{};
			std::array<nvrhi::FramebufferHandle, 2> OverlayFramebuffers{}; // each LDR target with SceneDepth (tested, not written)
		};

		// M18 (C18.1, Docs/Decisions/0029-m18-contract.md decision 6): the output stage's per-view objects at the renderer's
		// (view) size, owned by stream F (SceneRendererOutput.cpp) and created only while the output mapping is not the
		// identity; at the identity the editor overlays draw into the scene's LDR target with SceneDepth, as before.
		struct SceneRendererOutputTargets
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			nvrhi::TextureHandle Color{};                  // OutputColor: LdrColorFormat (UpscalePass::GetColorFramebufferInfo)
			nvrhi::TextureHandle Depth{};                  // OutputDepth: SceneDepthFormat, created when an editor overlay needs depth
			nvrhi::FramebufferHandle ColorFramebuffer{};   // Color alone: UpscalePassInputs::Color
			nvrhi::FramebufferHandle DepthFramebuffer{};   // Depth alone: UpscalePassInputs::Depth
			nvrhi::FramebufferHandle OverlayFramebuffer{}; // Color with Depth: the EditorOverlay stage's target
			// The ViewConstants of the editor overlays: the overlay camera's (SceneRendererOutput.cpp: the scene camera whose
			// projection maps the scene's clip volume onto the upscaled scene rectangle, Scale times the scene size, of the
			// view-size viewport), so lines and labels keep their pixel sizes at the view's resolution.
			nvrhi::BufferHandle OverlayViewConstants{};
			// The 1 x 1 LdrColorFormat texel the letterbox repaint clears to the letterbox colour and upscales over the
			// letterbox bars after the editor overlays (UpscalePass::RecordLetterbox), created with OverlayViewConstants.
			nvrhi::TextureHandle LetterboxTexel{};
			// Stream F: the selection outline's mask and dilation scratch at the view size (SelectionPass::GetMaskDesc), created
			// when a selection is drawn; the EditorOverlay stage draws the outline at the view's resolution.
			nvrhi::TextureHandle SelectionMask{};
			nvrhi::TextureHandle SelectionScratch{};
			// Stream F: a 1 x 1 EntityId texel cleared to 0 by every picking render, which a pick of a letterbox pixel reads
			// (SceneRenderer::RequestPick: the background's result).
			nvrhi::TextureHandle LetterboxEntityId{};
		};

		// M18 (C18.1, decision 1): what the stage functions of one Render share (SceneRenderer::State's Record*Stage). Render
		// fills the first group before the Prepare stage; each later group is filled by the stage it names and read by the
		// stages after it. These members are the frozen hand-over between stream A (Prepare to LdrPost) and stream F
		// (SceneOverlay, Output, EditorOverlay); stream A may add members that only its own stages read.
		struct SceneRendererFrame
		{
			// Set by Render before the Prepare stage.
			const RenderSnapshot* Snapshot = nullptr;
			nvrhi::ICommandList* CommandList = nullptr; // open, never null (the Prepare stage records nothing into it)
			RenderRecordingContext* Recording = nullptr;
			// ComputeRenderOutputMapping of the renderer's size and Snapshot->Output.
			RenderOutputMapping Output{};
			RenderDebugView DebugView = RenderDebugView::Lit;
			bool IsLit = true;       // DebugView == Lit
			bool IsDataView = false; // AO, ShadowCascades, Overdraw or LightCount
			bool IsOverdraw = false;
			bool Wireframe = false; // Lit with RenderViewFlags::Wireframe
			bool Picking = false;   // RenderViewFlags::Picking
			bool Selection = false; // the selection outline is drawn (EditorOverlays and Selection with a selection, not a data view)
			bool PixelArt = false;  // RenderOutputSettings::PixelArt: the 2D colour path (decision 7)
			// Set by the Prepare stage.
			bool HasCamera = false; // the snapshot's camera, false also for one with a non-finite matrix (the view is cleared)
			CameraData Camera{};    // the snapshot's camera with its viewport at the scene resolution
			// Set by the Tonemap and LdrPost stages: the index of the scene-resolution LDR target holding the scene image
			// (SceneRendererTargets::Ldr), which the SceneOverlay stage draws into through OverlayFramebuffers.
			size_t SceneTarget = 0;
			// Set by the Output stage: the editor overlays' target and the depth they test against, at the renderer's size:
			// the scene's LDR target with SceneDepth at the identity, OutputColor with OutputDepth otherwise.
			nvrhi::ITexture* ViewColor = nullptr;
			nvrhi::IFramebuffer* ViewOverlayFramebuffer = nullptr;
			nvrhi::IBuffer* ViewOverlayConstants = nullptr;
			// The first partial error of the render, returned by Render (Architecture §8.2).
			Status FirstError{};

			// Stream A's hand-over between its own stages (Prepare to LdrPost), which no later stage reads.
			// Set by the Prepare stage: GTAO and its resolution; the per-view constants Render writes before the Deform stage
			// (ViewConstants b0, EnvironmentConstants b2, ShadowConstants b1); the shadow plans; the environment's cubes (the
			// black cube without a map) and the blue noise of the dither (null without one).
			bool Ao = false;
			bool HalfAo = false;
			ViewConstants View{};
			EnvironmentConstants Environment{};
			ShadowConstants Shadow{};
			ShadowCascadeSet Cascades{};
			SpotShadowAtlas Spots{};
			// M23 (C23.1, decisions 3 and 5; streams B and C): the point-shadow plan (ShadowConstants' Points), and the cluster
			// grid the LightClusters pass and the forward passes read (LightClusterConstants b3).
			PointShadowSet Points{};
			LightClusterGrid Clusters{};
			// M23 (C23.3, decision 27; stream C): set by the Prepare stage: the indices into SceneRenderer::State::DeformedMeshes
			// of the items the Deform stage deforms this render (every deformed item one of whose draws survived culling, the
			// shadow casters' included), in snapshot order.
			std::vector<size_t> Deforms{};
			nvrhi::TextureHandle EnvironmentSpecular{};
			nvrhi::TextureHandle EnvironmentSkybox{};
			nvrhi::TextureHandle BlueNoise{};
			// Set by the DepthNormal stage: the view's prepass set (null without a camera or when it cannot be created), which
			// the Overdraw data view draws with.
			nvrhi::IBindingSet* PrepassSet = nullptr;
			// Set by the Ambient stage: the occlusion and linear depth the forward pass and the data views read (the neutral
			// white AO and far depth when GTAO did not run).
			nvrhi::ITexture* Occlusion = nullptr;
			nvrhi::ITexture* LinearDepth = nullptr;
			// Set by the HdrPost stage: the bloom chain's result for the Tonemap stage (null without bloom) and its intensity.
			nvrhi::ITexture* Bloom = nullptr;
			float BloomIntensity = 0.0f;
			// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): set by the Prepare stage with the view's
			// instance records (SceneRenderer::State::Instances, after the camera's draws'): the shadow casters the Shadows stage
			// records (ShadowPass::PrepareCasters; empty without a planned shadow) and the selection mask's draws the
			// EditorOverlay stage records (SelectionPass::PrepareDraws; empty without frame.Selection).
			ShadowCasterSet ShadowCasters{};
			SelectionDrawSet SelectionDraws{};

			// Keeps `result` when it is the render's first error.
			void KeepFirstError(Status result)
			{
				if (!result && FirstError)
					FirstError = std::move(result);
			}
		};

	}

	struct SceneRendererPipelines::State
	{
		GraphicsDevice* Device = nullptr;   // documented back-reference
		PipelineFactory* Factory = nullptr; // documented back-reference (EnsureDebugView)
		Utils::MeshBindingLayouts Layouts{};
		std::array<GraphicsPipeline, Utils::PrepassVariantCount> Prepass{};
		std::array<GraphicsPipeline, Utils::PrepassVariantCount> Picking{};
		std::array<GraphicsPipeline, Utils::MeshCullModeCount> Overdraw{};
		ComputePipeline DataView{};
		// The forward variants of each debug view, indexed by RenderDebugView (Lit's at startup, the others by
		// EnsureDebugView).
		std::array<std::array<GraphicsPipeline, Utils::ForwardVariantCount>, RenderDebugViewCount> Forward{};
		std::array<bool, RenderDebugViewCount> HasView{};
		nvrhi::SamplerHandle LinearClamp{};
		nvrhi::SamplerHandle AnisoWrap{};
		nvrhi::TextureHandle BlackCube{};
		nvrhi::TextureHandle EmptyCascades{};
		nvrhi::TextureHandle EmptyAtlas{};
		// M23 (R-PLS): the neutral PointShadowMaps (t12) of a view without a planned point light (SceneRenderer.cpp).
		nvrhi::TextureHandle EmptyPointShadows{};
		nvrhi::TextureHandle WhiteAo{};
		nvrhi::TextureHandle FarDepth{};
		nvrhi::SamplerHandle ShadowCompare{};
		Scope<ShadowPass> Shadows{};
		Scope<DepthPyramidPass> DepthPyramid{};
		Scope<GtaoPass> Gtao{};
		// M23 (C23.1, Docs/Decisions/0034-m23-contract.md decision 5; stream C): the LightClusters pass (the placement rule's
		// place 1).
		Scope<LightClusterPass> LightClusters{};
		// M23 (C23.3, decision 27; stream C): the Skinning pass of the Deform stage (place 1), created by CreatePipelines.
		Scope<SkinningPass> Skinning{};
		Scope<SelectionPass> Selection{};
		// The passes the set owns (SceneRenderer.h).
		Scope<SkyboxPass> Skybox;
		Scope<BloomPass> Bloom;
		Scope<TonemapPass> Tonemap;
		Scope<FxaaPass> Fxaa;
		Scope<DebugRenderer> Debug;
		Scope<TextRenderer> Text;
		Scope<BrdfLut> DfgLut;
		// M18 (C18.1): the QuadBatcher and the output stage's upscale (stream F), created by CreatePipelines once the wave
		// integrates them.
		Scope<QuadBatcher> Quads{};
		Scope<UpscalePass> Upscale{};
		uint32_t PipelineCount = 0;
		// M18 (C18.1): SceneRendererPipelines::GetGeneration: 0 until Create's CreatePipelines, which makes it 1, and increased
		// by every later successful CreatePipelines.
		uint64_t Generation = 0;
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 26): the set's registration with its device's recreation
		// registry (Create); kept by ReleaseGpuObjects like the back-references and the generation.
		GpuRecreationRegistration Recreation{};
	};

	struct SceneRenderer::State
	{
		// Documented back-references.
		GraphicsDevice* Device = nullptr;
		SceneRendererPipelines* PipelineSet = nullptr; // EnsureDebugView
		SceneRendererPipelines::State* Pipelines = nullptr;
		GpuResourceCache* Cache = nullptr;
		AssetManager* Assets = nullptr;

		// The view's binding sets (PassBindingCache.h): its own set-0 sets, those of each shared pass, the materials'.
		PassBindingCache ViewBindings{};
		PassBindingCache SkyboxBindings{};
		PassBindingCache BloomBindings{};
		PassBindingCache TonemapBindings{};
		PassBindingCache FxaaBindings{};
		PassBindingCache DebugBindings{};
		PassBindingCache TextBindings{};
		PassBindingCache ShadowBindings{};
		PassBindingCache DepthBindings{};
		PassBindingCache GtaoBindings{};
		PassBindingCache SelectionBindings{};
		PassBindingCache DataViewBindings{};
		PassBindingCache LightClusterBindings{}; // M23 (stream C): the LightClusters pass's sets
		PassBindingCache SkinningBindings{};     // M23 (C23.3, stream C): the Skinning pass's sets
		Scope<RenderTargetPool> TargetPool{};
		Scope<AsyncPicker> Picker{};
		Scope<GpuProfiler> Profiler{};
		RenderStatsHistory History{};
		std::vector<uint64_t> TimingSubmissions{};
		uint32_t NextTimingSlot = 0;
		std::optional<uint32_t> PendingTimingSlot{};
		uint64_t Generation = 1;
		uint64_t ImageFrame = 0;
		uint64_t ImageRevision = 0;
		uint64_t ImageGeneration = 0;
		std::vector<UUID> ImagePickTable{};
		bool PendingSubmission = false;
		bool RecordedPicking = false;
		bool SubmittedPicking = false;
		bool SpotLimitLogged = false;
		bool ShadowRejectionLogged = false; // a light the shadow planners reject is reported once per renderer
		bool WireframeBudgetLogged = false; // wireframe edges beyond the debug draw budget are reported once per renderer
		bool AllocationLimitLogged = false;
		nvrhi::BufferHandle ShadowConstantsBuffer{};
		nvrhi::BufferHandle ShadowCasterConstantsBuffer{}; // ShadowPass::GetCasterConstantsDesc
		Utils::MaterialBindingCache Materials{};

		Detail::SceneRendererTargets Targets{};
		// The renderer's (view) size: Targets are at the scene resolution of the last Render's output mapping.
		uint32_t ViewWidth = 1;
		uint32_t ViewHeight = 1;
		size_t FinalTarget = 0; // the index of the LDR target holding the last image
		// M18 (C18.1): the output stage (stream F): its targets, its binding sets and the last Render's mapping
		// (SceneRenderer::GetOutputMapping); and the pipeline generation this renderer's binding sets were made for.
		Detail::SceneRendererOutputTargets OutputTargets{};
		PassBindingCache UpscaleBindings{};
		RenderOutputMapping OutputMapping{};
		// M18's milestone review: the last Render's output settings (the defaults before the first), at whose scene resolution
		// Resize and the device-loss recovery create the targets, so the next Render with the same settings keeps them. CPU
		// state that a device loss's release keeps (SceneRendererRecovery.cpp).
		RenderOutputSettings OutputSettings{};
		uint64_t PipelineGeneration = 0;
		nvrhi::BufferHandle ViewConstantsBuffer{};
		nvrhi::BufferHandle EnvironmentConstantsBuffer{};
		nvrhi::BufferHandle LightBuffer{};
		// M23 (C23.1, Docs/Decisions/0034-m23-contract.md decisions 2 and 5): the mesh path's per-view buffers (the placement
		// rule's place 2), null until their streams create them: the Instances structured buffer of InstanceRecord (stream
		// A: t10 of set 0, grown to the view's instance count), the cluster masks (stream C: t11 of set 0, the LightClusters
		// pass's u0) and the cluster grid's constants (stream C: b3 of set 0).
		nvrhi::BufferHandle InstanceBuffer{};
		nvrhi::BufferHandle LightClusterBuffer{};
		nvrhi::BufferHandle LightClusterConstantsBuffer{};
		// M18 (C18.3, decision 26): the renderer's registration with its device's recreation registry (Create); kept by
		// ReleaseGpuObjects like the back-references.
		GpuRecreationRegistration Recreation{};
		// Reused by every Render.
		std::vector<Detail::SceneRendererDraw> OpaqueDraws{};
		std::vector<Detail::SceneRendererDraw> TransparentDraws{};
		std::vector<ShaderLight> Lights{};
		bool LightLimitLogged = false; // RENDER_LIGHT_LIMIT_EXCEEDED is logged once per renderer
		// M23 (C23.1): the view's instance records, reused by every Render (stream A: the opaque draws', then the transparent
		// draws', the shadow casters' and the selection's, written into InstanceBuffer by RecordViewSetup), and
		// RENDER_POINT_SHADOW_BUDGET logged once per renderer (stream B).
		std::vector<InstanceRecord> Instances{};
		bool PointShadowLimitLogged = false;
		// M23 (C23.3, Docs/Decisions/0034-m23-contract.md decision 27; stream C): the deformed items' GPU state (place 2), the
		// next VertexStream value to give a new entry (from 1), and the render count that stamps LastRender. A skinned item
		// whose palette is shorter than its mesh's skin draws its bind pose, which is logged once per renderer
		// (RenderSkinMismatchCode): ReleaseGpuObjects keeps SkinMismatchLogged with the other once-per-renderer flags
		// (SceneRendererRecovery.cpp's kept members, stream C), so a recovered renderer does not warn again.
		std::vector<Detail::SceneRendererDeformedMesh> DeformedMeshes{};
		uint32_t NextVertexStream = 1;
		uint64_t DeformRender = 0;
		bool SkinMismatchLogged = false;
		SceneRenderStats Stats{};
		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 44): the debug view the last Render recorded (Lit before the
		// first), which a Render records instead of a debug view whose first pipelines ran out of memory after their trim and
		// retry (SceneRenderer::GetRenderedDebugView); and the views whose switch failed that way, logged once per renderer and
		// view. CPU state that a device loss's release keeps (SceneRendererRecovery.cpp).
		RenderDebugView RenderedDebugView = RenderDebugView::Lit;
		std::array<bool, RenderDebugViewCount> DebugViewFailureLogged{};

		// The targets of `width` x `height` with their framebuffers.
		[[nodiscard]] Result<Detail::SceneRendererTargets> CreateTargets(uint32_t width, uint32_t height) const;
		// `pointShadowFaceSize`: the PointShadowMaps' face size while a point light is planned (GetPointShadowFaceSize), else 0.
		[[nodiscard]] Status EnsureFrameTargets(bool picking, bool ao, bool halfAo, uint32_t shadowSize, bool spots, uint32_t pointShadowFaceSize,
			bool selection, bool overdraw);
		// Clears every binding-set cache that references the targets.
		void ClearTargetBindings();
		// Clears every binding-set cache and the material sets: Render calls it before its first recording with a new
		// generation of the pipeline set (SceneRendererPipelines::GetGeneration), whose binding layouts replaced those the
		// sets were made for.
		void ClearBindings();
		// The material `handle` of this render: its mirror's set, created or found in the material cache; an empty
		// Detail::SceneRendererMaterial (no set: its draws are skipped) when the mirror lacks GPU objects, which the cache reported.
		// Errors: those of the material cache's GetOrCreate (Gpu), with the material named.
		[[nodiscard]] Result<Detail::SceneRendererMaterial> ResolveMaterial(AssetHandle handle);
		// Pass 1: the draws of `snapshot` seen by `camera` into OpaqueDraws and TransparentDraws, culled, resolved and sorted;
		// then (M23) their instance records into Instances and their instanced draws (SceneRendererDraw's Instance and Batch).
		// A material of a culled submesh keeps the set it already has while its mirror is current (R-MATSET,
		// MaterialBindingCache::Keep), so it makes no new set when it re-enters the view; a culled material never gets one.
		// Returns the first non-finite world matrix's error, after skipping that draw, or a material set's creation error.
		[[nodiscard]] Status PrepareDraws(const RenderSnapshot& snapshot, const CameraData& camera);
		// Passes 4, 7 and 9: `draws` with the pipelines of `pass` (the forward ones of `view`) and the view set `viewSet`, one
		// instanced draw per batch of `draws` (M23, R-INST).
		void RecordMeshPass(nvrhi::ICommandList& commandList, Detail::SceneRendererMeshPass pass, std::span<const Detail::SceneRendererDraw> draws, nvrhi::IBindingSet& viewSet,
			RenderDebugView view, RenderPassCounters& counters);
		// Drops what the render did not use from every binding-set cache.
		void ReleaseUnusedBindings();
		// M23 (C23.1, R-INST): grows InstanceBuffer, the view's Instances structured buffer (null after a device loss's
		// release, so the next Render creates it again), to hold Instances (at least one record, so every set binds a buffer).
		// Errors: Gpu when the buffer cannot be created (the previous one stays).
		[[nodiscard]] Status EnsureInstanceBuffer();
		// What Render records between the Prepare stage and the Deform stage: the per-view constants the Prepare stage made
		// (frame's View, Environment and Shadow, the packed Lights and the instance records), then the clears of SceneColor (the camera's
		// ClearColor), SceneDepth (0, the far plane), SceneNormals and, for a picking view, EntityId.
		void RecordViewSetup(Detail::SceneRendererFrame& frame);

		// M18 (C18.1, decision 1): Render's stage functions, one per RenderStage, called in that order between
		// RenderRecordingContext::BeginStage and EndStage; each records its stage's declared passes in their declared order,
		// or none, keeping partial errors in frame.FirstError. Stream A owns Prepare to LdrPost (SceneRenderer.cpp), stream F
		// SceneOverlay, Output and EditorOverlay (SceneRendererOutput.cpp).
		// Prepare (CPU): validates the snapshot, culls and sorts the draws, packs the lights, plans the shadows, writes the
		// per-view constants and ensures the frame's targets at the scene resolution. An error returned here is one of
		// Render's errors before recording: Render returns it and records nothing.
		[[nodiscard]] Status RecordPrepareStage(Detail::SceneRendererFrame& frame);
		// M23 (C23.3, decision 27; stream C): the Skinning pass, one dispatch per entry of frame.Deforms, each writing the
		// item's DeformedMeshes entry; an entry whose dispatch fails keeps its error in frame.FirstError and its draws are
		// skipped this render. Records nothing without a deformed item (every render until stream C).
		void RecordDeformStage(Detail::SceneRendererFrame& frame);
		void RecordShadowsStage(Detail::SceneRendererFrame& frame);
		void RecordDepthNormalStage(Detail::SceneRendererFrame& frame);
		void RecordAmbientStage(Detail::SceneRendererFrame& frame);
		// M23 (C23.1, decision 5; stream C): the end of the Ambient stage. Computes frame.Clusters at the scene resolution,
		// makes LightClusterConstantsBuffer and grows LightClusterBuffer to the grid (EnsureLightClusterBuffers), then records
		// the LightClusters pass, which writes the clustered constants, when Quality.ClusteredLights is on, the light list
		// holds a point or spot light (with directional lights alone every cluster would hold the whole list) and a pass reads
		// the masks (the Lit view's forward passes with a camera, or the LightCount view); otherwise, or when the pass cannot be
		// recorded (its error kept), it writes the brute-force constants. Without both buffers (their creation error kept) it
		// writes nothing, and the forward passes and the data views' composite, which bind them, are skipped.
		void RecordLightClusters(Detail::SceneRendererFrame& frame);
		// LightClusterConstantsBuffer when it is null, and LightClusterBuffer when it is null or holds fewer than
		// `clusterCount` clusters (LightClusterPass's descs), each replacing the member only when its creation succeeded.
		// Errors: those of GraphicsDevice::CreateBuffer (Gpu).
		[[nodiscard]] Status EnsureLightClusterBuffers(uint32_t clusterCount);
		void RecordForwardStage(Detail::SceneRendererFrame& frame);
		// Reserved for M21's Sprites and Lights2D passes; records nothing in M18.
		void RecordSpritesStage(Detail::SceneRendererFrame& frame);
		void RecordHdrPostStage(Detail::SceneRendererFrame& frame);
		// Tonemap (or the data views' composite); with frame.PixelArt the 2D colour path (decision 7).
		void RecordTonemapStage(Detail::SceneRendererFrame& frame);
		// FXAA, never with frame.PixelArt or a debug view.
		void RecordLdrPostStage(Detail::SceneRendererFrame& frame);
		// Game Text (world, then screen) at the scene resolution into the scene LDR target; M22 appends Ui after it.
		void RecordSceneOverlayStage(Detail::SceneRendererFrame& frame);
		// The upscale when frame.Output is not the identity (UpscalePass); sets frame's ViewColor, ViewOverlayFramebuffer and
		// ViewOverlayConstants either way.
		void RecordOutputStage(Detail::SceneRendererFrame& frame);
		// Wireframe, the selection outline, the debug lines with annotations, grid and icons, then the labels, at the
		// renderer's size into frame.ViewOverlayFramebuffer. When frame.Output is not the identity they draw over the whole
		// view with the overlay camera (the scene camera whose projection maps the scene's clip volume onto the upscaled
		// scene rectangle, partly outside the view for a cropped scene), so inside the rectangle every overlay lands where
		// that rectangle's viewport would put it; then, when the mapping has a letterbox, the letterbox repaint (the
		// "Letterbox" row, UpscalePass::RecordLetterbox) gives the bars their letterbox colour again, which clips the
		// overlays to the rectangle as its scissor, intersected with the view, would.
		void RecordEditorOverlayStage(Detail::SceneRendererFrame& frame);
	};

}
