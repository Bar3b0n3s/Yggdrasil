#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/AsyncPicker.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The scene renderer (Architecture §8.2, §8.3): it renders a RenderSnapshot, never the ECS, through a fixed pass list (no
// render graph). The pass list (Roadmap M8/M9; Docs/Decisions/0013-m8-decisions.md and 0017-m9-contract.md):
//   1. Prepare (CPU, RenderPrepare.h): every MeshDrawItem resolved through the GpuResourceCache (meshes, materials with their
//      textures, the environment; placeholders for missing assets, §7.2), submeshes culled against the view frustum by their
//      bounds, opaque draws (AlphaMode Opaque and Mask) sorted by pipeline, then material, then mesh, transparent draws
//      (AlphaMode Blend) back to front with ties by entity UUID; the lights culled and packed, at most MaxVisibleLights,
//      logging RENDER_LIGHT_LIMIT_EXCEEDED once per renderer when more are visible; the per-view constants written
//      (ViewConstants b0, EnvironmentConstants b2, the Lights structured buffer t0).
//   2. Directional cascades and 3. spot shadow atlas: shared shadow pipelines, per-view depth targets and constants (b1).
//   4. Depth/normal prepass: SceneDepth (reverse-Z) and SceneNormals (octahedral view-space normals, normal-mapped); Mask
//      materials discard below AlphaCutoff here and in the forward pass. Picking adds the same visible surfaces to EntityId.
//   5. Linear view-depth pyramid and 6. GTAO: projection-aware, with optional half resolution and edge-aware filtering.
//   7. Forward opaque: SceneColor cleared to the camera's ClearColor; PBR (§8.5: GGX, height-correlated Smith, Schlick,
//      Lambert, multi-scatter energy compensation from the DFG LUT (BrdfLut.h), perceptual roughness >= 0.045, geometric
//      specular anti-aliasing) with every visible light (artist units, windowed inverse-square falloff, smoothstep cones,
//      SourceRadius widening) and IBL (§8.6: SH9 diffuse, split-sum specular from the prefiltered cube, specular and
//      horizon occlusion), or the constant ambient FallbackColor * Intensity without an environment map; emissive added.
//   8. Skybox (SkyboxPass.h): when the camera clears to the skybox and the environment has a map and ShowSkybox.
//   9. Forward transparent: the Blend draws sorted back to front, alpha blended, depth tested without writes.
//  10. Bloom (BloomPass.h): when PostProcessSettings::BloomEnabled.
//  11. Tonemap + encode (TonemapPass.h): exposure 2^ExposureEV, the snapshot's tonemapper, the sRGB OETF, blue-noise dither
//      (the BlueNoise built-in through the GpuResourceCache). The renderer asks for it only when
//      AssetManager::GetAssetType(BuiltinAssetHandles::BlueNoiseTexture) is AssetType::Texture, which records no
//      diagnostic, and passes no blue noise (no dither) when that check fails or the mirror IsPlaceholder: a manager
//      without the built-in (InMemoryAssetManager, a test's AssetTestFixture without the engine resources) must not log
//      GetOrPlaceholder's ASSET_MISSING error on every render.
//  12. FXAA (FxaaPass.h): when PostProcessSettings::FxaaEnabled, LdrColor into its ping-pong partner.
//  13. Wireframe edges, selection mask/dilation/composite, then grid/icons and the snapshot's DebugDrawList, depth-tested
//      against SceneDepth or on top. Grid/icons/selection require EditorOverlays and their respective flags.
//  14. Text (TextRenderer.h): world texts (depth-tested), then screen texts, then the debug list's labels.
// Debug views (RenderSnapshot::DebugView other than Lit) specialize the forward pipelines and fix the post chain: no skybox,
// bloom, FXAA or dither, exposure 1, the Linear tonemapper, the OETF only for Albedo and Emissive. Existing M8 material
// views retain overlays/text. M9 AO/ShadowCascades/Overdraw suppress them except explicit capture annotations (ADR0017);
// Overdraw uses a dedicated depth-independent additive pass, not the forward shading specialization.
// A snapshot without a camera clears SceneColor to the default ClearColor and draws nothing but screen texts.
// Rendering is deterministic for a snapshot on a given device (§8.3: no temporal effects), which the golden images rely on.
//
// Pipelines (§8.5, §8.12). SceneRendererPipelines holds every pipeline of the pass list, created once per device at startup
// and shared by every SceneRenderer of that device (Docs/Decisions/0012-m7-decisions.md decision 7): the scene's own mesh
// pipelines, {Opaque, Mask} prepass and forward variants and the Blend transparent variants, each for CullBack (front
// faces counter-clockwise, §8.3), CullFront (a mirroring world matrix, whose front faces turn clockwise on screen; the
// shaders flip the normal by DrawConstants::Flags) and CullNone (double-sided materials; the shaders flip back faces'
// normals), plus picking and overdraw, 24 in all; plus the passes it owns: SkyboxPass, BloomPass (for the device's Bloom
// format, §8.1), TonemapPass, FxaaPass, DebugRenderer, TextRenderer, shadows, depth pyramid, GTAO, selection, data views and
// BrdfLut (which generates the DFG LUT once at Create). Material binding sets
// (set 1) are shared by every variant through shared binding layouts (PipelineFactory.h). A non-Lit debug view's
// pipelines (the 9 forward opaque and transparent variants specialized with the view) are created the first time a
// snapshot asks for that view and kept: debug views are a debugging path, like ImGui's per-format pipelines (ADR 0009
// decision 25), and creating them at startup would multiply the startup count by six (decision 12): Render creates them
// through SceneRendererPipelines::EnsureDebugView, which a host may also call ahead of time. The pipeline count is logged at
// creation and asserted by "Pipelines: count matches the expected total".
//
// Binding sets. The passes the set owns keep none of their own: every SceneRenderer keeps one PassBindingCache per pass
// with its per-view targets, clears them in Resize, releases the sets its last Render did not use at the end of every
// Render, and destroys them with itself, so views never evict each other's sets and no view's old targets outlive it.
//
// Stale mirrors (§8.14 items 1 and 2; Docs/Decisions/0013-m8-decisions.md decision 7). Every host that renders (EditorApp,
// RuntimeApp) calls GpuResourceCache::CollectStale() and SceneRendererPipelines::CollectStale(assets) once per frame after
// the frame's renders were recorded and executed, which releases the mirrors of replaced asset versions (hot reload,
// reimport); and, after the first frame rendered once a scene was opened, closed or swapped (play mode included), the same
// two calls with releaseUnused = true, which releases what the previous scene alone used. StaleMirrorSchedule.h decides
// which collection that is, for both hosts.
//
// Main thread only (NVRHI recording, §4.11); not copyable or movable. Destroyed before the SceneRendererPipelines, the
// GpuResourceCache and the device (§8.14 item 4).
//
// M18 (C18.1, Docs/Decisions/0029-m18-contract.md decisions 1 to 8). Stages: Render runs one stage function per RenderStage
// (RenderStage.h) in order, each recording its passes in their declared order, between the GPU-zone hook's
// RenderRecordingContext::BeginStage and EndStage; the passes above keep their order, except that game Text now draws in
// the SceneOverlay stage, before the editor overlays (wireframe, selection, debug lines, annotations, grid, icons) of the
// EditorOverlay stage, and the debug and annotation labels draw last, in that stage (the "Labels" row). Pass declarations:
// RenderPassDeclaration.h declares every pass (name, pipeline count, programs, stats rows and position, targets); the stats
// order is RenderStatsOrder; every pipeline is created by SceneRendererPipelines::CreatePipelines, which startup, shader
// reload and device-loss recovery call. The output stage: the scene stages render at the snapshot's scene resolution
// (RenderOutputSettings, the renderer's size by default), the Output stage upscales the scene image to the renderer's size
// (UpscalePass.h; nothing is recorded when the sizes are equal, which renders exactly as before), and the EditorOverlay
// stage draws at the renderer's size; GetOutputMapping maps view pixels to scene pixels. The 2D colour path:
// RenderOutputSettings::PixelArt fixes the post chain to Linear at exposure 1 with the OETF, no dither, bloom or FXAA.
//
// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 26 and 27): device-loss recovery. SceneRendererPipelines and
// every SceneRenderer register themselves with their device's recreation registry (Graphics/GpuRecreation.h) at Create: the
// set releases every pass and is created again through CreatePipelines; a renderer releases its per-view objects and
// creates its size-dependent ones again. They are two of the placement rule's three places: a later milestone's pass is
// declared (RenderPassDeclaration.h) and owned by the set, and a later per-view object lives in SceneRenderer::State, so
// neither needs a registration of its own.

