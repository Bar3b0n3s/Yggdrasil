# 0017 — M9 rendering contract

- Status: reviewed public contract; the freeze takes effect with its reviewed, passing contract-mode commit. Marked scaffolds do not establish implementation completion.
- Scope: Roadmap M9 only. M10 runs in parallel. No implementation milestone is declared complete.
- Worktree: branch `codex/m9-renderer`. No commit or push belongs to this assignment.
- Authority: Architecture sections 3, 8.2–8.14, 13.4–13.7 and 15; Roadmap M9; ADRs 0004, 0009, 0011, 0013 and 0016.

## 1. What is present

Public value types and documented APIs, marked compiled C++ stubs, and failing-by-design skipped acceptance cases. Existing M8 behavior is kept. New debug names are recognized, but rendering and screenshot requests still return Unsupported until M9 implements them. A nondefault M9 extraction/render option is explicitly refused by a marked contract guard. Existing tests are not skipped or weakened: the debug-name test now checks all three new names, while the old screenshot refusal tests remain valid until implementation changes the behavior.

The contract does not call its pass factories, register its three new methods, change the shader manifest, change existing pipeline counts, or route the application's live views through the new stubs. Those are implementation/integration tasks. In particular, declarations for project settings/statistics and viewport dimensions are not completed host services. Each stub returns Unsupported (or a neutral non-Result value) and carries ENGINE_CONTRACT_STUB. Full PreCommit is reserved for the parent's schedule, not run concurrently with another worktree gate.

No dependency, upstream code, asset or license was added. No files outside the assigned worktree were edited. Only the existing premake executable was copied from the parent checkout to this worktree's ignored tool directory.

## 2. Frozen interfaces shared with M10

`Renderer/RenderSnapshot.h` remains free of NVRHI and Scene includes. Additions:

- `RenderViewFlags`: None, EditorOverlays, Grid, Selection, Colliders, Icons, Picking, Wireframe. EditorOverlays gates Grid, Selection and Icons; Colliders, Picking and Wireframe are independent. Clean captures and game views default to None.
- `RenderSnapshot::PickTable`: PickId minus one maps to UUID. MeshDrawItem gains PickId, zero for unpickable. Assign once in canonical mesh-entity order before culling or draw sorting; submeshes of an entity share its id. No EnTT handles.
- `SelectedEntities`: copied UUIDs, canonicalized and deduplicated during extraction, stale ids ignored.
- `RenderAnnotations`: labels None/All/Selected/Explicit, bounds, axes. Collider annotations map to Flags::Colliders. These apply to one capture, never persistent editor options.
- `RenderIcon`: renderer-owned kind, world position, color, pixel size and UUID. Camera, three light kinds, AudioSource, AudioListener. Plain data and procedural renderer glyphs, no UI font dependency.
- `RenderQualitySettings`: ShadowMapSize and SsaoHalfResolution, copied by the host from ProjectSettings.Rendering. No hardcoded replacement for project settings. Every host and capture path must pass them.
- `FrameIndex`: explicit host frame for this view's observations. Screenshot renderers use their own frame history.
- `RenderDebugView` appends AO, ShadowCascades and Overdraw after Emissive without changing existing values.

`RenderExtractionRequest` carries Flags, SelectedEntities, Annotations and Quality. `ExtractRenderSnapshot` stays the sole ECS extraction API; there is no second public extraction helper introduced merely to accommodate stubs. The implementation extends that existing function to copy settings, fill pick ids and extract all directional shadow fields. The contract currently refuses nondefault new options.

`SceneRenderer` gains:

- GetEntityIdTexture: borrowed R32_UINT target after a Picking view; null otherwise.
- GetViewGeneration: nonzero monotonic generation, changed by resize or CancelPicks. The host cancels on camera changes, scene/play-target replacement, geometry-option changes and hidden/minimized transitions; M10 image.Generation uses this exact value.
- RequestPick(PickRequest) and PollPick(PickTicket, currentFrameIndex).
- CancelPicks, called on scene replacement, play-scene switch, viewport invalidation.
- GetRenderStats: independent value copy for this renderer.

M10 renders to framebuffer pixels. Convert ImGui's logical click coordinates by the actual viewport framebuffer scale before issuing PickRequest. Suppress picking while ImGuizmo is hovered or active. Consume only the latest click sequence whose scene revision/view generation still matches, then resolve the UUID in the current scene before selecting it. M9 cannot resolve a deleted entity because Renderer must not include Scene.

