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
#include "Engine/Renderer/Lights2DPass.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/Private/MaterialBindingCache.h"
#include "Engine/Renderer/Private/TilemapDrawCache.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SelectionPass.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Engine/Renderer/SkyboxPass.h"
#include "Engine/Renderer/SpotShadowAtlas.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/TonemapPass.h"
#include "Engine/Renderer/UpscalePass.h"
#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ViewConstants.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Engine {

	struct SpriteSheetData;

	namespace Detail {

		// One submesh draw of the frame, resolved by pass 1.
		struct SceneRendererDraw
		{
			nvrhi::IBuffer* VertexBuffer = nullptr; // held by the GpuResourceCache for the render
			nvrhi::IBuffer* IndexBuffer = nullptr;
			uint32_t IndexOffset = 0;
			uint32_t IndexCount = 0;
			nvrhi::IBindingSet* Material = nullptr; // held by the renderer's material cache for the render
			uint32_t ForwardVariant = 0;
			uint32_t PrepassVariant = 0; // opaque draws only
			DrawConstants Constants{};
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
			nvrhi::TextureHandle SelectionMask{};
			nvrhi::TextureHandle SelectionScratch{};
			nvrhi::TextureHandle Overdraw{};
			nvrhi::FramebufferHandle PickingFramebuffer{};
			nvrhi::FramebufferHandle OverdrawFramebuffer{};
			nvrhi::TextureHandle Bloom{}; // BloomPass::GetChainDesc
			nvrhi::FramebufferHandle PrepassFramebuffer{};
			nvrhi::FramebufferHandle ForwardFramebuffer{};
			std::array<nvrhi::FramebufferHandle, 2> OverlayFramebuffers{}; // each LDR target with SceneDepth (tested, not written)
			// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 22): the Lights2D pass's targets, created by the first
			// render that draws a lit 2D item (SceneRenderer::State::EnsureLights2DTargets) and kept like the other lazily
			// created targets until the size changes.
			nvrhi::TextureHandle Lights2DNormals{};                // Lights2DNormalsFormat
			nvrhi::TextureHandle Lights2DLight{};                  // Lights2DLightFormat
			nvrhi::FramebufferHandle Lights2DNormalsFramebuffer{}; // Lights2DNormals with SceneDepth (tested, not written)
			nvrhi::FramebufferHandle Lights2DLightFramebuffer{};   // Lights2DLight alone
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
			bool IsDataView = false; // AO, ShadowCascades or Overdraw
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
		nvrhi::TextureHandle WhiteAo{};
		nvrhi::TextureHandle FarDepth{};
		nvrhi::SamplerHandle ShadowCompare{};
		Scope<ShadowPass> Shadows{};
		Scope<DepthPyramidPass> DepthPyramid{};
		Scope<GtaoPass> Gtao{};
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
		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 22): the 2D lights' accumulation (place 1 of ADR 0029
		// decision 27).
		Scope<Lights2DPass> Lights2D{};
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
		// M21 (Docs/Decisions/0032-m21-contract.md decision 4): the Sprites stage's sets, the QuadBatcher's one per texture and
		// sampler (place 2 of ADR 0029 decision 27: released with the state on a device loss, like every set above).
		PassBindingCache SpriteBindings{};
		// M21 (C21.2, decision 22): the Lights2D pass's set (its view's Lights2DNormals).
		PassBindingCache Lights2DBindings{};
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
		bool Light2DLimitLogged = false; // lights beyond MaxVisibleLights2D are reported once per renderer (M21, C21.2)
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
		// M18 (C18.3, decision 26): the renderer's registration with its device's recreation registry (Create); kept by
		// ReleaseGpuObjects like the back-references.
		GpuRecreationRegistration Recreation{};
		// Reused by every Render.
		std::vector<Detail::SceneRendererDraw> OpaqueDraws{};
		std::vector<Detail::SceneRendererDraw> TransparentDraws{};
		// M21: the Sprites stage's quads in the snapshot's draw order, their textures borrowed from the GpuResourceCache's
		// mirrors for the render, and, one per quad, whether it is lit (C21.2, decision 22: 1 for a lit sprite or tile, which
		// draws with the HdrLit pipeline); AddSpriteQuad appends to both.
		std::vector<QuadDraw> SpriteQuads{};
		std::vector<uint8_t> SpriteQuadLit{};
		// C21.2: the lit quads of SpriteQuads, in order, for the Normals record, and the 2D lights the view draws.
		std::vector<QuadDraw> LitSpriteQuads{};
		std::vector<Light2DItem> VisibleLights2D{};
		// M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24): the Sprites stage's chunk bounds of each tilemap it
		// drew in its last render (TilemapDrawCache.h), and its scratch flags of the chunks that meet the view. CPU state that
		// a device loss's release keeps.
		std::vector<Detail::TilemapDrawCache> TilemapCaches{};
		std::vector<uint8_t> TilemapChunksDrawn{};
		std::vector<ShaderLight> Lights{};
		bool LightLimitLogged = false; // RENDER_LIGHT_LIMIT_EXCEEDED is logged once per renderer
		SceneRenderStats Stats{};
		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 44): the debug view the last Render recorded (Lit before the
		// first), which a Render records instead of a debug view whose first pipelines ran out of memory after their trim and
		// retry (SceneRenderer::GetRenderedDebugView); and the views whose switch failed that way, logged once per renderer and
		// view. CPU state that a device loss's release keeps (SceneRendererRecovery.cpp).
		RenderDebugView RenderedDebugView = RenderDebugView::Lit;
		std::array<bool, RenderDebugViewCount> DebugViewFailureLogged{};

		// The targets of `width` x `height` with their framebuffers.
		[[nodiscard]] Result<Detail::SceneRendererTargets> CreateTargets(uint32_t width, uint32_t height) const;
		[[nodiscard]] Status EnsureFrameTargets(bool picking, bool ao, bool halfAo, uint32_t shadowSize, bool spots, bool selection, bool overdraw);
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
		// Pass 1: the draws of `snapshot` seen by `camera` into OpaqueDraws and TransparentDraws, culled, resolved and sorted.
		// A material of a culled submesh keeps the set it already has while its mirror is current (R-MATSET,
		// MaterialBindingCache::Keep), so it makes no new set when it re-enters the view; a culled material never gets one.
		// Returns the first non-finite world matrix's error, after skipping that draw, or a material set's creation error.
		[[nodiscard]] Status PrepareDraws(const RenderSnapshot& snapshot, const CameraData& camera);
		// Passes 4, 7 and 9: `draws` with the pipelines of `pass` (the forward ones of `view`) and the view set `viewSet`.
		void RecordMeshPass(nvrhi::ICommandList& commandList, Detail::SceneRendererMeshPass pass, std::span<const Detail::SceneRendererDraw> draws, nvrhi::IBindingSet& viewSet,
			RenderDebugView view, RenderPassCounters& counters);
		// Drops what the render did not use from every binding-set cache.
		void ReleaseUnusedBindings();
		// What Render records between the Prepare stage and the Deform stage: the per-view constants the Prepare stage made
		// (frame's View, Environment and Shadow, and the packed Lights), then the clears of SceneColor (the camera's
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
		// Reserved for M23's Skinning pass; records nothing in M18.
		void RecordDeformStage(Detail::SceneRendererFrame& frame);
		void RecordShadowsStage(Detail::SceneRendererFrame& frame);
		void RecordDepthNormalStage(Detail::SceneRendererFrame& frame);
		void RecordAmbientStage(Detail::SceneRendererFrame& frame);
		void RecordForwardStage(Detail::SceneRendererFrame& frame);
		// M21's Sprites pass (Docs/Decisions/0032-m21-contract.md decision 4): in a Lit view with a camera (not the wireframe),
		// the snapshot's sprites in its draw order, one QuadBatcher quad each in world space (Asset/SpriteGeometry.h's quad of
		// the frame or texture this stage resolves through the GpuResourceCache, the camera's view-projection, the Point
		// sampler, Color as the quad's colour), with the Hdr pipeline into SceneColor depth-tested against SceneDepth, under
		// the Sprites row; a sprite whose quad lies wholly outside the view is culled. C21.2's tilemaps draw in the same order,
		// each before Sprites[DrawPosition], their tiles' quads appended by AppendTilemapQuads. C21.2 (decision 22): when a
		// drawn quad is lit, the Lights2D pass first (RecordLights2D), then the quads in runs of one lit-ness
		// (RecordSpriteQuads).
		void RecordSpritesStage(Detail::SceneRendererFrame& frame);
		// C21.2 (decision 22): appends `quad` to SpriteQuads and its lit-ness to SpriteQuadLit, in the 2D draw order.
		void AddSpriteQuad(const QuadDraw& quad, bool lit);
		// M21 (C21.2, decision 24): appends to SpriteQuads the quads of `tilemap`'s tiles that the Sprites stage draws, in the
		// tilemap's draw order (Asset/TilemapDrawOrder.h): every tile of each chunk whose bounds (TilemapCaches, rebuilt when
		// stale) meet the clip volume of `viewProjection`, each the quad of the sprite TilemapRendererComponent.h describes,
		// its sheet resolved through `resolveSheet` (the stage's sheets of the render: null for one that is missing or fails
		// to load, reported there). A tilemap that is missing or fails to load draws nothing and is reported with
		// GetOrPlaceholder's codes; a non-finite world matrix or PixelsPerUnit draws nothing and is kept as frame's error.
		void AppendTilemapQuads(Detail::SceneRendererFrame& frame, const TilemapItem& tilemap, const glm::mat4& viewProjection,
			const std::function<const SpriteSheetData*(AssetHandle)>& resolveSheet);
		// M21 (C21.2, decision 24): drops the tilemap caches the render did not draw, and clears the others' Used flags.
		void ReleaseUnusedTilemaps();
		// C21.2: the Lights2D targets and their framebuffers at the scene resolution (Targets), created when missing.
		// Errors: those of the pool and the framebuffer creation (Gpu, OutOfMemory after its trim and retry).
		[[nodiscard]] Status EnsureLights2DTargets();
		// C21.2, the Lights2D row, recorded when a quad of SpriteQuads is lit: Lights2DNormals cleared to the flat normal and,
		// when a lit quad has a normal map, every lit quad's normal drawn into it in draw order (the QuadBatcher's Normals
		// pipeline); then Lights2DLight cleared and the lights the view draws added (Lights2DPass: the snapshot's Lights2D that
		// reach the view of `viewProjection`, in their order, at most MaxVisibleLights2D, the first skip logged once per
		// renderer). Returns the light accumulation the lit quads sample, or null when no quad is lit, or when its targets or
		// its pass failed (the error kept in frame.FirstError).
		[[nodiscard]] nvrhi::ITexture* RecordLights2D(Detail::SceneRendererFrame& frame, const glm::mat4& viewProjection);
		// C21.2, the Sprites row: SpriteQuads in their order, each run of one lit-ness one QuadBatcher record (Hdr, or HdrLit
		// sampling `light`); a lit run without `light` is not drawn (RecordLights2D kept its error).
		void RecordSpriteQuads(Detail::SceneRendererFrame& frame, const glm::mat4& viewProjection, nvrhi::ITexture* light);
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