namespace Engine {

	class AssetManager;
	class GpuResourceCache;
	class GraphicsDevice;

	struct SceneRendererSpecification
	{
		// The size of the targets, both >= 1 (asserted).
		uint32_t Width = 1;
		uint32_t Height = 1;
	};

	// The legacy draw summary of the last Render; GetRenderStats supplies the per-pass detail.
	struct SceneRenderStats
	{
		uint32_t MeshDraws = 0;       // submesh draws recorded by the forward passes (opaque and transparent)
		uint32_t CulledSubmeshes = 0; // submeshes outside the frustum
		uint32_t Lights = 0;          // lights used after culling (at most MaxVisibleLights, RenderPrepare.h)
		// M8 additions.
		uint32_t TransparentDraws = 0;  // of MeshDraws, the Blend draws of pass 9
		uint32_t CulledLights = 0;      // LightCullResult::Culled: outside the view, or zero or non-finite radiance or range
		uint32_t DroppedLights = 0;     // visible lights beyond MaxVisibleLights (RENDER_LIGHT_LIMIT_EXCEEDED)
		uint32_t TextDraws = 0;         // text items and debug labels drawn
		uint32_t DebugLineVertices = 0; // vertices of the debug lines drawn
	};

	// The pipelines of the pass list (see the file comment), created once per device at startup and shared by every
	// SceneRenderer of that device. Destroyed after the renderers that use it and before the device. Main thread only; not
	// copyable or movable.
	class SceneRendererPipelines
	{
	public:
		// The scene's own mesh pipelines at startup: prepass {Opaque, Mask} x {CullBack, CullFront, CullNone} (6), forward
		// opaque the same (6), forward transparent (3), picking prepass (6), depth-independent overdraw (3).
		static constexpr uint32_t MeshPipelineCount = 24;
		// The pipelines one M8 material debug view adds: 6 forward opaque and 3 transparent variants. M9 data views use
		// their dedicated passes (ADR0017).
		// Data-view pipelines are included in StartupPipelineCount.
		static constexpr uint32_t DebugViewPipelineCount = 9;
		// Every pipeline Create makes: the mesh pipelines plus SkyboxPass (1), BloomPass (3), TonemapPass (1), FxaaPass (1),
		// DebugRenderer (2), TextRenderer (2), BrdfLut (1), ShadowPass (6), DepthPyramid (2), GTAO (2), Selection (8),
		// and the data-view composite (1). Each pass declares its own PipelineCount; this is their sum. M18 (C18.1): it
		// becomes GetDeclaredPipelineCount() (RenderPassDeclaration.h, 58 with the QuadBatcher and the UpscalePass) once
		// CreatePipelines creates the declared passes.
		static constexpr uint32_t StartupPipelineCount = MeshPipelineCount + 1 + 3 + 1 + 1 + 2 + 2 + 1 + 6 + 2 + 2 + 8 + 1;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SceneRendererPipelines;
		};