M10 owns EditorContext camera/options and viewport extent storage, viewport.camera/frame/setOptions, editor.state, editor.screenshot UI freshness, EditorApp panel wiring, and Autosave. M9's `EditorMethodContext::GetViewportPixelSize(ViewportView)` is only the adapter to that storage. Its headless default is 640 by 360, independent of any screenshot size; zero/minimized extents are InvalidState. The accepted adapter reads `EditorContext::GetViewportState().GetPixelSize(view)`: independent actual scene/game extents default 640x360; hidden/minimized is unavailable. Pending panel sizes and retained camera aspect never replace the displayed extent.

## 3. Shadows

One shadowed directional light per view: the first eligible visible directional light in canonical order, consistent with the single four-layer ShadowCascades target. Every other directional light still illuminates; it does not receive a second directional shadow allocation. The parent contract owner accepted this resolution of the otherwise unspecified multiple-directional-light budget.

LightData gains ShadowDistance, CascadeCount, CascadeSplitLambda, DepthBias and NormalBias. These copy DirectionalLightComponent verbatim; defaults for spot bias are 1. ShadowMapSize comes from the snapshot quality copied from the project, range 256–8192, power of two.

ShadowCascades provides pure split, generic frustum-slice-corner and stabilized fitting functions. Perspective uses finite slice planes despite the infinite reverse-Z projection; orthographic uses box corners. Practical split lambda, sphere fit, radius padded for snapping by `R = ceil(16*r/(1-1/N))/16` (double intermediates, upward float conversion), texel snapping, near extension for offscreen casters, depth-clamp pancaking when available, ten percent cascade blending and distance fade. Snap all three light-space centre coordinates to nearest texels (ties toward +infinity). With texel width `2R/N`, each coordinate moves at most `R/N`; `R >= r + R/N` guarantees containment in the fixed light-space box AFTER snapping. Extend near Z toward casters only after fitting this box; never translation-refit the radius. Snap the light-space Z centre too so the full matrix remains unchanged inside a snap cell. Tests choose an interior point of a cell; an arbitrary sub-texel translation can cross a cell boundary and is not promised invariant.

PCSS is implemented in-house: 16 Vogel blocker taps, 32 comparison-filter taps, fixed per-pixel IGN rotation, no temporal noise. Convert reverse depth to light distance. Directional softness is world penumbra (receiver minus blocker distance times tan(LightAngle/2)) converted to UV per cascade. Clamp filter radius to at least 1.5 texels and its cascade/tile maximum. Spot softness uses SourceRadius and the perspective-light geometry, not the directional formula.

SpotShadowAtlas uses a 4096-square D32 target, eight at most 1024-square tiles, two guard texels per edge, row-major assignment. Eligible visible spots use CullLights' importance, ties UUID then snapshot index. Excess lights remain illuminated without shadows; Dropped counts them and the renderer warns once per view with RENDER_SPOT_SHADOW_BUDGET. No point-light shadows.

ShadowPass receives the full snapshot caster list, not the camera-culled draw list. It culls independently per light. Six pipeline variants preserve Opaque/Mask discard, mirrored transform culling and DoubleSided behavior from M8. Blend does not cast. Per-view bindings/targets belong to the caller, shared pipelines own no per-view caches. Depth is reverse-Z throughout.

New `Shared/ShadowConstants.h` fixes set-0 b1's C++/Slang layout with size/offset assertions: four cascade matrices and scalar rows, eight 128-byte SpotShadowConstants, directional parameters and counts. Light indices in GPU constants address the uploaded visible Lights list, not original snapshot indices. The integrator performs that mapping. Shader manifest and reflection-test registration remain with the integrator.

Primary algorithm references for implementation (no code copied):
- Jimenez et al., [Practical Realtime Strategies for Accurate Indirect Occlusion](https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf).
- NVIDIA, [PCSS integration](https://developer.download.nvidia.com/whitepapers/2008/PCSS_Integration.pdf). Perspective-light formulas require the directional adaptation above.

## 4. Depth pyramid and GTAO

DepthPyramidPass owns two shared pipelines, with per-view R16_FLOAT targets: projection-aware linearization and conservative minimum reduction, at most five mips. For source extent S and native next mip D=max(1,S/2), output i reads `[floor(i*S/D), ceil((i+1)*S/D))` in each axis, with uint64 intermediate products. This gives 2x2 reads for even sizes and up to 3x3 for odd sizes, intentionally overlapping fractional boundaries and covering the final row/column. Small/one-dimensional targets use their actual mip count. Reverse depth zero is the 65504 background sentinel for both projections (including exact far-plane zero, as in the existing skybox). Positive depths use near/d or far-d*(far-near), saturated to half-float range. View reconstruction uses the projection kind, not a perspective assumption.

GtaoPass owns main and bilateral-denoise pipelines. Low/Medium/High use one/two/three slices with three steps per side, in-house horizon integration and thickness heuristic, fixed IGN orientation, then two edge-aware bilateral denoise axes. Half resolution is Low OR project SsaoHalfResolution; extents round up, minimum one. Final sampling uses depth-aware upsampling to avoid halos.

AO combines with material AO by min and modifies indirect lighting only. Diffuse uses the multibounce fit; specular uses AO, NdotV and roughness. Disabled AO yields white. No temporal reprojection, history buffers or simulation changes. AO debug view shows the final occlusion value directly.

## 5. Picking and selection

AsyncPicker owns a bounded pool of eight staging/query/command-list slots. Request is called only after the matching render submission is notified, records and submits its own one-pixel copy, and retains that request's PickTable copy, SceneRevision, Sequence, FrameIndex and ViewGeneration. It uses GraphicsDevice wrappers (CreateStagingTexture, CreateEventQuery, completed submission id), never blocking Readback::ReadTexture.

Poll cannot succeed before two host frames have elapsed AND the copy has completed. It never maps incomplete memory or waits. Null optional means pending. Ready results are consumed once. Unknown/consumed tickets return NotFound. Cancellation/generation mismatch returns Cancelled while a cancellation tombstone exists.

CancelAll retains in-flight resources until retirement. Request and Poll collect retired cancelled slots even if the caller never polls the old tickets. At most eight cancellation tombstones are kept, oldest evicted; after eviction an old poll gets NotFound. Queue full returns Conflict, never a stall or silent overwrite. The generation and sequence prevent a stale result selecting an entity from another scene or an older click. Parent review should confirm this bounded cancellation behavior before freezing.

GPU EntityId follows Opaque/Mask prepass coverage; no transparent or text ids. CPU queries intentionally intersect geometric triangles, including Blend and Mask, without texture-alpha sampling; that distinction is documented rather than promising pixel-exact CPU/GPU agreement at transparent texels.

SelectionPass writes full-resolution R8 SelectionMask (1 visible, .5 occluded), dilates separably and composites solid visible/dim occluded edges onto LDR. Six mesh variants preserve M8 alpha/cull parity; selected Blend geometry uses geometric coverage. Two compute pipelines perform dilation and composite. Deselection clears stale masks. Every target/binding lives per view and retires safely on resize.

## 6. Overlays, annotations and raycasting

AppendEditorOverlay is pure renderer-side grid/icon tessellation into a caller-owned DebugDrawList. Grid is XZ, spacing in powers of ten, axis emphasis and distance fade, valid with both camera projections. Renderer world glyphs are separate from M10's Editor/Icons.cpp UI glyphs; neither implementation includes the other. Existing DebugRenderer/TextRenderer render the resulting lines and labels.

AppendRenderAnnotations is the real asset-dependent operation after extraction. It reads snapshot flags, selection, annotations and Alpha, then appends capture-local DebugDraw commands and plain icon records. Labels carry entity name and six-character UUID prefix in owned DebugText strings. Bounds come from CPU mesh AABBs transformed at the rendered pose. Axes use RGB. Collider geometry comes from BuildColliderDebugDraw with ColliderDebugDrawOptions.Alpha exactly equal to snapshot.Alpha, then AppendColliderDebugDraw. Pass the play PhysicsSystem to classify sleeping/refused bodies; edit scenes use null. This closes ADR0016's collider-alpha seam without Renderer-to-Scene coupling.

RaycastScene uses CPU AABB broad phase then two-sided mesh triangles at the rendered world pose. Return the nearest hit within the inclusive request `[MinDistance, MaxDistance]` (scene.raycast uses MinDistance=0), exact-distance ties by UUID/submesh/triangle. It excludes effectively disabled, pending-destruction, invisible and null-mesh entities. Missing assets use the rendering placeholder; singular transforms/degenerate triangles do not hit. Result includes UUID, distance, point, geometric normal oriented against ray, barycentrics, submesh and local triangle index.

**Mask policy accepted by the parent contract owner:** MeshRenderer has no render-layer field. LayerMask uses the existing project physics layers; visual entity inherits the nearest enabled ancestor-or-self RigidBody.Layer, else its own CharacterController.Layer, else Default. Unknown names fall back to Default, as physics does. No physics world or actual collider is required. This resolves the underspecified scene.raycast layerMask without adding a component field.

Pixel rays use integer framebuffer x/y, top-left origin, centre x+.5/y+.5, exactly the pixel GPU picking copies. Perspective origin is camera.Position; orthographic origin is the near-plane point with constant forward direction. Camera range and matrix validity are checked; CPU math uses DetMath where needed. `ComputeViewPixelRayInterval` supplies the interval: perspective `[NearClip/c, FarClip/c]` with c the normalized ray's forward component; orthographic `[0,FarClip-NearClip]` relative to its near-plane origin. Narrow-phase traversal rejects out-of-interval triangles before selecting a winner, so clipped foreground cannot hide a farther valid hit.

## 7. Statistics and host adapters

RenderStatsHistory is per SceneRenderer and copies values; screenshots have separate histories. The renderer uses the existing Graphics/GpuProfiler pooled delayed nonblocking timer queries. It does not introduce another timer-query implementation. CPU frame and GPU source-frame indices remain separate, with explicit availability bits; an absent timing never appears as a fabricated zero measurement.

Stable pass names/order: Prepare, DirectionalShadows, SpotShadows, DepthNormal, DepthPyramid, GTAO, GTAODenoiseHorizontal, GTAODenoiseVertical, ForwardOpaque, Skybox, ForwardTransparent, Overdraw, Bloom, Tonemap, FXAA, SelectionMask, SelectionDilateHorizontal, SelectionDilateVertical, SelectionComposite, Wireframe, Overlays, Text. Only enabled recorded passes are listed. CPU counters remain tied to their CPU frame; GPU values are tagged by GpuFrameIndex. Timings never affect simulated state.

Targets come from existing RenderTargetPool. Each renderer's pool EndFrame must be notified after submission, including screenshot submission; stale targets and binding caches must not outlive resize/scene unload. The current Create signature stays stable: the implementation owns its per-view profiler/pool internally. The frozen SceneRenderer::OnSubmitted(frameIndex, submissionId) is the host notification after the list containing that Render executes, before another Render/Resize. It stamps the pool and profiling frame without waiting, including no-camera clears (which NVRHI may not retain) and partial/error recordings. With no pending GPU work it is a no-op, so the host need not guess whether a failed Render recorded commands. EditorApp, RuntimeApp, ViewportCapture and GPU fixtures must call it; the integrator owns these updates.

Application exposes `OnRenderSubmitted(frameIndex, submissionId)` after executing the scene/UI command list and before Present. During UI drawing the host only queues owned clicks identifying the displayed frame, table and generation. This callback first notifies every renderer used by that frame, then drains its queued picks before any later render/resize can replace their source. A skipped frame has no callback. Frame pacing records the device's last submission after the callback so copy submissions retire before slot reuse. Captures/thumbnails submit and notify their private renderers independently and cannot drain displayed-view clicks. M10's `RequestOffscreenUiFrame()` requests a complete UI frame through these same hooks on the next render phase even when minimized, using an offscreen target and the last nonzero UI size with no swapchain acquire/present; it never renders or advances simulation inside the request.

The parent also freezes `GpuProfiler::BeginFrame(slot, frameIndex)` and `GetLastFrameResult() -> GpuTimingFrame{Available, FrameIndex, Samples}`. The returned copy names the recorded frame that was collected, not the frame starting now. An incomplete query or unused slot returns unavailable with empty samples, never a relabelled previous result. Missing-query scopes remain absent. The existing single-argument BeginFrame uses a local monotonic sequence; callers do not mix forms. Collection requires the slot's prior submission to have retired and never waits itself. Parent-owned application and profiler acceptance scaffolds cover these handoffs.

StatsGet reports FPS, CPU and scheduler dropped time, entities/bodies/voices, independent scene/game view observations and allocation counts. ScriptAvailable is false until M13 supplies usage; report configured soft/hard limits without claiming heap measurement. No device still returns CPU/session observations. Allocation warning above 2000 is diagnostic, and MaxMemoryAllocationCount is the device limit.

The parent adds `GraphicsDevice::GetMemoryAllocationCount` to the frozen contract and owns its implementation and `MemoryAllocationCountTests`. Count successful native device-memory allocations and actual frees, including NVRHI internal staging/heaps and host-image blocks; GPU-object counts and `VkAllocationCallbacks` (host allocations) do not measure this. The implementation can instrument the existing shared Vulkan dispatcher at device initialization, retaining/restoring original entry points through teardown. This stays within the existing process-level Vulkan-dispatch lifetime, requires no vendored-source change and must cover failed allocations and sequential devices. No GPU wait is introduced for statistics.

AutomationMethodContext adds GetProjectSettings (borrowed only for the request), GetHostStatistics (value Result), and GetSelectedEntities (owned UUID vector for capture annotations, empty in Runtime). Editor and Runtime must implement them; current marked defaults are not completion. Runtime settings come from PlaySession::GetProjectSettings and stats from its own game renderer, not the editor.

## 8. Automation wire contract

All handlers use the existing typed registry, descriptions, strict camelCase members, presence-aware target default, located errors and method coverage.

| Method | Required | Behavior | Flags |
|---|---|---|---|
| scene.raycast | origin, direction, maxDistance | CPU geometric hit; optional layerMask,target | read-only, dry-run capable, batchable, Runtime; not launcher/tool |
| viewport.pick | x,y,view | current camera and framebuffer extent, CPU hit; optional target | read-only, dry-run capable, batchable; editor only; not launcher/tool |
| stats.get | none | no waits or mutations, returns latest observations | read-only, dry-run capable, batchable, Runtime; not launcher/tool |

scene.raycast returns hit plus EntitySummary, position, normal, distance, barycentric, submesh and triangle; a miss has hit false and empty entity/zero geometry. viewport.pick adds view and width/height around that raycast result and never changes selection. No raw uint64 JSON values are rounded: stats frame identifiers are decimal strings.

viewport.screenshot keeps its existing outer wire fields and Variant annotate. ParseViewportAnnotations accepts Architecture's labels `all|selection|[EntityRefs]`, plus `none` and the `selected` alias of `selection`; colliders/bounds/axes are booleans. An absent label member disables labels; [] is Explicit with no labels. The pure parser owns up to 1000 nonempty references. ResolveViewportAnnotations then resolves all refs against the selected screenshot target via the existing resolver, retaining located `/annotate/labels/index` errors, and only on complete success copies sorted/deduplicated UUIDs into RenderAnnotations.LabelEntities. Runtime supports explicit IDs; selection is empty there. Non-Explicit modes keep LabelEntities empty; the screenshot host separately fills SelectedEntities from GetSelectedEntities for the target scene. Unknown/type errors locate /annotate/member. Empty object disables annotations. New debug views remain refused by the existing handler until integration; remove the later-milestone guard only when captures truly render them. The editor's editor.screenshot wrapper belongs to M10 and is untouched here.

Implementation updates RegisterSharedMethods and RegisterEditorMethods, their exact-method-list tests and Runtime exported tests. Regenerate catalog.json because viewport_screenshot's debugView/annotate descriptions change, even though the new three methods are engine_call-only. Do not manually expand the MCP tool list.

## 9. Exact implementation ownership

All public declarations and documented behavior in this ADR are frozen only AFTER parent review. A stream can add private implementation fields freely; changing these public contracts routes to the parent. New helpers stay inside its listed unit or a uniquely named private file approved by the parent.

| Owner | Implementation files and tests |
|---|---|
| A shadows | Renderer/ShadowCascades.h/.cpp, SpotShadowAtlas.h/.cpp, ShadowPass.h/.cpp; tests ShadowCascadesTests, SpotShadowAtlasTests, ShadowPassTests; implementation-owned new Passes/Shadow.slang and Common/Shadows.slang after integrator creates manifest entries |
| B GTAO | Renderer/DepthPyramidPass.h/.cpp, GtaoPass.h/.cpp; tests DepthPyramidPassTests, GtaoPassTests; new Passes/DepthPyramid.slang, Passes/Gtao.slang, Common/Gtao.slang and their private constants after integrator approves shared declarations |
| C picking and overlay | Renderer/AsyncPicker.h/.cpp, SelectionPass.h/.cpp, EditorOverlay.h/.cpp; tests AsyncPickerTests, SelectionPassTests, EditorOverlayTests; new Passes/Selection.slang after integrator creates manifest entry |
| D scene, methods and stats | Scene/SceneRaycast.h/.cpp, RenderAnnotations.h/.cpp (including EvaluateRenderSceneValidation); Renderer/ViewProjection.cpp and RenderStats.h/.cpp; Automation/Methods/RaycastMethods.h/.cpp, StatsMethods.h/.cpp, ViewportAnnotations.cpp, RenderMethodServices.cpp; EditorCore/Automation/ViewportPickMethods.h/.cpp; matching new tests under Scene, Renderer and Automation; Tests/Automation/test_renderer_ii.py |
| Integrator | existing RenderSnapshot.h/.cpp, SceneRenderer.h/.cpp, SceneRendererQueries.cpp, SceneTargetFormats.h, Scene/RenderExtraction.h/.cpp, Scene/ColliderDebugDraw if needed; Private/ForwardPipelines and LightingInputs, ViewportCapture, Shared/ShadowConstants.h and other shared shader structs, Passes/Scene.slang, Shaders.json, all binding/reflection and pipeline-count tests; RenderViewContractTests, ShadowConstantsTests, RendererIIGoldenTests and golden candidate review; existing ScreenshotMethods.h/.cpp and its C++/Python tests; AutomationMethodContext.h, EditorMethodContext.h and host overrides; register files/catalog; ProjectValidator no-lighting and spot-budget code/adapters; EditorApp/RuntimeApp/PlaySession seams jointly coordinated with M10; all build files/ModuleRules/root docs |

The Tests paths mirror their source include root unless explicitly named. Golden generation is a shared integration activity; streams supply deterministic scene setup and oracle proposals, not conflicting edits to the one golden file. A/D request LightData extraction changes through the integrator. C requests EntityId prepass MRT and selection hooks through the integrator. B requests forward AO binding/application through the integrator. Streams do not edit Shaders.json or shared scene shaders concurrently.

## 10. Acceptance and remaining integration

Scaffolding names every Roadmap acceptance CPU/GPU case and all nine goldens, plus stale/cancelled picker, mask/cull parity, half-resolution, per-view stats, annotation-alpha and settings propagation regressions. Python scaffolding names test_stats_report_pass_timings and test_viewport_pick_deterministic plus exported Runtime raycasts and capture-local annotation checks. All new skipped bodies fail if forced to run; removing a decorator without implementing the case cannot produce green.

Before milestone completion: every marker and non-child skip removed; Runtime methods and host services wired; project rendering settings observed in edit/play/capture/export; no screenshot stats eviction; async cancellation reclamation and shutdown tested; schemas/catalog regenerated; RENDER_NO_LIGHTING and RENDER_SPOT_SHADOW_BUDGET validator codes and fixtures registered; shader reflection checks cover all new programs/constants; candidate goldens reviewed; 1080p timing logged without a speed gate; no M8 behavior or tests weakened; strict local PreCommit and CI green including both Vulkan API caps and clang-cl portability. Remote CI remains non-blocking (ADR0011).

Parent-owned documentation amendments: Architecture sections 8.2–8.4, 8.7–8.10, 8.13–8.14 and 13.5–13.7; Roadmap M9 status/details; add-automation-method Runtime list for scene.raycast/stats.get. Remove interim M8/M9 scaffold comments once implementations land. No root documentation amendments have been applied by this worker.

## 11. Initial contract-worker verification (before review corrections)

Parent interface review joined M9's pixel-size adapter to `EditorContext::GetViewportState().GetPixelSize(view)`. M10 stores independent actual scene/game image extents, initially 640x360. A hidden/minimized view is unavailable; the retained camera aspect is never substituted for its missing image. The host publishes a new extent only after the corresponding image is rendered. Screenshot captures never change these extents.

Run from this worktree on Windows with VULKAN_SDK set to the installed 1.4.350.0 for each command session:

- Generate.py: passes; new files enter the VS2026 project and compilation database.
- Format.py --check on changed/new C++ and shared headers: 63 files, passes.
- Lint.py --allow-contract-stubs on changed/new code: all steps pass with zero findings, including clang-tidy naming/syntax and clang-query JSON checks. It reports 66 marked stub sites and 64 skip decorators allowed in contract mode (the Python class decorator covers four cases).
- Header self-containment: 22 affected headers pass independently with Clang 22.1.3.
- Actual object compilation with the generated Debug arguments: 41 changed/new C++ translation units pass, zero warnings/errors. After the final selection-service addition, RenderMethodServices.cpp was compiled again and passes.
- git diff --check: clean.

This is focused compile/static verification, not linked MSVC executables, shader compilation/reflection, executed acceptance cases, full PreCommit, or CI. Those remain parent-scheduled checks. No new GPU behavior is claimed implemented. No files staged, committed or pushed by this worker.

## 12. Review corrections and frozen integration dependencies

These are contract declarations, Unsupported stubs and skipped failing test scaffolds, not feature implementations. The eight review findings and post-snap containment are addressed as follows. Parent owns the combined contract gate and all four Application/GpuProfiler source/header files; this patch does not edit them.

### Submission and image identity (finding 1)

Parent froze `Application::OnRenderSubmitted(uint64_t frameIndex, uint64_t submissionId)`, called after scene/UI Execute and before Present. UI Draw enqueues only owned click data. EditorApp calls `SceneRenderer::OnSubmitted(frameIndex,submissionId)` for each view actually rendered, then drains the clicks against that submitted image's copied PickTable, FrameIndex, SceneRevision and generation. RequestPick rejects an unsubmitted image with InvalidState and a mismatched image tuple with Conflict. It never substitutes a later table/camera. The host fills snapshot.FrameIndex and SceneRevision before Render; camera/view invalidation calls CancelPicks. Minimized/skipped frames do not notify a render that did not happen; parent RequestOffscreenUiFrame uses the same hook for an actual forced scene/UI frame. Captures and thumbnails notify their private renderer directly and never replace live image extents/identities. The application pacer covers the device's last submission AFTER pick copies from the callback. Parent owns callback/retirement GPU tests; M9 adds identity rejection cases. AsyncPicker cases are GPU-suite tests because its public API needs a GraphicsDevice; they must use the standard headless GPU fixture when implemented. This also fixes M10 camera-generation versus renderer-generation ambiguity.

### Borrowed recording context and frame-aware samples (findings 2 and 3)

RenderRecordingContext in RenderStats.h/.cpp borrows the per-view stats and optional Graphics GpuProfiler; it owns a monotonic CPU diagnostic-clock callback. It is stack-owned during Render, main-thread-only, nonnested, and never retained by shared passes. BeginPass/EndPass name each actual recorded subpass, measure CPU elapsed time and saturating counters, and wrap its GPU work in the existing profiler. Repeated same-name scopes accumulate (e.g. multiple lights); disabled work has no row. Errors close scopes and count only work recorded. ShadowPass, DepthPyramidPass, GtaoPass and SelectionPass explicitly borrow this context. The integrator wraps existing M8 pass calls the same way. No Graphics header includes Renderer.

Parent froze `GpuProfiler::BeginFrame(slot,frameIndex)` and `GetLastFrameResult()->GpuTimingFrame {Available,FrameIndex,Samples}` (owned); unavailable/no-prior collections return no samples and FrameIndex is meaningful only if Available. The one-argument BeginFrame keeps a local sequence and is never mixed with the explicit form. M9 forwards this result to `RenderStatsHistory::PublishGpu(const GpuTimingFrame&)`; it never guesses a retired frame or reads the legacy last-samples vector. Unavailable clears GPU availability, not CPU counters. Available delayed samples retain their source index and match by name; old frames are ignored and missing/newly-enabled scopes remain unavailable. Before reusing a profiling slot, SceneRenderer verifies its submission completed; if busy it records CPU/markers without GPU timing and does not recycle pending queries. Parent owns Graphics metadata/failure tests, M9 owns history/subpass tests.

### Clipping, labels and odd-depth mips (findings 4 to 6)

The clipped-ray helper, MinDistance field and per-candidate rejection freeze correct perspective and orthographic intervals. New tests cover off-axis far geometry, near-clipped foreground, inclusive endpoints and independent actual scene/game extents. Label parsing and separate target-scene resolution support the exact Architecture protocol, aliases and indexed atomic failures. Explicit UUID storage is distinct from UI selection; bounds/axes/colliders flags do not inherit label filtering. The conservative footprint helper freezes odd mip arithmetic and its independent GPU oracle includes 5x3, 1x5, 5x1 and a near sample only at the final corner.

### Validation (finding 7)

Stream D owns EvaluateRenderSceneValidation in Scene/RenderAnnotations. Parent integrator owns ProjectValidator adapters/GetCodes, exact-list tests and generated reference wiring. Both diagnostics are Warning, not automatically fixable, keyed once per scene file without an entity. NoLighting requires a visible enabled non-null mesh with a nonemissive material, zero effective ambient, and no contributing enabled light. Effective environment is the first enabled Environment or RenderEnvironment defaults: a positive loaded map SH DC term at positive intensity, or the renderer's effective positive fallback when no usable map is present. A skybox background alone is not indirect lighting. Missing resources use renderer CPU placeholders and keep their separate asset diagnostics. Empty/text-only scenes and scenes where every visible submesh has potential emission do not warn; potential emission is max(Emissive*EmissiveStrength)>0 without inspecting texture texels. Emission never lights other nonemissive meshes. There is no invented Unlit material field. Shadow budget counts all enabled, finite contributing spots with CastShadows (not only the current camera's visible subset), warning above eight; actual per-view allocation still follows CullLights. Open and scratch-loaded scenes use the same checks. Tests include Basic3D positive illumination, black fallback, emission mixed with nonemissive meshes, and stable warning IDs/no undo changes; AUDIO_NO_LISTENER remains a separate warning, never an error.