		// Use Create.
		explicit SceneRendererPipelines(ConstructionKey key);
		~SceneRendererPipelines();

		SceneRendererPipelines(const SceneRendererPipelines&) = delete;
		SceneRendererPipelines& operator=(const SceneRendererPipelines&) = delete;

		// Creates every pipeline of the pass list through `pipelines`, checking each layout against its reflection
		// (ValidatePipelineLayout), with their binding layouts and samplers, and the passes it owns (BloomPass for
		// device.GetInfo().BloomFormat; BrdfLut generates the DFG LUT, executing one command list). Logs the pipeline count at
		// Info. `device` and `pipelines` are documented back-references that outlive the set (EnsureDebugView creates
		// pipelines through `pipelines` after Create). Errors: those of PipelineFactory, the passes' Create and the
		// GraphicsDevice wrappers; a Gpu error is an out-of-memory creation, which a caller creating the set at startup turns
		// into FatalError(OutOfMemory) (§8.14 item 7).
		[[nodiscard]] static Result<Scope<SceneRendererPipelines>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// For M8 material views creates DebugViewPipelineCount forward variants; M9 data views ensure their dedicated
		// pass resources instead (ADR0017, integration). Variants are specialized with view unless they exist (Lit's are the
		// startup pipelines: no effect); logs the new pipeline count at Info. SceneRenderer::Render calls it the first time a
		// snapshot asks for a view, and a host may call it ahead of time. Errors: InvalidArgument for a view this build has no
		// pipelines for (not below RenderDebugViewCount); those of PipelineFactory, where a Gpu error is an out-of-memory
		// creation, which the caller turns into FatalError(OutOfMemory) (§8.14 item 7). Nothing is kept on failure, so a later
		// call tries again.
		[[nodiscard]] Status EnsureDebugView(RenderDebugView view);

		// Actual pipelines created so far: startup plus each lazily created material-view family, counted once.
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// M18 (C18.1, R-PASS; Docs/Decisions/0029-m18-contract.md decision 2): the one creation entry of the pass list's
		// pipelines. Creates every pass ScenePassDeclarations declares (RenderPassDeclaration.h), each with its PipelineCount
		// pipelines and its binding layouts, samplers and startup resources (the DFG LUT, the neutral textures, the
		// QuadBatcher's white texture), through the set's PipelineFactory, aside from the current ones. Only when every
		// creation succeeds do they replace the current passes: the lazily created debug-view pipelines are dropped
		// (EnsureDebugView creates them again on first use), GetGeneration increases and the count is logged at Info. On any
		// error nothing changes and the current pipelines stay in use. Create (startup), ReloadShaders (shader hot reload) and
		// the device-loss recovery (C18.3) create pipelines through it, and nothing else creates the set's pipelines; command
		// lists already recorded keep the replaced pipelines alive until they retire (§8.14 item 2). Main thread, with no
		// Render of the set's renderers awaiting its command list's close. Errors: those of PipelineFactory and the passes'
		// Create; a Gpu error is an out-of-memory creation, which a caller at startup turns into FatalError(OutOfMemory)
		// (§8.14 item 7), while ReloadShaders returns it.
		[[nodiscard]] Status CreatePipelines();

		// M18 (C18.1): 1 after Create, increased by every successful CreatePipelines. A SceneRenderer that last recorded with
		// an earlier generation clears its binding-set caches and its material sets before its next Render, since they were
		// made for the replaced binding layouts.
		[[nodiscard]] uint64_t GetGeneration() const;

		// M18 (C18.1, R-SHR, shaders.reload; decision 3): the editor's shader hot reload. The factory's ShaderLibrary begins a
		// reload (ShaderLibrary::BeginReload: every variant is read again from the compiled output, which CompileShaders.py or
		// a build of the Shaders project rewrote), then CreatePipelines runs. On success the library keeps what it read
		// (EndReload(true)) and the new pipelines are used from the next Render of every renderer of the set (ViewportCapture's
		// included). On failure the library restores its earlier variants (EndReload(false)), the old pipelines stay in use,
		// and the error is returned with the context "while reloading the shaders; the previous pipelines stay in use". It
		// compiles nothing and starts no process (§13.2). Main thread, between frames. Errors: those of CreatePipelines
		// (Validation for a layout that no longer matches its reflection, NotFound for a variant the build did not write, Gpu
		// for a failed creation, which is not fatal here).
		[[nodiscard]] Status ReloadShaders();

		// The layout description of every pipeline Create makes, its passes' included, with the BloomPass pipelines for
		// `bloomFormat` (R11G11B10_FLOAT or RGBA16_FLOAT, §8.1), for the CPU-only reflection test ("Shaders:
		// LayoutsMatchReflection", PipelineFactory.h). A debug view's pipelines share their Lit variant's description.
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions(nvrhi::Format bloomFormat = nvrhi::Format::R11G11B10_FLOAT);

		// Releases what the owned passes cache per asset version (the TextRenderer's font atlases): mirrors of old versions,
		// and with `releaseUnused` those no Render used since the previous call (a scene unload, with
		// GpuResourceCache::CollectStale; "GpuResourceCache: live counts return to baseline after unloading a scene"). Hosts
		// call it as the file comment says.
		void CollectStale(const AssetManager& assets, bool releaseUnused = false);