### Debug outputs and wireframe (finding 8)

All three new data views (AO/ShadowCascades/Overdraw) have exposure 1, no OETF, bloom, FXAA, dither, ordinary editor overlays or text. Existing M8 material views keep their documented overlay/text behavior. Explicit capture annotations may append afterwards. AO emits the final denoised, depth-aware upsampled GTAO value in RGB before material AO/multibounce; disabled AO and background are white. ShadowCascades emits receiver cascade colors 0 red, 1 green, 2 blue, 3 yellow; blends over the same last ten percent of each cascade and fades to black over the final ten percent of ShadowDistance. Background/outside range/no allocated directional shadow is black. Palette values are display-encoded unit primaries, provided by pure palette helpers for tests.

Overdraw counts rasterized mesh fragments BEFORE depth rejection, with the same winding/cull rules and viewport clipping as M8. Opaque counts, Mask counts only after alpha discard, Blend counts only for alpha>0. Text/shadows/overlays never count. Dedicated additive RGBA16_FLOAT coverage (ONE+ONE, one in R), no depth attachment/test/writes and no atomics; counts >=5 share one color, so finite half precision saturation above that does not alter the displayed result. RGB palette: 0 black, 1 blue, 2 cyan, 3 green, 4 yellow, >=5 red. It does not run expensive lighting/shadow/AO work solely for overdraw; required solid EntityId/depth may still run for Picking. The integrator owns accumulation target, shader specialization/pipeline declarations and count/reflection tests. It must not implement Overdraw as ordinary depth-tested forward shading.

Wireframe applies only to Lit and replaces shaded mesh interiors with camera ClearColor, suppressing skybox/bloom/FXAA; it draws white one-pixel LineList triangle edges after output encoding, then requested selection/grid/icons/text. AppendWireframeOverlay resolves CPU meshes and emits canonical edges, with normal mirrored/double-sided culling, degenerate rejection and existing DebugDrawList budget/drop reporting. This is the portable path on every device, including devices without polygon-line support; no optional fillModeNonSolid feature is required. Solid depth/EntityId prepass still runs so hidden edges are depth tested and picking stays solid, including Mask discard. Mask and Blend wire edges use geometric edge coverage, intentionally not sampled texture alpha; transparent edges test opaque/mask depth and do not write it. Non-Lit debug views override Wireframe. CPU palette/edge tests, GPU coverage tests and Python screenshot cases exercise every new view, overlay precedence, mirrored/masked/blended geometry, unchanged picking and both API caps.

### Cascade containment (additional review correction)

ComputeStabilizedCascadeRadius supplies padding before quantization/snapping, including conservative float conversion. A wide 1000:1 orthographic view at 256-square shadows exercises worst-case snap displacement and verifies all XYZ corners after snapping. The stable radius depends only on the slice shape, preserving within-cell matrix stability; translating the camera never triggers a post-snap radius refit.