		// M18 (C18.3, decisions 26 and 27): the set's Release hook in the recreation protocol (registered by Create, as
		// "SceneRendererPipelines"): drops every pass, pipeline, binding layout, sampler and startup resource, the lazily
		// created debug-view pipelines included, by resetting every member of the set's state except its back-references, its
		// registration and its generation, so a pass a later milestone adds to the state is released without code of its own. GetPipelineCount
		// is 0 afterwards. Its Recreate hook is CreatePipelines, which creates every declared pass again and increases
		// GetGeneration, so every renderer of the set clears its binding sets before its next Render. Main thread, from
		// GraphicsDevice::Recover; renders of the set's renderers in between are a programmer error (asserted).
		void ReleaseGpuObjects();
	private:
		// SceneRenderer records with the set's pipelines, passes, binding layouts and samplers (State, SceneRenderer.cpp).
		friend class SceneRenderer;
		// The back-reference, the pipelines, the passes, their binding layouts and the samplers (SceneRenderer.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	class SceneRenderer
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SceneRenderer;
		};

		// Use Create.
		explicit SceneRenderer(ConstructionKey key);
		~SceneRenderer();

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;

		// Creates the per-view buffers and the targets of the specification's size (SceneTargetFormats.h; LdrColor and its
		// FXAA partner, the bloom chain of BloomPass::GetChainDesc); it records with the shared `pipelines` and creates no
		// pipeline itself (Render extends `pipelines` through EnsureDebugView, which every renderer of the device then
		// shares). `device`, `pipelines`, `cache` and `assets` are documented back-references that outlive the renderer.
		// Errors: those of the GraphicsDevice wrappers; a Gpu error is an out-of-memory creation, which a caller creating the
		// renderer at startup turns into FatalError(OutOfMemory) (§8.14 item 7).
		[[nodiscard]] static Result<Scope<SceneRenderer>> Create(GraphicsDevice& device, SceneRendererPipelines& pipelines, GpuResourceCache& cache,
			AssetManager& assets, const SceneRendererSpecification& specification);

		// Recreates the size-dependent targets for `width` x `height` (both >= 1, asserted) and clears the renderer's binding
		// sets (which reference the old targets); no effect at the current size. Errors: InvalidState while a Render awaits its
		// OnSubmitted (also asserted); those of the target creation (Gpu: FatalError(OutOfMemory) for the caller, §8.14 item 7).
		[[nodiscard]] Status Resize(uint32_t width, uint32_t height);

		// Records the pass list for `snapshot` into `commandList` (open, asserted). The snapshot's camera viewport should
		// match the scene resolution (M18: RenderOutputSettings, the renderer's size by default); a different one renders at
		// the scene resolution with the snapshot's projection. Uploads
		// the assets it needs through the GpuResourceCache first, and creates a debug view's pipelines the first time it is
		// asked for (SceneRendererPipelines::EnsureDebugView; a Gpu error there is FatalError(OutOfMemory), §8.14 item 7).
		// Afterwards GetFinalTexture holds the image.
		// Errors returned before recording (nothing recorded, no OnSubmitted needed): InvalidState while the previous Render
		// awaits its OnSubmitted (also asserted); InvalidArgument for a debug view outside RenderDebugView or a
		// Quality.ShadowMapSize that is not a power of two in [256, 8192]; a debug view's pipeline error other than Gpu (an
		// invalid shader). Partial errors (the view is still recorded, the first error is returned): InvalidArgument for a
		// non-finite camera matrix (the view clears as without a camera) or world matrix (that draw is skipped, the error
		// names the entity), or invalid wireframe, overlay or selection data; Gpu when a binding set or a lazily created
		// target cannot be created. Data the schemas accept is never a render error: a missing asset draws its placeholder, a
		// missing environment the fallback ambient, and a shadow-casting light the shadow planners reject stays lit without
		// shadows (warned once per renderer).
		[[nodiscard]] Status Render(nvrhi::ICommandList& commandList, const RenderSnapshot& snapshot);

		// The LDR target holding the last Render's image (RGBA8_UNORM, display-encoded values, §8.9), at the renderer's size:
		// LdrColor, or its FXAA partner when FXAA ran, when the output mapping is the identity; otherwise OutputColor, the
		// upscaled scene image in its letterbox with the editor overlays (M18). What BlitPass presents and ViewportCapture reads
		// back.
		[[nodiscard]] nvrhi::ITexture* GetFinalTexture() const;
		// The renderer's size: the view's (GetFinalTexture's). The scene resolution is the snapshot's (GetOutputMapping).
		[[nodiscard]] uint32_t GetWidth() const;
		[[nodiscard]] uint32_t GetHeight() const;
		// M18 (C18.1, R-2D-PIXEL; decision 6): the output stage's mapping of the last Render, ComputeRenderOutputMapping of the
		// renderer's size and the snapshot's Output: which view pixels of GetFinalTexture show which scene pixels. The
		// identity at the renderer's size before the first Render and after Resize.
		[[nodiscard]] RenderOutputMapping GetOutputMapping() const;
		[[nodiscard]] const SceneRenderStats& GetLastStats() const;

		// M9: borrowed R32_UINT target of the last view with Flags::Picking, otherwise null. Valid until Resize/destruction.
		// Transparent geometry and text do not write EntityId; Opaque/Mask use the same discard and cull rules as depth.
		// M18: at the scene resolution (GetOutputMapping).
		[[nodiscard]] nvrhi::ITexture* GetEntityIdTexture() const;
		// Monotonic, nonzero; increments after actual resize or CancelPicks, never wraps/reuses a generation.
		// Host calls CancelPicks on camera change, scene replacement, play target switch and hidden/minimized transition.
		// M10 image.Generation is exactly this value; options that change visible geometry also invalidate it.
		[[nodiscard]] uint64_t GetViewGeneration() const;
		// UI drawing only queues an owned click. Application::OnRenderSubmitted(frameIndex, submissionId) first calls
		// OnSubmitted on each view recorded in that frame, then drains matching clicks before any Render/Resize.
		// Uses that submitted image's copied PickTable, FrameIndex, SceneRevision and generation, not a newer camera/table.
		// InvalidState before a submitted picking render (including a newer recorded-but-unsubmitted Render); Conflict
		// when request FrameIndex/SceneRevision/ViewGeneration differs from that image. A rejected stale miss never clears
		// selection. Other errors follow AsyncPicker::Request. Pixel coordinates are framebuffer
		// pixels, not logical ImGui coordinates. The UI suppresses requests while ImGuizmo is hovered/active.
		// M18 (decision 6): X and Y are view pixels of GetFinalTexture; the pick reads EntityId at MapViewPixelToScenePixel
		// of the submitted image's output mapping, and a view pixel in the letterbox picks nothing (a PickResult with the
		// invalid UUID and PickId 0, like the background).
		[[nodiscard]] Result<PickTicket> RequestPick(const PickRequest& request);
		[[nodiscard]] Result<std::optional<PickResult>> PollPick(PickTicket ticket, uint64_t currentFrameIndex);
		void CancelPicks();
		// Copy of this view's counters/timings, never a shared last-render value. Screenshot renderer owns separate history.
		[[nodiscard]] RenderStats GetRenderStats() const;
		// After the host submits the command list containing this view's Render, stamp this view's RenderTargetPool and
		// profiling frame with its actual submission. Main thread; nonzero submissionId belongs to this device (asserted).
		// Called for each rendered view, including ViewportCapture's private renderer, before another Render/Resize.
		// Does not wait. Required even for a no-camera clear, which NVRHI may not retain as a texture reference.
		// With pending recorded work, frameIndex must equal snapshot.FrameIndex (asserted); stamp once per Render, including
		// partial/error renders. Without pending work, no-op, so hosts can notify after an attempt that failed before recording.
		// Skipped frames receive no notification. Captures notify directly.
		// The application pacer covers GetLastSubmissionID AFTER its hook drains pick-copy submissions.
		void OnSubmitted(uint64_t frameIndex, uint64_t submissionId);

		// M18 (C18.3, decisions 26 and 27): the renderer's hooks in the recreation protocol (registered by Create, as
		// "SceneRenderer"). Release drops every per-view GPU object of the state, by resetting every member except its
		// back-references, its registration, its size, its stats history, its once-per-renderer log flags and its counters: the targets and the
		// output stage's targets, every target of its RenderTargetPool (RenderTargetPool::ReleaseAll), its binding-set caches
		// and material sets, its per-view buffers, its picker's staging copies and its profiler's queries; a pending Render
		// (awaiting OnSubmitted) is forgotten, the view generation increases (CancelPicks: every pending pick resolves as
		// cancelled), and GetFinalTexture and GetEntityIdTexture return null until the next Render. A per-view object a later
		// milestone adds to the state is released without code of its own. Recreate creates the per-view buffers and the
		// targets of the renderer's size again, as Create does, so GetFinalTexture is valid again; everything else is created
		// by the next Render. Errors: those of the creation (Gpu). Main thread, from GraphicsDevice::Recover.
		void ReleaseGpuObjects();
		[[nodiscard]] Status RecreateGpuObjects();
	private:
		// The back-references (the shared pipelines among them), the targets, the per-view and material buffers and the
		// stats (SceneRenderer.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