### Follow-on ownership

A: corrected cascade containment; B: recording context consumption, odd footprints and AO debug oracle; C: submission identity, portable wireframe and selection scopes; D: RenderRecordingContext/RenderStatsHistory, clipped-ray math, palette helpers, annotation resolution and render validation; integrator: host submission callbacks, Graphics profiler metadata, debug GPU pipeline/targets, ProjectValidator integration, shared shader/build/catalog edits, complete frame/stats host wiring and combined gates. Keep root parent amendments when applying this patch, especially accepted shadow/mask policies and GetPixelSize semantics.


### Review-correction verification

Focused checks on the corrected worktree, with VULKAN_SDK set per command to the installed 1.4.350.0:

- Format.py --check: all 42 affected C++ headers/sources pass with clang-format 22.1.3.
- Lint.py --allow-contract-stubs --paths on all 43 affected code files: zero findings across includes, banned APIs, contract markers/skips, clang-tidy naming/syntax, header self-containment, Python and JSON. Sixteen headers were compiled independently for self-containment; another thirteen were inspected through including sources for naming. Contract mode reports 58 marked stubs and 69 skip decorators in this subset, including the Python class decorator.
- git diff --check: clean. The patch adds 23 named C++ acceptance scaffolds and four Python cases; existing executable tests were not weakened. AsyncPicker tests use the GPU suite to match their device-dependent public API.
- No new untracked files; the unstaged patch includes every correction. The initial staged contract is unchanged. Application.h/.cpp and Graphics/GpuProfiler.h/.cpp are untouched by this worker.

No linked build, executed acceptance suite, full gate, remote CI, feature implementation, staging or commit is claimed for these corrections. The parent owns combined contract verification and application of the exported unstaged patch.
