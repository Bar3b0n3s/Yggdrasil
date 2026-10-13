# 0021 — Review fixes for M9, M10 and M13

- **Status:** accepted by the lead, acting as contract owner of the frozen M9, M10 and M13 interfaces and as docs owner, for the review-fix integration on `integrate/review-fixes`.
- **Date:** 2026-10-12
- **Base:** `81fe89e` (M9 and M10 completed in `c95862a`, M13 in `89487ac`, the ADR 0020 redesign in `81fe89e`).
- **Context:** M9 (Renderer II; contract `dd5d57d`, implementation `c95862a`; ADR 0017), M10 (Editor UI; the same two commits plus the ADR 0020 redesign `81fe89e`; ADR 0018) and M13 (Scripting; contract `8c066eb`, implementation `89487ac`; ADR 0019) each passed the strict gate. The lead then reviewed each milestone at `81fe89e`: five (M9) or six (M10, M13) independent review lenses read the code, and an adversarial verifier tried to refute every candidate finding against the code, the vendored sources and the authoritative documents. The review kept 185 verified findings: M9 42 (7 major, 25 minor, 10 nit), M10 78 (2 blocker, 23 major, 39 minor, 14 nit, including the lead's own drag-rounding finding `V1`) and M13 65 (3 blocker, 19 major, 31 minor, 12 nit). The product owner chose to fix every one of them, nits included. Eight parallel fix streams, each in an isolated worktree from `81fe89e`, fixed the findings in the files they owned, added a regression test for each behaviour fix, and wrote down their decisions and the edits they needed in files they did not own:
  - **renderer:** Engine `Renderer/`, the device setup and fault injection in `Graphics/`, the shadow, scene, debug-line and data-view shaders, `Scene/RenderAnnotations` and `SceneRaycast`, the engine's screenshot method and `AutomationMethodContext`, their tests and goldens, Architecture §8 and the Roadmap's M9 section.
  - **viewport:** the Scene and Game panels, `GizmoOverlay`, `EditorViewportHost`, `Editor/Private/EditorHostViewports`, `EditorCore/Viewport/`, the viewport methods, fresh editor screenshots, `EditorApp`'s input and screenshot paths, and the view-size seam of `PlaySession` and `RuntimeApp`.
  - **inspector:** `InspectorPanel`, `ReflectedDrawers`, `ReflectedEditController`, `SceneHierarchyPanel`, `EditorLayer`, `Editor/Ui`, `EditorActions`, `CommandHistory`, the selection in `EditorContext` and `EditMethods`, and the Undo History and Diagnostics panels.
  - **host:** `EditorCore/Autosave/`, `EditorHostRecovery`, `EditorApp`'s host lifecycle (close, error logging, shutdown), the automation server's mutation policy and the Automation panel, the recovery and project methods, the MCP bridge in `Tools/MCP/` and `test_editor_ui.py`.
  - **content:** the content browser, `ThumbnailCache`, `EditorHostThumbnails`, `ProjectLauncher`, `FolderPicker`, `ProjectManager`, `EditorPreferences`, the source-editor and preference host helpers, and `Platform/Process`.
  - **vm:** Scripting's `Sandbox`, `LuaHelpers`, `ScriptApiRegistry`, `ScriptProxy` and `Bindings/`, the new limit and time headers, the Luau build file, `CheckBuildConfig.py` and the two scripting skills.
  - **lifecycle:** `ScriptEngine` and its private instance, reload and state units, `ScriptImporter`, `ScriptTypeChecker`, `EditorScriptService`, the reload side of `EditorPlayController`, and the script fixtures and their generator.
  - **testing:** `FeatureTestRunner`, `TestResults`, the replay recorder, player and data, the play session's fatal state, the watchdog's script phase, the play and replay methods, the recording, replay and test side of `EditorPlayController`, `EditorFeatureTestHost`, `RuntimeApp`'s test paths, `SceneEdit`, and the Python script, replay and test suites.

  The lead merged the eight streams on `integrate/review-fixes`, and an integration pass applies the cross-stream follow-ups (decision 115). The three review records and the streams' decision notes are kept under the ignored `bin/Reviews/` (`M9-Review.md`, `M10-Review.md`, `M13-Review.md`, `FixDecisions.md`, and `FixReview.md` for the second round's `R01` to `R27`). Finding IDs below have the form `M9-F01`; `M10-V1` is the lead's finding.

## Decisions

Decisions are numbered across all groups, and each title names the findings it resolves. A decision states the rule a later change must keep. Decisions 94 to 114 record each change to an earlier ADR or test rule on its own; the subsystem decisions they cite hold the details.

### Renderer and shadows

#### 1. Directional casters pancake per fragment in the shader (M9-F01, M9-F06)

NVRHI never sets `depthClampEnable`, and its upstream source is not edited, so M9 pancaked by clamping clip z per vertex. That moved the plane of every caster triangle crossing the extended near plane, and large casters leaked light on the default path. When a caster pancakes (`Caster.Bias.z`), the vertex shader in `Shadow.slang` now outputs a constant mid-range clip depth (0.5 w, so neither the near nor the far plane clips the triangle) and the exact light-space depth in a `noperspective` varying; the pixel shader saturates that depth, which is hardware depth clamp's per-fragment result. It needs no Vulkan feature, keeps the one set of six caster pipelines that directional and spot maps share, and behaves the same on every device. The spot and fallback paths are unchanged.

- `VK_EXT_depth_clip_enable` is rejected for three reasons. Once it is enabled, NVRHI applies `depthClipEnable = false` to every pipeline that keeps the `RasterState` default, so every forward, prepass, selection, skybox, blit and ImGui pipeline would need auditing, and a future pipeline that forgot it would differ only on such devices. Spot maps would need separate pipelines. Devices without the extension would still need the fallback.
- The device no longer requests the `depthClamp` feature, and `DeviceCandidate::DepthClamp` is gone from device selection and scoring (the log calls the capability "shadow pancaking"). `GraphicsDeviceInfo::DepthClamp` now means "pancaking in use" and equals `!GraphicsSpecification::DisableDepthClamp`, so `DisableDepthClamp` still forces the extended-near fallback, which `BuildShadowCascades`' near extension serves. Decision 94 records the supersession of ADR 0017 §3.
- Tests: "Shadows: a caster crossing the near plane keeps its exact depth per fragment"; "Shadows: both depth-clamp and extended-near fallback render off-screen casters" also draws a caster at world z = 100, beyond the fitted box and inside the `ShadowDistance` extension, on both paths.

#### 2. The final fade covers the last tenth of the effective shadow distance; cascade-fit tests (M9-F04, M9-F05, M9-F08, M9-F09)

The last cascade's `BlendStart` is `max(SplitNear, 0.9 * min(FarClip, ShadowDistance))`, so the fade covers the last tenth of the effective shadow distance, as Architecture §8.7 and ADR 0017 §12 state, instead of the last tenth of the last cascade. The other cascades and the shader's weight formula are unchanged, so shadow visibility and the `ShadowCascades` palette stay consistent. Test: "Shadows: the final fade covers the last tenth of the effective shadow distance". The cascade-fit tests the review found weak now use an oblique light with a rotated camera, stepping along the light's right, up and back axes from a light-space grid point; a SceneRenderer case puts the blocker behind the camera, outside every frustum plane; and the orthographic and perspective fits check every cascade's sphere.

#### 3. A light the shadow planners reject stays lit without a shadow (M9-F03)

Schema-valid lights that cannot be shadowed are not render errors. A directional light that `BuildShadowCascades` rejects (for example `ShadowDistance <= NearClip`) is passed over for the next eligible directional light. `AllocateSpotShadowAtlas` validates each spot on its own (`Utils::RankShadowSpot`, which also computes its projection) and counts the ones it cannot shadow in the new `SpotShadowAtlas::Rejected`; only invalid visible indices or an invalid camera, which are caller or view errors, still fail the atlas. Rejected lights stay lit. The renderer logs one warning per renderer, without a diagnostic code; a `ProjectValidator` code may be added later.

#### 4. Per-view shadow objects belong to the caller (M9-F07, M9-F10)

`ShadowPass` creates no GPU object while recording. `ShadowRenderInputs` (a frozen `ShadowPass.h` change) gains `CascadeFramebuffers`, `AtlasFramebuffer` and `CasterConstants`. `ShadowPass::GetCasterConstantsDesc()` describes `ShadowCasterSlotCount` (12) slots of `ShadowCasterSlotStride` (256) bytes, four cascades then eight tiles; 256 is Vulkan's largest `minUniformBufferOffsetAlignment` and NVRHI's constant-buffer range unit. `ShadowPass::CreateFramebuffers(device, target)` makes one framebuffer per slice. `Record` writes the used slots with one `writeBuffer` and binds each slot through a 256-byte `BufferRange`, so the caller's `PassBindingCache` keeps its sets across frames. `SceneRenderer` creates the per-slice framebuffers when it acquires the cascade and atlas targets, drops them on `Resize`, and keeps one caster-constants buffer per view. Volatile constant buffers are rejected, because their bounded version count fails with an unbounded number of views. Test: "ShadowPass: steady-state records create no GPU objects". The file-local types of `ShadowPass.cpp` and `SpotShadowAtlas.cpp` live in unnamed namespaces (CodeStyle §4.6).

#### 5. Transparent surfaces use material AO only (M9-F11, M9-F16)

GTAO describes the prepass (Opaque and Mask) surface at a pixel. `ForwardTransparent` therefore binds a second forward view set, identical to the opaque one except that t6 is the neutral white AO texture and t8 the far-depth texture. `SampleGtao` returns 1 for far depth, so Blend surfaces get material AO only. No shader or layout changes. Test: "SceneRenderer: transparent surfaces ignore the GTAO of the opaque surface behind them". The GTAO direct-light test restores occlusion 1 before it compares direct lighting, so it can fail.

#### 6. One depth offset for depth-tested debug lines (M9-F12, M9-F19)

Every depth-tested line (wireframe, grid, bounds, colliders, script lines) uses one offset, applied in `DebugLines.slang`: each vertex moves toward the camera by `DebugLineDepthOffsetPixels` (8) pixel footprints of its distance. In perspective, clip z is divided by `1 - 8 * 2 * tan(fov / 2) / height`; in orthographic, clip z gains `8 * 2 * halfHeight / height / (far - near)` times w. A line coplanar with a surface stays solid wherever the surface makes more than about 3.6° with the view ray, which covers the half-pixel line-versus-plane depth error and the rounding between CPU-built vertices and the GPU prepass. The accepted trade-off: a hidden line less than 8 pixel footprints behind a surface shows through. The on-top pipeline tests no depth and is unaffected. Test: "DebugRenderer: depth-tested lines on a coplanar sloped surface stay continuous".

#### 7. Grid spacing, extent and fade (M9-F19, M9-F20)

The grid spacing is `10^ceil(log10(scale / 10))`, where scale is the camera's height above y = 0 (at least `NearClip`) in perspective and the orthographic half height in orthographic. The far plane never enters it, and the default editor pose gets 1 m cells. The grid covers 40 cells around the camera's ground point (perspective) or the view axis's crossing of the plane (orthographic), clipped to the frustum footprint. The fade `(1 - d / range)^2` uses the distance on the plane from that point with range 40 cells in perspective, and the distance in the view plane (depth along the view axis ignored) with range twice the view's half diagonal in orthographic. Line colours and alphas are unchanged. Test: "EditorOverlay: grid spacing follows the camera height and orthographic size, never the far plane".

#### 8. Wireframe edges are culled like the solid passes (M9-F14, M9-F13, M9-F38)

`AppendWireframeOverlay` skips every submesh for which the solid passes' `IsOutsideView` holds, frustum and `FarClip` included, so off-screen meshes no longer exhaust the `DebugDrawList` budget and meshes beyond `FarClip` draw no edges. `SceneRenderer` warns once per renderer when the edge list drops commands. ADR 0017's wireframe coverage now exists: GPU tests, among them "Wireframe: masked blended mirrored and double-sided meshes keep geometric edges and solid picking", cover mirrored and double-sided meshes, Mask and Blend edges, Blend edges behind opaque depth, occlusion, the suppression of skybox, bloom and FXAA, the budget warning, and EntityId equality including Mask holes. The Python wireframe check uses an observable `editor.screenshot`, while `viewport.pick` and `viewport.screenshot` stay unchanged by the option.

#### 9. Annotations travel apart from the view's debug list (M9-F23, M9-F24)

`RenderSnapshot` gains `DebugDrawList AnnotationDraw` (a frozen-header change). `AppendRenderAnnotations` writes labels, bounds, axes and colliders there. `SceneRenderer` draws the ordinary `DebugDraw` (script lines, gizmos) only in non-data views and `AnnotationDraw` in every view; the `Colliders`-flag shortcut is gone, and `viewport.screenshot` no longer clears `DebugDraw` for data views. The live editor's Colliders option therefore shows colliders in the AO, ShadowCascades and Overdraw views but never a session's script lines, with no host change. A CPU test checks the bounds box against the rendered pose at alpha below 1.

#### 10. Capture selection follows the target scene (M9-F22)

`AutomationMethodContext::GetSelectedEntities(const Scene& scene)` returns the selection only when it refers to that scene. `EditorMethodContext` compares the scene of `EditorContext::GetSelectionTarget()` with the argument, as the scene view's outline does, and the Runtime default in `RenderMethodServices.cpp` returns nothing. `viewport.screenshot` passes its resolved target scene, so `annotate.labels: "selection"` never labels the other target's entities.

#### 11. Debug-view pipeline failures and the `oom-specialization` fault (M9-F39, M9-F15)

`SceneRenderer::Render` returns `InvalidArgument` for an out-of-range `DebugView` before `EnsureDebugView`. A `Gpu` error from `EnsureDebugView` is `FatalError(OutOfMemory)` ("Cannot create the <view> debug view's pipelines: ..."), as M8 and Architecture §8.14 require; other errors, such as an invalid shader package, are returned. To test the fatal path, `GpuFault::OomSpecialization` (`--gpu-inject-fault=oom-specialization`) makes `GraphicsDevice::CreateShaderSpecialization` fail as if the device were out of memory. Only the material debug views specialize shaders, so startup and the other pipelines are unaffected, the same narrow targeting as `oom-texture`. The AO data view has a per-pixel oracle: "DebugViews: AO shows the GTAO pass's denoised and depth-aware upsampled output per pixel".

#### 12. Project rendering quality on every capture path (M9-F29, M9-F30, M9-F32)

Every path that extracts a snapshot copies `ProjectSettings.Rendering` into `RenderExtractionRequest::Quality`: editor thumbnails (`EditorHostThumbnails::Render`), and script-test screenshots in the Editor and the Runtime through `MakeTestScreenshotRequest(session, width, height)` (`Testing/FeatureTestRunner`). Edit-mode and play propagation into both editor views is tested. The golden capture helper and an observable check in the exported Runtime complete it at integration (decision 115): `RenderStats` and the `stats.get` view summaries report the quality a view rendered with (`shadowMapSize`, `ssaoHalfResolution`), in the Editor and the Runtime alike, and a Runtime test exports Rendering 512/true and reads it back from the game view.

#### 13. Runtime statistics are per session; statistics tests (M9-F33, M9-F31, M9-F35, M9-F02)

`RuntimeApp` keeps a per-session baseline of the frame loop's dropped time (`TimingSession`, `TimingDroppedBase`), set when the start session, a `--replay` session, or a recording or replay restart is installed; `stats.get` reports `max(0, total - baseline)`, as the editor already did. New tests pin the statistics behaviour the review found untested: "SceneRenderer: statistics list the enabled passes in the fixed pass order" (through `SceneRenderer`, so the stable sort is covered), the once-per-renderer warning above 2000 native allocations, and the once-per-view `RENDER_SPOT_SHADOW_BUDGET` warning with `DroppedSpotShadows`.

#### 14. Target formats, goldens and renderer test placement (M9-F17, M9-F40, M9-F44, M9-F10, M9-F18, M9-F27, M9-F36, M9-F37, M9-F43)

- Passes take their formats from `SceneTargetFormats.h` (`EntityIdFormat`, `ViewDepthFormat`, `AmbientOcclusionFormat`, `SelectionMaskFormat`, `LdrColorFormat`, `ShadowDepthFormat`, `SceneDepthFormat`); no pass repeats a format literal.
- The nine M9 goldens use `ENGINE_CHECK_GOLDEN` with the repository settings, so `--update-golden` writes them into `Tests/Golden/<DeviceClass>/` like every other golden; control images go to `bin/TestResults/Golden/RendererIIControls`. After integration the lead regenerated every golden with `--update-golden` on nvidia-61x and reviewed each changed image against the committed one. Six changed: `AnnotatedScreenshot` (decisions 6 and 7: the grid shows on the ground, and the axis and bounds lines are solid instead of dashed), `DebugDraw` (14 pixels of decision 6's line offset), `EditorDefaultLayout` (the grid replaces the banding at the horizon) and the three `EditorDesignSelected` sizes (the grid, the gizmo on the unfocused Scene view of decision 18, and decision 27's `X 0` and `2.5` display). The other 19 were rewritten byte for byte.
- The `RenderSceneValidation` cases live in `RenderAnnotationsTests.cpp`, the unit that owns `EvaluateRenderSceneValidation`. The `ProjectValidator` render cases stay in `RenderViewContractTests.cpp`, the integrator file ADR 0017 approves, but run under `TEST_SUITE("EditorCore")`. The two `RendererIIGolden:` support cases stay with the fixture they test.
- File-local helpers are static functions (in `Utils` for engine files), include groups are sorted, and the stale contract and scaffold comments describe the implemented code.

### Editor viewport, input and view coordinates

#### 15. One view-coordinate contract for scripts (M10-F16, M13-F18)

Scripts measure the screen in view coordinates: the pixels of the image the session's game camera renders into (`PlaySession::SetViewSize`, new `GetViewSize`), origin top-left, +y down. The view is the Runtime's framebuffer, or the editor Game panel's image (its fixed resolution, or the panel extent times the framebuffer scale, letterbox excluded).

- `PlaySession` overrides `ScriptEnvironment::WindowSize` with the view size, so `Application.GetWindowSize`, `Input` cursor positions and `Camera:ScreenToWorldRay`/`WorldToScreen` agree in every host.
- Hosts map device cursor positions with `MapToViewCoordinates` (`PlaySession.h`; pure, linear and unclamped). The Runtime scales by framebuffer over window size (identity on Windows and X11, 2× on Retina); the editor maps through the displayed game image. Injected positions (`input.inject`, `play.step`, replays, `Test.Inject*`) are already view coordinates.
- Edit-context `WindowSize` is the Game image size, else the project window size. Editor test and scratch sessions keep the project window size and are not resized by `EditorApp`, matching their screenshots and the Runtime's test sessions, so test results never depend on the panel.
- This changes ADR 0019's live-window rule for `WindowSize` only (decision 108).

#### 16. Game input is click-to-capture (M10-F14)

A press on the Game image while playing captures game input. The capture persists while the window keeps focus, the image is held or the game cursor is `Locked`; it never depends on per-frame hover. Escape, focus moving to another ImGui window, a hidden panel, the end of play and native window blur release it, and only a new click captures again. The capturing press is not game input, but capture queues the pointer's current position as the game cursor. Presses outside the image (the letterbox) are not game input unless the cursor is `Locked`; releases always pass; non-device events are not game input. While captured with a `Locked` cursor the host sets `ImGuiConfigFlags_NoMouse` (the upstream backend's recommendation for `GLFW_CURSOR_DISABLED`), so presses at the virtual cursor never reach other panels. `EditorApp::OnEvent` forwards the mapped event to `EditorPlayController::OnInputEvent` with `gameFocused` set only for game input. `EditorViewportHost` gains a non-pure virtual `IsGameInputFocused()` returning false by default, which only `EditorHostViewports` overrides.

#### 17. Losing the capture never releases input in lockstep sessions (M10-F17)

Native input is never forwarded to a lockstep session, so every held input there came from its owner, and losing the Game capture no longer queues `QueueReleaseAll` into it. Ordinary sessions without test or replay ownership still release all held input on capture loss. Keys that native input held when an agent switched a session into lockstep stay held until the owner sends `input.inject {releaseAll}`. Decision 113 records the change to ADR 0020.

#### 18. The scene image is an item; the gizmo owns the pointer over its handles (M10-F13, M10-F18, M10-F19, M10-F24, M10-F78)

- The scene image is drawn with `AddImage` under the gizmo. After `ImGuizmo::Manipulate` the panel submits an `InvisibleButton` (left, right and middle buttons, `NoNav`) over it, unless the gizmo hovers a handle or drags in this frame; then it submits an ID-less `Dummy`, because ImGuizmo activates only while no item is hovered or active. `DrawGizmoOverlay` reports this through a new `bool& ownsPointer`, computed inside the gizmo's ID scope (`IsUsingAny`, or interactive and `IsOver(op)`). The previous frame's `ImGuizmo::IsOver()` still suppresses background picks and navigation for one frame.
- Navigation runs while the image item is active (orbit Alt+LMB, pan MMB, fly RMB), so the window's move ID never blocks it. Viewport windows move only by title bar or tab (`BgClickFlags` None, set after the host's `Begin`).
- The wheel zooms the hovered view without focus and claims `ImGuiKey_MouseWheelY`. A right or middle press focuses the view and navigates in the same frame; the first left click focuses and picks. The gizmo is drawn whenever the view has a usable image and a selection, focused or not, interacts on hover, and is cancelled by focus loss only during a drag. W, E, R, X and F need the view focused and hovered.
- `GizmoController::DragState` records `PlaySession::GetSceneGeneration()` at `Begin` and `HasCurrentSource` compares it, so a `Scene.Load` of the same scene mid-drag ends in `Conflict`.
- Tests drive asset drag-drop and the input mapping through the real panel on a GPU-less host that supplies an opaque texture and the extracted camera; the real ImGuizmo drag checks exactly one "Transform selection" undo entry, equal non-zero world deltas and undo; the parent-space test snaps under a 30° rotated, non-uniformly scaled parent, so Local and World give different results.

#### 19. Panels draw into the host's window with their own flags (M10-F84)

`SceneViewportPanel` and `GameViewportPanel` draw into the caller's window and no longer call `Begin` a second time or handle closing. Each exposes a static `GetWindowFlags()` (`NoScrollbar | NoScrollWithMouse`), returned as `int` so the headers stay free of `imgui.h`, which `EditorLayer` passes to its `Begin` for those panels. Independently, the scene image claims the wheel, and the game image does while captured. A zero-area Scene rectangle (the panel not drawn, or a collapsed area) cancels an active gizmo drag in `EditorHostViewports::SetRectangle`.

#### 20. `viewport.camera` and `viewport.setOptions` cover every viewport option (M10-F20)

`viewport.camera` takes and returns, presence-aware, `projection` (the registered enum `ProjectionType`, accepted in any ASCII case), `verticalFov`, `orthographicSize`, `nearClip` and `farClip`. Each member is validated and located, then all are applied in one atomic `SetCamera`, and the result is the whole camera. `viewport.setOptions` takes and returns `debugView` (a string parsed with `ParseRenderDebugView`, as `viewport.screenshot` does) and `gameResolution` (`[w, h]`; `[0, 0]` is free; sides 1 to 8192), and validates every member before anything changes. `editor.state` is unchanged, and `viewport.screenshot` already takes `debugView`, `width` and `height` for one-off renders. Neither method is an MCP tool, so for them only `Docs/Reference/Methods.json` is regenerated. Decision 104 records the change to ADR 0018 §7.

#### 21. Minimized frames and stale screenshot frames (M10-F21, M10-F22, M10-F26)

- `EditorHostViewports::SetUnavailable` and `SetAvailable` are explicit, driven by `EditorApp`'s zero and nonzero framebuffer resize events. While unavailable, a forced offscreen frame renders and registers the images for the UI capture but never publishes extents, so `GetPixelSize` keeps answering `InvalidState` until restore.
- Texture withdrawals outside a render (minimize, retire) are counted. The new optional `ScreenshotCaptures::IsUiFrameCurrent` compares the count with the value recorded when `CaptureReadyFrame` completed. On a stale frame `FreshEditorScreenshot` re-arms (admitted = completed) and requests another frame, which keeps `CompletedUiFrame` monotonic.
- `ScreenshotCaptures::RequestUiFrame` returns `Status`; `editor.screenshot` fails at admission or at a re-arm instead of timing out.

#### 22. Hidden views and per-view render errors (M9-F34, M9-F28)

`EditorHostViewports` tracks the shown scenes (`ShownSceneTracker`: the edit and session scene pointers plus `SceneOpened` and `PlayStateChanged` events). A hidden view releases its `SceneRenderer`, after cancelling its picks, together with the binding caches that kept the previous scene's GPU textures alive, only when the shown scene changes while it is hidden and it has rendered since creation. It recreates the renderer from the device, pipelines and cache borrowed at `Initialize` when shown again. A plain tab switch keeps the renderer, and `SceneRenderer.h` is unchanged. `Render` keeps the first non-`Gpu` failure (extraction, poll or partial render) and returns it only after both views were extracted, rendered, registered, published and marked recorded; a `Gpu` error returns at once, because the host treats it as fatal.

#### 23. Framing and the camera basis (M10-F15, M10-F23)

`EditorCamera::FrameBounds` only widens the clip range (near = min(old, fitted), far = max(old, fitted)), so framing never clips what the old range showed. `EditorViewportState::SetCamera` refuses a direction with `|normalize(target - position) x +Y| < 1e-6`, computed in float exactly as `RenderExtraction`'s look-at does, so every accepted camera has the same right axis in the editor and in the renderer. The renderer's `DegenerateAxisLength` is private to `RenderExtraction.cpp`, so the constant is duplicated with a comment.

#### 24. Picking images and one scene-view icon producer (M9-F21, M9-F26, M10-F25)

`EditorViewportImage` gains `Picking`, set from the rendered snapshot's flags, and `EditorHostViewports::RequestPick` refuses a non-picking image with `Unsupported` before anything is queued. The game view renders with `RenderViewFlags::None`, so only the scene view carries `Picking`. Without an open scene the scene panel shows its "Open a scene to begin" placeholder and requests no pick, while its empty image stays registered for `IsLayoutReady`. `AppendEditorIcon` and the glyphs only it used are removed from `Editor/Icons.cpp`; the scene view's single icon producer is `RenderViewFlags::Icons` (collected in `Scene/RenderAnnotations`, drawn by `Renderer/EditorOverlay`).

### Inspector, selection and undo

#### 25. Test-run admission lives on `EditorContext` (M10-F28, M10-F75)

`EditorContext::CheckTestRunAdmission(method, dryRun)` holds the observation-only subset a test run admits, moved verbatim from `AutomationServer::AdmitRequest`, which calls it instead of a private copy. `EditorActions` checks it at `Submit` and again at `Pump`. `CommandHistory::Undo` and `Redo` call `CheckMutationPermission`, so undo and redo honour both the test-run lease and the agent-mutation policy at the lowest level. The UI controls the lease would refuse are disabled during a run (the Undo, Redo and Save menu items and shortcuts, the Undo History buttons, the playback toolbar, and the Diagnostics validate, fix and select actions); other panel actions are refused with a visible error. `EditorPlayController::ReleaseTestRun` restores the saved selection through `EditorContext::RestoreSelection`, which drops vanished ids instead of asserting and keeps the asset selection when nothing is restored. `EditorActions::Discard(ticket)` cancels an abandoned ticket and erases its entry; panels discard their tickets on a project change, and the Diagnostics panel defers refreshes during a test run.

#### 26. A selection belongs to one scene instance (M10-F27, M10-F31, M10-F32, M10-F42)

- A Play selection records the session serial and scene generation (in `SetSelection`, the legacy overload, `RestoreSelection`, the retarget, and the dry-run save and restore). `GetSelection` reads empty once `Scene.Load` or a session replacement swapped that scene, with no frame gap and independent of call order.
- `PruneSelection` clears such a selection and drops destroyed ids once per frame (`EditorContext::Update`), after UI actions (`EditorLayer::OnSafePoint`) and at the start of `EndSession`, so Stop cannot resurrect same-UUID edit entities. It never runs during a test run.
- `EditorPlayController::Start` and `ReplaceSession` re-target an Edit selection into the runtime scene by UUID (`RetargetSelectionToPlay`, the mirror of Stop: missing ids dropped, asset selection kept), and `ReplaceSession` clears a replaced runtime selection. The hierarchy highlights rows only when the selection targets the shown scene.
- `edit.select` picks the Play target when the resolved scene is the running session's scene, without calling `GetScene()` when no edit scene is open, and runs (so an empty list clears) when only a play session exists; `MakeSelection` resolves the recorded target without requiring an edit scene. The inspector skips selected ids missing from the target scene and draws the rest, with an empty state when none remain.

#### 27. Float display, drag precision and numeric clamping (M10-V1, M10-F36)

Every inspector float drag (vector axes, scalar `Float`, rotation angles and components) uses the display format `%.7g` (`X %.7g` for vector axes), which shows authored floats without float noise and round-trips typed text, with `ImGuiSliderFlags_NoRoundToFormat | ImGuiSliderFlags_AlwaysClamp`, so a drag never rounds the stored value. -0 is shown as 0 by normalizing the drawn copy only. `Int32` and `UInt32` drags clamp to `ceil(Min)` to `floor(Max)` within the type. A value landing in `FieldMeta::MinMagnitude`'s excluded band steps to the bound on its side (the previous value's side for exactly 0), so drags through zero stay valid.

#### 28. Quaternion fields edit as Euler degrees (M10-F30)

Every `Quat` field (collider rotations, the raw Transform quaternion, `WorldRotation`, script fields) edits as Euler degrees in `Transform.EulerAngles`' convention through `TransformSystem` (DetMath), at the field's `Step` or 0.5° per pixel. The angles being dragged stay in ImGui storage while the gesture is active, so the X in [-90°, 90°] decomposition never flips mid-drag. The raw x, y, z and w stay under a "Quaternion components" disclosure and are renormalized after an edit; a zero or non-finite result is `Validation`. Both edits produce unit quaternions, so field validation is unchanged. A rotation is always replaced whole, also in multi-edit.

#### 29. Multi-edit merges per component (M10-F34)

For `Vec2`, `Vec3`, `Vec4` and `Bool3` fields, and script vectors held as Variant arrays, the inspector passes the mask of differing axes to the drawer (`ReflectedDrawerContext::MixedComponents`; only those axes show "--") and the gesture's accumulated `ReflectedDrawerResult::ChangedComponents` to `ReflectedEditController::Preview(value, components)`, which merges per target, so each entity keeps its own untouched axes. Colours, rotations, scalars and containers keep whole-value replacement.

#### 30. Overlapping inspector gestures (M10-F29)

A gesture that activates while another still owns the controller is held as pending (target, latest candidate, components, release flag). It begins only after the previous edit was committed at the next safe point, or ended without change at the end of `Draw`. Only the active field's release queues a commit, and a change reported after the item stopped being active (ImGui applying deactivated text) counts as a release. `CancelEdit` cancels both gestures, `EndActiveEdit` only the active one.

#### 31. Preview validates an isolated copy (M10-F40, M10-F44)

`Preview` runs the real setters on an isolated copy of the edited entities and their ancestor chains (`SceneSerializer::EntityToJson` and `EntityFromJson`), so owner-dependent setters keep per-frame validation at a cost independent of scene size. `Commit` repeats the setter check once on a whole-scene copy before the live write. The map-key test now drives the controller: a rename onto an existing key reaches it as a whole unique-key map and replaces the stored map, and undo restores both keys.

#### 32. Open panels persist in `imgui.ini` (M10-F79, M10-F85)

The open-panel set is stored in `imgui.ini` as `[EnginePanels][Open]` with `Panels=<names>` (the launcher excluded; it follows the project state) through an ImGui settings handler, and applied with `EditorUiState::SetOpenPanels`. A loaded ini without the entry (a fresh project or a pre-change layout) opens the defaults, matching ADR 0018 checklist item 1. Panel changes mark the ini dirty, and the layer saves the ini before unregistering its handler at destruction, because Dear ImGui's final save would otherwise drop the entry. `test_editor_state_tracks_panels_selection_and_lockstep` asserts `openPanels`, and the on-disk flag test is named `test_scene_changed_on_disk_flag_holds_until_reload`.

#### 33. Shortcuts, error notices, the play accent and the asset property cache (M10-F43, M10-F39, M10-F37, M10-F38)

- Ctrl+S, Ctrl+Z and Ctrl+Y use `ImGuiInputFlags_RouteGlobal` (an active `InputText` keeps its own Ctrl+Z and Ctrl+Y through its higher-priority route), are skipped while a modal popup is open, and respect read-only projects and test runs.
- Inspector and hierarchy errors use `Utils::EditorErrorNotice` (red text and Dismiss). A notice clears on Dismiss, on the next successful action, and on a selection, asset or owner change (inspector) or a shown-scene change (hierarchy). The change checks run before action results are taken, so a failure just reported stays visible.
- Play-scene edits are tinted with the amber play accent (0.95, 0.62, 0.22) from `Editor/Ui/EditorStyle` (`EditorPlayAccentColor`, `BeginEditorPlayTint`/`EndEditorPlayTint`, `EditorPlayNotice`): the inspector tints runtime selections and the hierarchy the running scene's rows, as ADR 0018 §2 requires.
- The selected asset's parsed properties and import-settings schema are cached by asset, project generation, asset version, history position and editor revision, and re-read at most every 0.5 s of UI time; `ImporterRegistry` is built per refresh. Read and parse failures are shown inline and logged once at Warn, and `Draw` returns success, so stale-gesture cleanup always runs.

#### 34. Hierarchy reorder indexes after removal (M10-F33)

`entity.reparent` indexes siblings after removing the child. A hierarchy drop that keeps the dragged entity's parent and lands below its current row therefore passes the strip's index minus one, and a drop that would not move the entity queues nothing.

### Autosave, recovery, host lifecycle and automation policy

#### 35. Autosave document identity (M10-F02, M10-F65, M10-F04)

`EditorContext::GetSceneDocumentEpoch()` identifies the open scene document for autosave. `SetScene` and `CloseScene` advance it outside dry runs only; settings installs, edits, undo and redo do not. `MarkSceneSaved` deliberately does not advance it: explicit-save cleanup matches generations against the pre-save baseline, and a successful cleanup resets the baseline anyway. `Autosave` uses the epoch in `Publish`, `AdoptRecovery` and `DiscardSavedRecovery`. A new document's baseline is the source on disk at its first publication, unless the scene is already flagged `SceneChangedOnDisk`: a dirty scene is then a `Conflict` until reload or save, and a clean one has no baseline yet. `DiscardSavedRecovery` treats a manifest entry that cannot be read, or that belongs to another canonical project, as non-matching and keeps it instead of failing the explicit save.

#### 36. The editor's own project-file writes are admitted (M10-F01)

The captured project identity follows any `.eproj` whose bytes equal the canonical `ProjectSerializer` text of the loaded settings, however many editor writes happened since the last publication (a `project.upgrade` rewrite, an `edit.batch` rollback, an A to X to A undo). Any other change stays a `Conflict` until the project is reopened.

#### 37. Publication is cheap and happens once per safe point (M10-F03, M10-F69)

`Publish` returns its cached outcome (success or `Conflict`, never another failure) while a key of project identity, document epoch, revision, dirty flag, changed-on-disk flag, scene path, and the `.eproj` and source size plus opaque write time is unchanged; checking the key costs two stats. File hashes are reused while size and write time are unchanged, and write times stay equality-only. A generation write does a stat precheck and exactly one full byte verification before it publishes its manifest. `EditorApp` publishes once per safe point through `Autosave::Update` and before request-driven captures; the publishes in `OnUpdate` and `OnRender` are removed, because nothing between the safe point and the render commits edit-scene changes. `AutosaveSpecification::ReadObserver` lets tests count reads.

#### 38. Final writes are deduplicated (M10-F09)

`EditorApp::RefreshFatalSnapshot` only publishes; the installed fatal hook writes once. In addition, `Autosave` never writes a `FatalError` or `Exit` generation identical to the newest generation it wrote while the manifest still lists that generation first and the project and source stamps are unchanged (`Save` returns `Written = false`, `WriteFatalSnapshot` returns `Saved`). Periodic and `BeforePlay` writes are unchanged.

#### 39. Retirement is best effort and unreferenced generations are swept (M10-F08)

Removing retired generations after the manifest is committed is best effort and never fails the durable write. `Save` and `DiscardSavedRecovery` then sweep every `g-*` directory the committed manifest does not name, under the writer lease and never following links (`CheckAutosavePath`). The fatal path never sweeps.

#### 40. What "Deny agent mutations" covers (M10-F62, M10-F63)

`DenyMutations` blocks agent commands and project-file writes, plus the requests that would discard the open scene's unsaved changes and undo history when the scene is dirty: `scene.open {discardChanges}` and `session.shutdown {force}` without save. `AutomationServer::AdmitRequest` enforces this outside dry runs; human UI actions are unaffected. Method output under `Library/Automation/Out` (screenshots, test reports, offloaded results) is not project content, so `WriteOutputFile` no longer applies the policy. A refresh that must create `.meta` files remains a project write and stays refused, so `test.run` under deny works on an already indexed project. Decision 103 records the change to ADR 0018.

#### 41. Closing the window with a dirty scene asks first (M10-F64)

Only a dirty, writable, non-dry-run scene holds a window close: the `WindowCloseEvent` is marked handled, active gestures are cancelled, and the editor offers Save, Don't save and Cancel. Save runs `scene.save` through `EditorActions` and exits once the scene is clean; a failed save stays in the prompt. Don't save writes a final recovery generation (the new `AutosaveReason::Exit`, appended so persisted `Reason` values stay compatible, and deduplicated by decision 38) and then exits even if that write fails, logged at Warn, as the user chose. Cancel dismisses the prompt and drops a queued save. Untitled scenes offer only Don't save and Cancel, because the UI has no save-as. A scene that becomes clean while the prompt is open completes the close, and the prompt waits rather than displacing another popup such as the recovery offer. Read-only, clean and scene-less editors close at once; headless editors and `session.shutdown` are unchanged. The prompt lives in `Editor/Private/EditorHostClose`, drawn from `EditorApp::OnImGuiRender`.

#### 42. Recovery is refused during a test run (M10-F10)

`Autosave::Recover` and `EditorHostRecovery`'s binding check (at enqueue and at `Pump`) refuse with `InvalidState` while a test run owns the editor. `SetScene` ignores installs during a run, so accepting would otherwise report success and install nothing, or assert without an open scene. The refusal stays visible in the offer and can be retried after the run.

#### 43. A play request is validated before play is prepared (M10-F06, M10-F07)

`EditorPlayController::ValidatePlayRequest` checks the project, the time scale, that the parameters are an object, and that a scene is open or the requested `ScenePath` resolves, reads and parses. `Start`, `StartRecording` and `StartReplay` call it before `EditorContext::PrepareForPlay`, so an invalid request cancels no gesture and writes no `BeforePlay` generation. `PrepareSession` reads a requested scene file a second time after `PrepareForPlay`, because gesture cancellation can revert the open scene and the edit scene must be serialized only after it; one extra read of a small file at play start is an accepted cost. `StartReplay` validates, runs `CheckPlayScripts` and calls `PrepareForPlay` before `ReplaceSession`, like `play.start` and `input.record`.

#### 44. Host error logging and the layout file at shutdown (M10-F66, M10-F68)

`Editor/Private/EditorHostErrorLog` is one channel per error source: it logs a new error once and re-arms on success. `EditorApp` uses it for UI draw failures, UI safe-point action failures, host services and autosave, so a persistent failure never logs every frame. `EditorApp::OnShutdown`, while the project is still bound and locked, saves the project layout and switches the ImGui ini to the user-data file (or to none), so ImGui's save at context destruction never writes into a project whose lock was released.

#### 45. MCP `editor_launch` offers recovery (M10-F77, M10-F80)

`editor_launch` gains `recover?`. For an existing project, when `recover` is requested or the probe finds a published autosave (`has_published_autosave`), the bridge opens the project through `project.open {path, recover}` on a launcher-state editor, and the launch result reports `recoveryAvailable` and `recovered`. Without `recover` a candidate is only reported, never adopted. `recover` while an attachable editor already serves the project is an error naming that editor's Recover prompt, because that editor owns the project; a refused recovery shuts the spawned editor down; `EditorCrashed`'s hint points to `recover` when `autosaveAvailable`. The `template` enum is read from the catalogue's `project.create` schema, so it offers Empty and Basic3D and cannot drift. `catalog.json` does not describe bridge-local tools; Architecture §13.8 documents the tool.

#### 46. CLI fault tests assert what a startup fault can produce (M10-F11)

A startup `--gpu-inject-fault` run has no project and cannot have a dirty scene, so those tests assert only the exit code and crash report, and the device-loss case is named "EditorApp: --gpu-inject-fault=device-lost exits 4 with a crash report". The editor-autosave half of the M5 acceptance is covered by the in-file death tests (the production `MakeEditorAutosaveFatalHook` under device loss and hang) and by Python `test_device_loss_autosaves_current_dirty_scene`.

### Content browser, thumbnails, launcher, preferences and external editors

#### 47. Content-keyed thumbnail disk cache (M10-F45, M10-F58)

Thumbnails live in `<cache>/Thumbnails` as `<asset>-<size>-<digest>.png`, with a manifest `<asset>-<size>.json` that lists the inputs: the asset and everything it references through the asset dependency graph. The digest is XXH64 over the renderer format (now 2), `EngineCookVersion`, the asset, the size and, for each input, the size and modification time of its source, its `.meta` and its dependency files; built-ins contribute only their handle. File stamps are chosen over content hashes so that a cache hit needs no loading and no large reads, the signal the hot reloader also uses; an explicit Refresh rechecks entries. A request reuses the image while the digest recomputed from the current files matches the manifest. Writing a new image deletes the asset's previous one, and `BindProject` sweeps PNGs no manifest names, the manifests and images of unregistered assets, and the legacy `thumbnail-v1-*` files, so the directory holds at most one image per asset and size. In memory, keys are (asset, size) within a binding, and freshness comes from the asset version plus a snapshot of each input's (version, load state). The random `DiskEpoch` and the project generation are no longer part of file names. `test_thumbnails_generated_headless` decodes the PNG, requires rendered content and checks reuse across a restart. Decision 102 records the refinement of ADR 0018 §6.

#### 48. Thumbnail work never blocks a safe point (M10-F48, M10-F49, M10-F59)

`ThumbnailCache::Pump` handles each request at the asset's current version, so a load or reload since queueing is not a `Conflict`. A type icon or a reusable disk image needs no loading. A render needs the asset and its dependency-graph closure loaded: `Pump` starts `AssetManager::LoadAsync` for unloaded inputs and requeues the request while any input is loading, so a first render takes two safe points and no render is thrown away. Each `Pump` visits at most 64 requests (or `maxItems`). The editor's thumbnail capture calls `ViewportCapture::Capture` directly, without `CaptureView`'s `WaitIdle`, and keeps the GPU out-of-memory fatal path. `Pump` silently drops queued requests and remembered results of assets that are no longer registered. Consequence: a host must drain the main-thread queue every frame for asynchronous loads to publish. The production `FrameLoop` does; the golden `EditorDesign` harness is brought in line at integration (decision 115).

#### 49. Stale thumbnails stay visible while they regenerate (M10-F50, M10-F51)

`ThumbnailResult` gains `Stale` and `ThumbnailCache` gains `Revalidate()`. `Find` returns the last image marked `Stale` when the asset's version or any input's version or load state changed, and the panel then queues a regeneration. A remembered failure is returned only while its inputs are unchanged; otherwise `Find` returns `nullopt`, which retries. `Revalidate`, called by the panel's Refresh button, marks every entry for a disk recheck, which catches files edited outside the editor while their assets were unloaded. `EditorHostThumbnails` keys texture registrations by (project generation, asset, size) and swaps them when the cache names another PNG, without flicker; a request for an older version gets texture 0. Thumbnail failures stay on their tile (the type icon, with "Preview unavailable: <reason>" in its tooltip); the panel's error line reports only user operations.

#### 50. External editors run without captured output and are detached at shutdown (M10-F46)

`ProcessSpecification::CaptureOutput` (default true) set to false sends the child's stdout and stderr to the null device; `Wait` then returns empty text and `WaitForOutput` returns `InvalidState`. `Process::Detach()` releases the child without killing it (Windows closes the handle; POSIX stops the I/O thread and clears the pid) and is allowed only without captured output (`InvalidState` otherwise), so a detached child never writes into closed pipes. External source editors always start without captured output. `EditorSourceLauncher` (`Editor/Private/EditorHostSource`, owned by `EditorApp::State`) keeps the running editors, reaps them as they exit and detaches the survivors in its destructor, so closing the engine editor never kills the user's external editor and its output never accumulates in memory. On POSIX a detached child that exits before the engine editor stays a zombie until the editor exits; detaching happens only at shutdown, so this does not arise in practice. Decision 106 records the change to ADR 0018.

#### 51. Open source validates on the click (M10-F56)

The `OpenSource` host service reads `user://Editor.json` once on the explicit click (not per frame, and hand edits apply without a restart), resolves the path, builds argv, checks that the executable exists and returns any failure to the clicked panel: `Unsupported` when no editor is configured (naming `ExternalEditorExecutable`, `ExternalEditorArguments` and the native `Editor.json` path), the `ResolveEditorSourcePath` errors, `InvalidArgument` for arguments without `{path}`, and `NotFound` for a missing executable. Only process creation is deferred to the safe point: `EditorSourceLauncher::Pump` starts each queued launch independently and returns the first failure with the count of others, and `EditorApp` logs each failed launch once. Reaping and the thumbnail pump never depend on a launch succeeding.

#### 52. The content browser rebuilds only on change signals (M10-F47)

`ContentBrowserPanel` caches one recursive listing of the project's asset folders (with a parent-to-children index shared by the tree, the grid and the destination dialogs) and the current folder's tiles (sorted by reference path, each import diagnostic found by binary search in the sorted diagnostics). The listing is rebuilt on a project change, a finished panel ticket (Refresh included), a command-history change (current sequence, undo or redo count), a registry record-count change, or new `AssetReloaded` or `AssetImportFailed` events read through an event-log cursor (dropped events count too). A diagnostics-count change or a folder change rebuilds only the current folder's tiles. Search filters only those tiles, and `Draw` does no project-sized work. An empty folder created outside the editor appears after Refresh, as the panel header documents.

#### 53. Drops and scene opens ask when the outcome is not obvious (M10-F53, M10-F55)

Files dropped while the content panel was not drawn on the previous frame (another tab selected, the panel closed, or never drawn) go into an "Import dropped files" prompt that lists them, offers every folder as destination (defaulting to the current one) and has Import files and Discard; nothing is imported into a folder the user did not confirm. Drops while the panel is visible import directly into the shown folder. Read-only projects report rejected drops in the panel and at Warn. Opening a scene from the content browser while the open scene is dirty asks Save and open (disabled, with the reason, for read-only projects and untitled scenes), Discard and open, or Cancel, which submit `scene.open` with `save` or `discardChanges`. During a preview the action is disabled and a double-click explains why; a clean scene opens at once.

#### 54. Preferences never block startup and have one lossless writer (M10-F67, M10-F71, M10-F54)

`Editor/Private/EditorHostPreferences` loads `user://Editor.json` without ever failing startup: a malformed or newer file keeps the defaults, publishes the error as the automation preference's `Failure` (shown in the Automation panel) and is logged once at Warn, and the file is left for the user to repair. A preference listener that cannot start is turned off and its failure published; explicit CLI listening is unchanged, because the server owns it. The queued "Allow AI automation" state machine (`QueueAutomationChange`, `GetAutomationState`, `Pump(apply)`) moved from `EditorApp` into the same helper, so the production path is unit-tested. `SerializeEditorPreferences(const Json&)` is the file's one serializer (tab indent, exact doubles, and a recursive UTF-8 check that returns `Validation`); `WriteEditorPreferences` and `ProjectManager::AddRecentProject` both use it, so unknown members survive a recent-project update (`JsonWriter` rounded doubles to float and rejected 1e200).

#### 55. Launcher location, folder listing and source paths (M10-F60, M10-F52, M10-F57)

- `GetDefaultProjectLocation()` (`ProjectLauncher.h`) is `HOME`, then `USERPROFILE`, when it names an existing absolute directory, otherwise the current directory. It needs no per-OS code outside `Platform/` and never points into `Resources/`; Browse with an empty Location and the Browse projects fallback use it.
- `FolderPicker::Navigate` skips and counts entries whose metadata query fails (a dangling link, an entry removed meanwhile, no permission) instead of failing the folder, and shows "N entries could not be read and are not listed.".
- `ResolveEditorSourcePath` treats text as a URI only when it starts with a scheme of at least two characters (`[A-Za-z][A-Za-z0-9+.-]+`) followed by `:`. Every other text is a native path and keeps every colon (POSIX names such as `Games: 2026`, Windows `\\?\` paths). Unknown schemes stay `InvalidArgument`.

### Scripting VM, sandbox, determinism and bindings

#### 56. Luau compiles with the precise floating-point model (M13-F01)

The `Luau` project (VM, compiler, vector and math libraries) runs gameplay arithmetic, so it compiles with the precise model on every toolset: `floatingpoint "Default"` with `-fno-fast-math -ffp-contract=off` for GCC and Clang, which also overrides xcode4's `-ffast-math` in Dist, and the `/clang:` forms for clang-cl. `CheckBuildConfig.py` enforces it as for JoltPhysics and MikkTSpace (`PRECISE_VENDOR_PROJECTS`), and the seeded fixture `Tests/Data/BuildConfig/LuauContraction` proves the check. `LuauAnalysis` (editor-only type checking, never on the simulation path) is unchanged. `Vendor/Luau/VENDOR.md` and Architecture §2.2 record it.

#### 57. The sandbox rebinds `vector.angle` and removes `gcinfo` (M13-F02, M13-F03)

`Sandbox::Initialize` replaces `vector.angle` with `Utils::VectorAngle` before `luaL_sandbox`. It keeps upstream's contract (signed by the optional axis) but works in double with exact operations, `sqrt` and `DetMath::ATan2`. The safe-environment flag is cleared, so fast calls dispatch through the table too. `gcinfo` is removed, because Luau paces garbage collection by the wall clock; the escape suite asserts both. Architecture §4.12, §11.1 and the §15.2 escape list record them.

#### 58. Interned identities and value equality (M13-F36, M13-F23)

Entity, component-proxy and `AssetRef` userdata are interned per VM in the weak-valued registry table `Engine.Interned`, keyed by their canonical field bytes (tag plus UUID, scene generation and component index, or the asset handle). One identity is one value while referenced, so table keys, `rawequal` and `==` agree. The weak values never extend a lifetime: a script can observe a fresh userdata only after the old one was collected, and then it holds nothing to compare against. `pairs` order over object keys still follows heap addresses, so Architecture §4.12 and the `luau-gameplay` skill require sets to be keyed by `entity.ID` or kept as arrays sorted by ID wherever order matters. `__eq` is defined for `AssetRef` (by handle), `Color` (the four channels exactly) and `Quat` (the four components exactly, so q and -q are unequal), as Architecture §11.5 states.

#### 59. Host-copy limits for script strings (M13-F16, M13-F17)

`Scripting/ScriptLimits.h` bounds what the host copies or keeps outside the VM allocator:

- A `Log.*` or `print` entry keeps at most 8 KiB (`MaxScriptLogMessageBytes`). It is assembled bounded, whatever the argument count or aliasing, cut at a UTF-8 boundary and marked ` [truncated: N bytes]`. Evaluation and load-time print capture keep the bounded text even when the entry is rate-dropped.
- One VM writes at most 1024 log entries per (tick, frame) window (`MaxScriptLogEntriesPerFrame`). Excess entries are dropped and counted, with one Warn when dropping starts and one summary when the window rolls over or the VM closes.
- `Debug.DrawText` text is limited to 1 KiB, entity names (`Entity.Name`, `Scene.CreateEntity`) and tags to 256 bytes each, and `AddTag` refuses beyond 256 tags per entity (authored tags count).
- An oversized retained argument raises "argument #N is X bytes; the limit is M" (`Lua::CheckBoundedString`) before any mutation; log text is truncated instead, so diagnostics never fail. The binding descriptions state the limits.

#### 60. Enum parameters, overload selectors and enum merging (M13-F19)

`ScriptEnumParameter` gains an optional selector (another argument's index and Lua type: `SelectorIndex` and `Selector`, with `ScriptArgumentType`), so an overloaded member can declare a different enum for the same slot per overload, each with its own per-value counters; the reference reads "when argument N is a string/number". `Freeze` accepts a binding enum whose entry names and values equal those of a reflected enum of the same name as one enum, and rejects any other difference; descriptions are not compared, and the generated references keep the first registration's descriptions. `InputEventState` (Down, Up, Tap) is a registered enum with the automation method's descriptions. `Test.InjectAction`, `InjectKey`, `InjectMouse` and `InjectGamepad` validate their key, button, axis and state names with `CheckEnum`, canonicalize them before the host call and have typed signatures (`InjectGamepad` is an intersection overload). `Engine.d.luau` and `ScriptAPI.md` are regenerated.

#### 61. Source-map identities are reused for identical chunks (M13-F05)

A `LoadedMap` record stores an exact copy of its bytecode next to the source map, in the same allocator-charged block. A later execution reuses its `=engine/N` identity only when the bytes and every map field (chunk name, path, JSON pointer, source hash and size, generated prefix lines, line offsets) are equal; XXH64 only filters candidates. Each unique chunk pays its bytecode size once, and repeated predicates (`play.waitFor`, replay `Expect`) pay nothing.

#### 62. Nested entries share a pending memory fault (M13-F08, M13-F04)

While a soft-limit recovery is pending or the VM must stop, `EnterProtected` refuses only a new outermost slice. Nested entries inside the faulting slice (the instances one `Scene.Instantiate` creates, `OnDisable` from `SetActive`) inherit the latched memory fault: their failures are that one Memory fault, attributed to the faulting owner and deferred through `PendingSafetyFailure`, never Runtime errors that disable innocent instances. The acceptance cases are hosted: "Watchdog: OnCreate inside Instantiate inherits the outer deadline" spins in a scripted prefab's `OnCreate` under a controlled `ClockSeconds` (the fixture's new `OnHostRequest` hook moves the clock inside the host call), the GC-interrupt case runs a real GC-heavy loop past an expired deadline, "Allocator: second breach stops the session with a structured error" recovers a first breach and stops on the second, and "Allocator: a pending recovery lets nested entries share the one memory fault" is new.

#### 63. Forwarded errors are bound to their slice (M13-F09)

The forwarded `ScriptError` record (`ForwardedErrorRecord`) carries the serial of the outermost execution slice that raised it (`SandboxAccess::GetSlice`). `CaptureFailure` accepts a record's kind, entity, callback and traceback only within that slice. A copy that a script keeps and raises later is a Runtime error located at the raising frame and attributed to the current owner, keeping the original message text.

#### 64. Safety labels and authored locations in messages (M13-F07, M13-F06)

Timeout and memory messages name `<script stem>.<callback>`, or `<stem>.<module top>` for load-time imports, `require` and `RunModule`. Every `engine/<n>:<line>:` prefix in a script message, `print` line or `Log` entry is rewritten through `Locate` to `<authored file>[#json pointer]:<line>:` (`SandboxAccess::RewritePrivateLocations`, applied in `CaptureFailure`, print capture and `Log.*`); unknown identities are left unchanged. Private chunk identities therefore never reach users, including through `error(msg, 2)`. Decisions 129, 130 and 134 amend this: diagnostics are bounded after rewriting, test messages take the diagnostic path, and `CaptureFailure` bounds through `SandboxAccess::BoundScriptText`, which replaces `RewritePrivateLocations`.

#### 65. Error texts (M13-F67, M13-F12)

The script error for a destroyed entity is "entity <uuid> was destroyed"; an identity from a replaced scene reports "belongs to an expired scene", and a malformed identity "is not a valid entity identity". Architecture §5.7 is amended to this format, replacing "entity 'Ball' (…) was destroyed at tick 812": recording the tick would add data to the Scene module's `PendingDestroyTag`, and after the flush the entity and its name no longer exist, so the message could not be consistent across the pending and flushed states. The UUID is stable and is what `scene.query` and `script.errors` report. `Test.Case` outside a suite raises the documented "Test.Case outside Test.Suite". Member errors use `:` for methods of type groups (`Entity:AddComponent: argument #2 ...`) and `.` for functions, properties, operators and constructors.

#### 66. Positions are validated before instantiation (M13-F15, M13-F24, M13-F20)

`ScriptProxy::ValidateWorldPosition(parent, position)` is the single representability check, the computation the `Transform.WorldPosition` write already used. Script-facing hosts (`PlaySession::State::Instantiate` and `ScriptTestFixture`) call it before the prefab subtree is created, so a position the parent cannot represent is a located `Validation` error and nothing is created. `TransformSystem::SetWorldPosition` keeps its assert as a programmer-error guard that script input never reaches. `Entity:AddComponent` returns nil, without an error, when the entity is no longer valid after the instance sync (the new script's `OnCreate` destroyed it). The play-session fuzz test keeps its fixed corpus with exact expectations (7,000 calls) and adds 3,000 seed-generated calls with the value classes NaN, ±Inf, 0, -0, negative, huge, subnormal, wrong types and nil across transform and collider writes, `AddComponent` values, `Scene.Instantiate` position, rotation and parent, and `SetParent` on physics entities; after each call every transform and body must stay finite (Architecture §15.2).

#### 67. Decimal-safe tick rounding (M13-F38)

A wait of s seconds lasts `ceil(s / FixedDelta)` ticks, at least one, except that a double quotient within `min(1e-9, 8 * eps * |q|)` of an integer counts as that integer, so 0.14 s at 50 Hz is 7 ticks. The 1e-9 cap means no real fraction of a tick is ever absorbed. The helper is `ScriptSecondsToTicks` (`Scripting/ScriptTime.h`), a pure double function and therefore deterministic. `Task.Wait` and `Task.Delay` (through `TaskScheduler::Delay`) both use it, and Architecture §11.3 documents the rule.

### Script lifecycle, hot reload, importer and type checker

#### 68. Case ownership is separate from attribution (M13-F27)

A test case owns only its own thread and the tasks it spawns, which are cancelled at the case end. Lifecycle callbacks, including ones nested in case code, are instance work: `Invoke` gives them the owner {entity, no case}, and their tasks belong to the instance and survive case boundaries, as Architecture §11.3 says. `ScriptEngineAccess::GetOwner` no longer falls back to the active case, so `CheckExecution` cancels only case-owned work. The active case still receives attribution: errors (observed and claimable) and `Test.Fail` or `Skip` raised from gameplay code latch it. A callback's failure is suppressed only when that callback itself raised `Fail` or `Skip` (the scoped `TestSignalRaised`); genuine errors publish even while the case is terminal. Evaluations inherit a case only from case-owned callers.

#### 69. Contacts for unstarted instances are held (M13-F28)

Physics events that reach a created but unstarted instance, or one that already holds events, are held in `Instance::PendingPhysics` in dispatch order, neither dropped nor delivered before `OnStart`. They are delivered at the first fixed-step start flush after `OnStart` (never at the frame flush, which keeps delivery frame-rate independent and physics callbacks in the fixed step), before `OnFixedUpdate`, in (`ExecutionOrder`, canonical) order and in dispatch order within an instance. Destruction, recreation or a fault discards them. Enter and exit stay symmetric for every side.

#### 70. Evaluation failures are report-only (M13-F29)

Failures of `Evaluate` and `ExecuteBytecode` (`script.eval`, `play.waitFor`, replay `Expect`) publish with the report-only fault policy (`State::FaultPolicy`). They are recorded, logged and attributed to the active case, but they are not instance faults: they never disable the instance, cancel its tasks, drop its table, borrow its behaviour's `Script` path or request a `PauseOnError` pause. Tasks spawned by an evaluation belong to its entity and inherit a case only from case-owned callers.

#### 71. Test runs never pause on script errors (M13-F33, M13-F30)

A test-mode `ScriptEngine` never requests a pause on a script error, in any mode, because the runner owns time; Editor, Release and Dist test runs therefore observe the same audio and alpha after an error. The suite override `Overrides.PauseOnError` (Pause or Continue) has no effect on a test run's time, and its registry descriptions say so. Making Pause meaningful was rejected because the Runtime already ignored it (`IsEditor` false), so any pausing variant keeps the Editor/Release/Dist divergence. Architecture §11.7 and §11.10 are amended. The acceptance case "Errors: location, traceback and instance disabling" now also faults a resumed `Task.Spawn` thread and checks its location, the callback `Task`, the traceback, the disabled instance, the stopped `OnFixedUpdate` and the cancelled sibling task; it uses a non-test engine, so its `PauseOnError` assertion keeps its meaning.

#### 72. No callback before `OnCreate` (M13-F26, M13-F37)

`CreateInstances` records each instance's {order, entity, serial} as it creates it and sorts on that record, never on a lookup that user code may have invalidated. The new `Instance::Created` flag ensures that no callback, `OnDestroy` included, reaches an instance whose `OnCreate` never ran. An instance that user code removed or replaced during its own module load is simply not created, and no error is published, because removing the component is legitimate API use; the class table is adopted only while the instance serial still matches. "Callbacks: order and phases match §5.7" runs through `PlaySession::Tick` only and covers mid-phase creation (created at once, not visited by that phase, started at the next start flush before its first update) and `OnDestroy` before the synthesized exit sent to the surviving partner.

#### 73. Recreated instances restart their lifecycle (M13-F34)

An instance with no table (a module-top fault, or a memory fault that dropped it) is recreated on reload from defaults and overrides, with `Started` reset and held contacts cleared. It gets `OnCreate` in (`ExecutionOrder`, canonical) order, inactive instances included as at creation, before any surviving instance's `OnHotReload`, then `OnStart` at the next start flush. It does not get `OnHotReload`, since no state survived. Architecture §11.8 documents it.

#### 74. Reload gating follows the reload chain (M13-F35)

`ScriptEngine::GetReloadChain(script)` (an addition to the frozen `ScriptEngine.h`, approved in decision 109) returns exactly the dependency-ordered chain `Reload` patches: the script, plus the loaded modules and attached behaviours that require it. It shares `State::PlanReload` with `Reload`, executes nothing and publishes nothing. `EditorPlayController::OnSafePoint` holds a reload only when a script in that chain has a failed latest import, so an unloaded failed test suite no longer blocks the live game. `WaitIdle` runs only when new script publications arrived since the last safe point (`ScriptReloadsArrived`, set in `OnAssetReload`).

#### 75. Import attempts carry their located failure (M13-F40, M13-F41)

A failed `ScriptImporter` attempt adds its script failure (compile, load-time extraction or unresolved require) to the attempt's `ScriptImportCheck` as one `SCRIPT_COMPILE_ERROR` at the error's file, line and column, skipped when the checker already reported a compile error on that line; infrastructure failures add nothing. `ScriptTypeChecker` labels Luau syntax errors `SCRIPT_COMPILE_ERROR`, in required modules too. The safe point's lookup therefore publishes hot-reload compile errors at their real location instead of 1:1. `script.write` reports one finding per syntax error and load-time failures as `SCRIPT_COMPILE_ERROR` (the unregistered `SCRIPT_IMPORT_ERROR` is gone), and `project.validate`'s `CheckScriptSources` keeps the checker's code without adding a duplicate compile error.

#### 76. A missing required module is a checker finding (M13-F39)

`ScriptCheckReader` turns a module-read failure into a hard error only for infrastructure codes (`Io`, `PermissionDenied`, `InvalidState`, `Unsupported`), the same rule as `script.write`. A missing or misspelled `require` stays the checker's located finding, so `script.check`, `project.validate`, the `test.run` preflight, export and `BlockPlayOnTypeErrors` keep working. A missing requested path is still `NotFound`.

#### 77. `.luaurc` files apply by directory depth (M13-F43)

`CheckerConfigResolver::getConfig` applies the ancestor `.luaurc` files by directory depth (root, `Assets`, deeper), so the nearest directory's file wins whatever the directory names; the byte order of paths no longer decides. Architecture §11.9 documents it.

#### 78. Edit-mode evaluation skips unloadable references (M13-F45)

`EditScriptHost::Initialize` leaves an unloadable, failed or non-script reference out of the edit-evaluation schema snapshot instead of failing every evaluation. The read-only engine's instance creation publishes that entity's error on each edit evaluation (deduplicated with a count in `script.errors`); every other evaluation still runs.

#### 79. The Test template names its suite after the file (M13-F47)

`script.create`'s Test template names the suite after the file stem without `.test`, escaped for a Lua string (`Tetris.test.luau` gives `Test.Suite("Tetris", ...)`). The Behaviour and Module templates keep their `Script_` identifier prefix.

#### 80. Pinned cooked-script hash and pinned type-checker diagnostics (M13-F46, M13-F42, M13-F10)

"ScriptData: identical inputs cook identically across configurations" also checks a committed XXH64 of a fixed cooked artifact (`0x100a42b920d434f6`) in every configuration that runs the unit suite, so a Debug, Release or Dist difference in compiler output, source map or field encoding fails it; an intended compiler or format change updates the constant deliberately. `MakeScriptFixtures.py` emits per-fixture `Diagnostics` (code, file, range, message substring) for every `Errors/*` fixture and adds `Errors/OptionalArgument.luau` (an optional passed to a non-optional parameter, the Architecture §11.9 seeded mistake). The test compares pinned diagnostics exactly for every unclean fixture and fails when a `TypeChecker` `.luau` file is missing from the manifest. The `LoadTimeVm` cycle test supplies both modules and asserts the exact "require cycle: ..." message, and a runtime case drives a cycle through cooked modules.

#### 81. Lifecycle bookkeeping scales with scripted instances (M13-F32)

`LifecycleOrder` holds instance IDs only. It refreshes when the scene revision or the instance membership changes (`LifecycleDirty`), and new instances are appended at creation, so they always tear down. `SynchronizeInstances` iterates instances through it and walks the scene only when a scripted entity may need a new instance; `CreateInstances` returns early for an empty list and never copies the whole canonical order. Measured in Debug with 20,502 entities, two of them scripted: about 2.3 s per tick and frame before, about 1 ms after, and 500 spawns in one `OnStart` no longer reach the watchdog.

### Test runner, replay, recording and play state

#### 82. Every editor decodes audio deterministically; the runner verifies audio-time ownership (M13-F48, M13-F63)

`CreateEditorApp` builds every editor's `AudioEngine` with `AudioDecoding::Deterministic`, keeping the window mode's device or the one `--audio-device` chose, as ADR 0019 decision 6 decided; `test.run`, `test.list` and `--run-tests` therefore work in the ordinary windowed editor. `EditorFeatureTestHost::Preflight` keeps its guard for in-process engines. The runner cannot set `OwnsAudioTime`, because the hosts build the sessions. After applying lockstep (in `OpenSession` and `BeginReplay`) it verifies `Session->IsAudioTimeOwned()` whenever `GetAudioEngine()` is non-null and fails the suite's setup otherwise, which also catches a `ScriptedClock` suite without audio-time ownership. The Roadmap's M13 carry-over is amended to match: the hosts set `OwnsAudioTime` for every suite and replay session, and the runner verifies it.

#### 83. Replay parameters are canonical from the start of a recording (M13-F44)

Every authored file stores floats as float (Architecture §6, `JsonWriter`), and a replay header is an authored file, so no double writer is added for one member. `CanonicalizeReplayParameters` (`Asset/ReplayData`) gives the exact form a `.replay` file and a cooked replay read back (floats rounded to float, integers exact, order kept). A recording starts its session from that form, so the recorded run and every replay read identical `Scene.GetLoadParameters()`. A float beyond the float range is refused when the recording starts, before anything changes: `InvalidArgument` at `/parameters/<member>` for `input.record`, `Validation` at `/Testing/Suites/<i>/Parameters` for `test.run {record}`. `CheckTree` keeps `ReadFloat` as the format rule. Architecture §6.7 and §13.6 document it.

#### 84. A recording starts from the saved scene file (M13-F50)

Without a scene path, `input.record start` records the open scene's saved file (`ScenePath = GetScenePath()`), the asset the replay header names and every replay loads, and it refuses an unsaved scene or one with unsaved changes (`IsSceneDirty`) with `InvalidState` and the hint to run `scene.save`. Serializing the in-memory scene would depend on dirty tracking being complete; loading the file guarantees that the recording and its replays start identically, and a file that cannot load fails at record time instead of at replay time.

#### 85. Play-scene edits invalidate a recording (M13-F49)

`SceneEdit::Commit` is the single commit point of every play-scene edit (automation `entity.*` with target play, `edit.batch` through `InvokeNested`, `ReflectedEditController`, `GizmoController`, asset repairs). When such an edit changes the play scene or draws ids from it, `Commit` invalidates the session's `ReplayRecorder` with the reason "the play scene was edited (<label>)". Dry runs are excluded, because their transient undo takes the edit back; edits inside a real `edit.batch` that later rolls back still invalidate, which is conservative. `EntityMethods.cpp` has no recording logic.

#### 86. An invalidated recording ends at Stop; its owner may restart (M13-F52)

`ReplayRecorder::Finish` ends an invalidated recording with `InvalidState` and a hint to restart: it can never succeed, and keeping it blocked restart, `test.run` and `input.replay` until `play.stop`. A bad expectation (`InvalidArgument`) and the method's checks before `Finish` (path, write permission) still keep the recording, so a corrected Stop can retry. `CheckTestAdmission(client, ownerRestart)` admits `input.record start {restart: true}` from the recording's own owner (`RecordingOwner`); every other driver is still refused. The Editor and Runtime hosts of the shared method now agree.

#### 87. A replay that stops early is a failed playback (M13-F51, M13-F66)

A quit request or fatal stop that keeps the session from reaching `FinalTick` completes playback as failed: `StopReason`, `FinalTick` set to the tick reached, its `StateHash`, `HashMatched = false`, and every later expectation unevaluated with a located `Validation` failure. `input.replay` returns `Validation` with the full outcome and a `stopReason` field, and its message names the reason. In the runner, a stopped replay reports a `<stopped>` error row (pointer `/FinalTick`) and no `FinalStateHash` row; a replay that fails to load, start or play becomes a `<replay>` error row in its failed `replay:<path>` suite, and the run continues; a run tick limit becomes a timeout row and skips the remaining replays, as for suites. Record-verification failures stay outer `Validation` errors, with the stop reason. The Runtime's `--replay` logs the stop reason. A Python test kills a client during a pending `input.replay` and checks that lockstep is released, play is paused below `FinalTick` and an observer can step and inject (Architecture §13.2).

#### 88. `play.state` reports a stopped session (M13-F61)

`PlaySession::GetFatalError()` keeps the structured error on every fatal-stop path (a fatal `OnScriptError` and both session-error paths, which take the error `ReportSessionError` just published). `PlayStateResult` gains `stopped` and `stopError` (the Architecture §11.7 `ScriptErrorToJson` form, null while running). No `PlayRunState` value is added, which would change `_meta.playState` and the `PlayStateChanged` vocabulary: a stopped session stays `Paused`. `play.step` and `play.resume` refuse a stopped session with `InvalidState` naming the error instead of a later `Cancelled`.

#### 89. The script phase overrides the watchdog phase (M13-F60)

`Watchdog` keeps a separate script phase that, while set, is the reported phase; clearing it reveals the base phase (`SetScriptPhase`; `ExchangePhase` backs `WatchdogPhaseScope`, which can no longer copy a script phase into the base). Script entries can therefore nest inside any request, pending operation or frame hook without saving each other's text. The Sandbox publishes the innermost gameplay entry on every context push and pop (callbacks, resumed tasks, case bodies, nested requires) through `IScriptHost::OnScriptPhase` (default no-op), which `PlaySession` forwards to `IPlaySessionHost::OnScriptPhase` (pure). Entries whose origin is an evaluation publish nothing, so `script.eval` and `play.waitFor` keep `Automation:...` and `Play:...`. The text is `Script:<file stem>.<callback>` (`FormatScriptPhase`), with `Task` and `Test.Case` as the callback of threads. `AutomationServer` installs the editor's marker for its lifetime (`EditorPlayController::SetPhaseMarker`), and `RuntimeAutomationServer::GetWatchdog()` serves the Runtime. Updates reuse buffers and do not allocate.

#### 90. Each test session starts from the cursor baseline (M13-F53)

Each suite and replay session starts from `CursorMode::Normal`, the run baseline the editor sets in `AcquireTestRun`. The hosts reset the cursor right before creating the session (`EditorFeatureTestHost::ResetTestCursor` for acquired runs; `RuntimeApp`'s `CreateSuiteSession` and `CreateReplaySession`), which also covers case isolation and the Runtime's discovery sessions, and the editor still restores the user's cursor in `ReleaseTestRun`. `IFeatureTestHost` documents the rule.

#### 91. The runner sanitizes report text (M13-F55)

`SanitizeTestReportText` (`Testing/TestResults.h`) turns invalid UTF-8 bytes and U+FFFE/U+FFFF into U+FFFD, and C0 controls other than tab, line feed and carriage return into the visible text `\u00XX`. The runner applies it once, when it completes (every row and suite), so the RPC result, the JSON file and JUnit agree and cannot fail on script text. `TestRunResultToJUnit` and `TestRunResultToJson` stay strict validators for other producers.

#### 92. Tick limits stop stepping without relabelling finished cases (M13-F56)

The suite and run tick limits stop stepping where they are reached. A case still running times out at its wait location; a case that finished earlier in the same frame keeps its result. Selected cases left unrun produce one `<tick limit>` timeout row; when none remain, the suite passes.

#### 93. `ExpectQuit` -1 never matches a quit (M13-F54)

`QuitExpected` is `ExpectQuit >= 0 && ExpectQuit == code`, so a suite that must not quit (the default -1) fails even on `Application.Quit(-1)`.

### Amendments to earlier decisions and test rules

#### 94. Supersedes ADR 0017 §3: no Vulkan depth-clamp feature

ADR 0017 §3's statement that both the effective device capability and the enabled Vulkan feature are `hardwareSupport && !DisableDepthClamp` is superseded. Pancaking is emulated per fragment in the shadow shader on every device (decision 1); the device requests no `depthClamp` feature and device scoring ignores it, and `GraphicsDeviceInfo::DepthClamp` reports pancaking in use, `!DisableDepthClamp`. `DisableDepthClamp` stays a default-false programmatic restriction with no CLI option, so `HeadlessGpuFixture` still exercises the extended-near fallback under both API caps, and a later default device pancakes again. ADR 0017 §3's "depth-clamp pancaking when available" now reads "pancaking unless disabled".

#### 95. Refines ADR 0017 §3: rejected lights and caller-owned shadow objects

The "first eligible visible directional light" of ADR 0017 §3 excludes lights `BuildShadowCascades` rejects, and the spot atlas validates each spot separately and counts rejections in `SpotShadowAtlas::Rejected` instead of failing (decision 3). "Per-view bindings/targets belong to the caller" now also covers the per-slice framebuffers and the caster-constants buffer, which `ShadowRenderInputs` carries (decision 4).

#### 96. Amends ADR 0017 §6: annotations go to `AnnotationDraw`

`AppendRenderAnnotations` appends its capture-local commands to `RenderSnapshot::AnnotationDraw`, not to `DebugDraw`, and data views draw only `AnnotationDraw` (decision 9). ADR 0017 §12's rule that data views have no ordinary editor overlays, while explicit capture annotations may append afterwards, is kept and now also holds for the live editor view.

#### 97. Amends ADR 0017 §7 and §8: capture selection takes the target scene

ADR 0017 §7's `AutomationMethodContext::GetSelectedEntities()` becomes `GetSelectedEntities(const Scene& scene)`, which returns the selection only when it refers to that scene and nothing in the Runtime (decision 10). §8's "the screenshot host separately fills SelectedEntities from GetSelectedEntities for the target scene" is implemented through that argument.

#### 98. Amends ADR 0018 §2: selection, multi-edit and test-run admission

- "Stop restores edit selection by UUID; runtime scene replacement clears its selection" gains its mirror: Start and session replacement carry an Edit selection into the runtime scene by UUID, and a Play selection is bound to the session serial and scene generation (decision 26).
- The drawer contract "edit replaces all, never silently picks the first" is refined: vector fields merge the edited axes per target, while colours, rotations, scalars and containers keep whole-value replacement (decision 29).
- `EditorActions` preserves the test-run admission rule as well as the availability, read-only, parameter and revision checks, and undo and redo pass `CheckMutationPermission` (decision 25). Agent pause and deny still never block human commands.

#### 99. Amends ADR 0018 §3: one icon producer and picking images

The scene view's single icon producer is `RenderViewFlags::Icons`, and `Editor/Icons.cpp` emits UI vectors only. Only the scene view renders with `Picking`: the game view renders with `RenderViewFlags::None`, and `EditorViewportImage::Picking` gates `RequestPick` (decision 24).

#### 100. Amends ADR 0018 §4 and §8: fresh screenshots

`ScreenshotCaptures::RequestUiFrame` returns `Status`, so a failed fresh-frame request fails `editor.screenshot` at admission, as §8 required. A forced minimized frame never publishes viewport extents, and a completed frame whose viewport textures were withdrawn afterwards is stale and re-armed through `ScreenshotCaptures::IsUiFrameCurrent` (decision 21).

#### 101. Amends ADR 0018 §5: autosave identity, retirement and final writes

The scene half of recovery identity is keyed to `EditorContext::GetSceneDocumentEpoch()` (decision 35), the project half admits the editor's own canonical rewrites (decision 36), and publication runs once per safe point plus before request-driven captures (decision 37). Retention removes retired generations best effort and sweeps unreferenced ones (decision 39). `AutosaveReason` gains `Exit`, written by a declined close (decision 41), and `FatalError` and `Exit` writes never duplicate the newest generation (decision 38). Recovery acceptance is refused during a test run (decision 42). Decision 122 adds a second writer of `AutosaveReason::Exit`: a close in an editor that cannot draw the prompt.

#### 102. Refines ADR 0018 §6: thumbnail keys

ADR 0018 §6's "in-memory and queue keys include a nonzero host-issued project generation and canonical cache root in addition to handle/version/size/renderer-format version" is refined: memory and queue keys are (asset, size) within a binding, and the binding supplies the project generation, the canonical cache root and the renderer-format version; the generation and the root stay in the binding, not in disk names. Disk entries are content-keyed (decision 47), so they survive reopening a project. "Failed generations ... are remembered until input changes" now counts every input of the dependency-graph closure (decision 49).

#### 103. Amends ADR 0018 §6 and its third implementation review: the deny policy

"Denying mutations also guards command/file-write boundaries" and "the editor policy also guards actual commands and project file writes by command origin" gain the requests that would discard the open scene's unsaved changes (`scene.open {discardChanges}`, `session.shutdown {force}` without save), and exclude method output under `Library/Automation/Out` (decision 40).

#### 104. Amends ADR 0018 §7: viewport method contracts

The method table's `viewport.camera` row becomes "optional position, target, projection, verticalFov, orthographicSize, nearClip, farClip; the whole camera", and `viewport.setOptions` adds `debugView` and `gameResolution` (decision 20). Flags are unchanged.

#### 105. Amends ADR 0018 checklist item 1: the open-panel set persists

"Reset default layout only when no saved layout exists or explicitly requested" covers which panels are open as well as the dock geometry: the set persists in `imgui.ini` (decision 32).

#### 106. Amends ADR 0018's second implementation review: external editors are detached at exit

"The host retains the spawned process until exit" becomes: the host keeps running external editors, reaps them as they exit, and detaches the survivors at shutdown without killing them; external editors run without captured output (decision 50).

#### 107. Amends the Roadmap's M10 acceptance: the listener-preference tests

Headless editors always listen (ADR 0008 decision 14), so a headless null-platform editor cannot prove that the preference opts an editor in. `test_attach_to_windowed_editor_with_automation_allowed` stays the proof, as a windowed `--renderer none` editor whose only opt-in is the saved preference, and `test_attach_to_headless_editor_with_automation_allowed` covers the Roadmap's headless case (attachment, and preference preservation for both values). The Roadmap's M10 acceptance text is amended to name both (M10-F72).

#### 108. Amends ADR 0019 decision 10: `WindowSize` is the session's view

"Both Edit eval and Play read live environment/cursor state from the borrowed window" no longer holds for `WindowSize`: scripts see the session's view size, and cursor positions are mapped into view coordinates (decision 15). The other environment queries and cursor ownership keep reading and writing the borrowed window. Architecture §4.3, §11, §12 and §13 are updated.

#### 109. Approves `ScriptEngine::GetReloadChain` in the frozen `ScriptEngine.h`

The contract owner approves the addition of `ScriptEngine::GetReloadChain(script)` to ADR 0019's frozen header (decision 74). It shares the reload plan with `Reload`, so the editor's gate and the patch it gates can never disagree.

#### 110. Amends ADR 0019 decisions 6 and 7: recording and replay rules

ADR 0019 decision 6's recording invalidations ("known test-driver mutations or session RNG consumption") gain play-scene edits through `SceneEdit::Commit` (decision 85). "A reproducible starting scene asset is required for recording" means that `input.record start` without a path records the saved, clean open scene (decision 84), and recorded parameters are canonical from the start (decision 83). Decision 7's "verification failures retain the full outcome in structured error data" also covers a replay that quits or stops before `FinalTick` (decision 87). An invalidated recording ends at Stop, and its owner may restart (decision 86).

#### 111. The `Lua` sub-namespace

Scripting may use the sub-namespace `Engine::Lua`, and only for its checked argument and result helpers and protected VM entry points: `Lua::Check<T>`, `Lua::Push<T>`, `Lua::CheckEnum`, `Lua::CheckBoundedString`, `Lua::RaiseError`, `Lua::ProtectedCall`, `Lua::ProtectedResume` and the related helpers declared in `Scripting/LuaHelpers.h` and in Scripting's private `BindingRegistration.h` and `ScriptCall.h`. These functions mirror the Luau C API's naming domain and are called from every binding unit, so a dedicated namespace keeps them apart from `ScriptBindings` (the registered C functions) and `Utils` (file-local helpers). No other module may open `Engine::Lua`, and the namespace never exposes Luau types through public headers. ADR 0019 did not record this exception although Architecture §3, `AGENTS.md` and `LuaHelpers.h` cited it (M13-F68); they now cite this decision, which is the authority.

#### 112. `test_runaway_script_busy_watchdog` is a documented wall-clock exception

The Busy watchdog measures wall time by definition: a request is answered Busy only after the main thread has not pumped for 5 s. `test_runaway_script_busy_watchdog` (`Tests/Automation/test_script.py`) must therefore stall the main thread longer than that, which it does with a gameplay `OnFixedUpdate` that spins under a raised 10 s `CallbackBudgetMs`, itself a wall-clock budget; it asserts the Busy phase `Script:Spin.OnFixedUpdate` and `stalledMilliseconds >= 5000` (M13-F62, M13-F60). Like `test_busy_watchdog_reports_phase` (ADR 0008 decision 15) it is a wall-clock exception. It has no sleep, and every other wait is a short bounded request whose 45 s deadline only bounds a failure and is never asserted. Its pass condition depends on the configured budget (10 s) exceeding the watchdog threshold (5 s), not on machine speed. The wall-clock exception lists in `AGENTS.md` "Tests", CodeStyle §14 and ReviewChecklist §8 name it with this decision.

#### 113. Amends ADR 0020: Game input ownership

ADR 0020's "While the Game view owns input, the viewport host suspends ImGui keyboard navigation ... Losing focus restores navigation and releases game input through the existing host path" now applies to click-to-capture ownership (decision 16): a click captures; focus loss, Escape, a hidden panel, the end of play or native blur release; hover no longer matters. Keyboard navigation stays suspended while the capture lasts. The release never applies to a lockstep session (decision 17), so the blur release of ADR 0020's integration review item 3 holds for ordinary sessions only.

#### 114. Public interface changes to frozen headers

The contract owner approves these additions and changes to headers frozen by ADRs 0017 to 0019 and to the earlier milestones' frozen headers they touch (`Platform/Process.h`, `Core/Utf8.h`, `Scene/Scene.h`, `Scene/AudioSystem.h`, `Asset/AssetRegistry.h`, `Asset/AssetManager.h`, `Session/PlaySession.h`); the decision in parentheses describes each:

- Renderer and graphics: `ShadowRenderInputs::CascadeFramebuffers`, `AtlasFramebuffer` and `CasterConstants`, `ShadowPass::GetCasterConstantsDesc`, `ShadowPass::CreateFramebuffers`, `ShadowCasterSlotCount` and `ShadowCasterSlotStride` (4); `SpotShadowAtlas::Rejected` (3); `RenderSnapshot::AnnotationDraw` (9); `GpuFault::OomSpecialization` (11); the meaning of `GraphicsDeviceInfo::DepthClamp` and the removal of `DeviceCandidate::DepthClamp` (1).
- Automation: `AutomationMethodContext::GetSelectedEntities(const Scene&)` (10); the new `viewport.camera` and `viewport.setOptions` members (20); `PlayStateResult::Stopped` and `StopError` (88); the `input.replay` result's `StopReason` (87); `Watchdog::SetScriptPhase` and `ExchangePhase`, `FormatScriptPhase`, and `RuntimeAutomationServer::GetWatchdog` (89).
- Editor: `EditorContext::GetSceneDocumentEpoch` (35), `CheckTestRunAdmission` and `RestoreSelection` (25), `RetargetSelectionToPlay` and `PruneSelection` (26); `EditorUiState::SetOpenPanels` (32); `EditorActions::Discard` (25); `ReflectedEditController::Preview(value, components)`, `ReflectedDrawerContext::MixedComponents` and `ReflectedDrawerResult::ChangedComponents` (29); `EditorViewportHost::IsGameInputFocused` (16); `DrawGizmoOverlay`'s `ownsPointer` (18); `SceneViewportPanel::GetWindowFlags` and `GameViewportPanel::GetWindowFlags` (19); `EditorViewportImage::Picking` (24); `ScreenshotCaptures::RequestUiFrame` returning `Status`, and `ScreenshotCaptures::IsUiFrameCurrent` (21); `AutosaveReason::Exit` (41) and `AutosaveSpecification::ReadObserver` (37); `ThumbnailResult::Stale` and `ThumbnailCache::Revalidate` (49); `SerializeEditorPreferences` (54); `GetDefaultProjectLocation` (55).
- Session, scripting and testing: `ScriptEngine::GetReloadChain` (74, 109); `PlaySession::GetViewSize` and `MapToViewCoordinates` (15); `PlaySession::GetFatalError` (88); `IScriptHost::OnScriptPhase` and `IPlaySessionHost::OnScriptPhase` (89); `ScriptEnumParameter`'s selector and `ScriptArgumentType` (60); `ScriptProxy::ValidateWorldPosition` (66); the new headers `Scripting/ScriptLimits.h` (59) and `Scripting/ScriptTime.h` (67); `CanonicalizeReplayParameters` (83); `SanitizeTestReportText` (91); `MakeTestScreenshotRequest` (12).
- Statistics: `RenderStats::ShadowMapSize` and `SsaoHalfResolution`, and `StatsViewSummary::ShadowMapSize` and `SsaoHalfResolution` with the `stats.get` wire fields `shadowMapSize` and `ssaoHalfResolution` (12).
- Editor panels: `EditorExternalEditorState`, `EditorExternalEditorServices` and `EditorPanelContext::ExternalEditor` (51, 54), `EditorPanelContext::GetRecentProjects` (115), and `EditorLayer::RevealPanel` (53, 115).
- Platform: `ProcessSpecification::CaptureOutput` and `Process::Detach` (50).
- Added by the second review round and the CI fix (decisions 117 to 132): `GetDeterministicViewSize(const WindowSettings&)` in `PlaySession.h` and `EditorPlayController::SyncGameView` (117); `EditorHostClose::RequestClose(bool canPrompt)` (122); `AssetRegistry::GetRevision` (123); `ThumbnailCache::GetRenderQuality` (125); `MaxScriptComponentStringBytes`, `MaxScriptTestNameBytes` and `MaxScriptErrorPatternBytes` in `ScriptLimits.h`, `Lua::CheckDiagnosticText` and `SandboxAccess::BoundScriptText` (128 to 130).
- Added by the third and fourth rounds: `BoundScriptError`, `ScriptErrorStream::GetEvictedCount` and `HasEvictedAfter`, and the `script.errors` result field `evicted` (134); `MaxScriptErrorTracebackFrames`, `MaxScriptErrorEntries`, `MaxScriptTestFailuresPerCase`, `MaxScriptTestFailuresPerSuite`, `MaxScriptTestUnclaimedErrors`, `MaxScriptTestCasesPerSuite`, `MaxScriptInjectedInputsPerTick`, `MaxScriptRequireBytes`, `MaxScriptRequireEdges` and `MaxScriptTestScreenshotsPerSuite` in `ScriptLimits.h` (134 to 136, 140); `CompleteUtf8Prefix` in `Core/Utf8.h` and the removal of `SandboxAccess::RewritePrivateLocations` (134); `Scene::GetPendingDestroyCount` (137); `MaxAudioOneShotsPerFrame` in the M12 header `Scene/AudioSystem.h` (139); `MaxAssetMissingDiagnostics` and `AssetMissingSuppressedSubject` in the M6 header `Asset/AssetManager.h` (141).

### Integration and documentation

#### 115. Cross-stream follow-ups

Each stream edited only its own files and listed the edits its decisions needed elsewhere. The integration on `integrate/review-fixes` applies them:

- `AutomationServer::AdmitRequest` calls `EditorContext::CheckTestRunAdmission` instead of its own copy, and `ProjectSettingsPanel` and `StatsPanel` discard their abandoned tickets (decision 25, M10-F75).
- `EditorLayer` passes the viewport panels' `GetWindowFlags()` to their `Begin` (decision 19).
- The binding and method descriptions that say "window coordinates" (`Application.GetWindowSize`, `Input.GetMousePosition`, `input.inject`'s mouse positions) say "view coordinates" (decision 15).
- `TaskScheduler::Delay` counts ticks with `ScriptSecondsToTicks` (decision 67).
- The registry descriptions of the suite `PauseOnError` override state that it has no effect on a test run's time (decision 71).
- `ProjectValidator::CheckScriptSources` keeps the checker's codes and adds no duplicate compile error (decision 75).
- `session.info` reports a stopped session as `play.state` does (decision 88).
- The golden capture helper (`Tests/Source/Support/GoldenScene.cpp`) copies the project's rendering quality, and a test observes the quality in the exported Runtime (decision 12); the golden `EditorDesign` harness drains the main-thread queue each frame (decision 48).
- `ProjectLauncher` reads recent projects from a memory-only snapshot published at a safe point instead of reading `user://Editor.json` in `Draw` (M10-F61); a drop while the content panel is hidden gives immediate feedback (M10-F53); the editor's preferences UI holds the external editor's executable and arguments, written at a safe point like the AI-automation preference (M10-F56).
- The remaining test moves, include-order fixes, stale comments and mojibake repairs in files the streams did not own (M9-F27, M9-F43, M10-F76, M10-F81, M10-F82, M10-F83, M13-F25, M13-F58, M13-F71).
- The wall-clock exception lists (decision 112) and Architecture §2.3's PreCommit row (M13-F69).

#### 116. Documentation (M9-F42, M13-F65, M13-F69, M13-F70)

The streams amended the authoritative documents in their sections, citing this ADR:

- Architecture: §2.2 (Luau's precise model); §3 (the `Lua` sub-namespace); §4.3, §11, §12 and §13 (view coordinates); §4.12 (`vector.angle`, `gcinfo`, `pairs` order over object keys); §5.7 (the destroyed-entity text); §6.7 and §13.6 (replay parameters and recording rules); §8.1 to §8.5, §8.7, §8.8, §8.10, §8.13, §8.14 and §15.3 (the renderer as implemented, including the snapshot members, the 54 startup pipelines, t8 and t9 in the forward set and the error contract); §11.1, §11.3, §11.5 and §11.7 to §11.10 (scripting, with §11.8 using the Roadmap's hot-reload test names); §12.2 and §12.5 (the icon producer, panel persistence); §13.5 (viewport methods) and §13's screenshot text (stale fresh-screenshot frames); §13.8 (MCP recovery); §15.2 (escape suite, generated fuzz phase); §15.9 (the references step of the commit gate).
- Roadmap: M9's "Review corrections" paragraph with the added acceptance names, and M10's acceptance (decision 107).
- `AGENTS.md`: the `Lua` sub-namespace citation, the references step of the commit gate, `Resources/Scripting/Engine.d.luau` in the repository map and the outputs of `GenerateDocs.py`.
- Skills: `luau-gameplay` (`math.pow` over `^`, `vector.angle` and DetMath, no `gcinfo` or `collectgarbage`, ID-keyed sets and `pairs` order, interning, `:` error prefixes, host-copy limits) and `add-script-api` (enum selectors, `CheckBoundedString`, interning, the `:` prefix).
- Generated references: `Docs/Reference/Methods.json`, `Docs/Reference/ScriptAPI.md` and `Resources/Scripting/Engine.d.luau` are regenerated, and so is `Tools/MCP/catalog.json`, whose `input_inject` and `play_step` schemas now describe mouse positions in view coordinates (decision 15).
- `Vendor/Luau/VENDOR.md` records the precise floating-point build settings.

### Second review round

The lead reviewed the integrated fix diff with five lenses (renderer and viewport, editor, scripting, and two fix-completeness audits covering every finding) and adversarial verification. 26 findings survived (2 major, 15 minor, 9 nit; one candidate was refuted); decisions 117 to 131 record their fixes. Three streams fixed them from the integration checkpoint with regression tests that fail without the fix; the documentation findings are fixed in this record and in the Roadmap. These decisions amend the earlier ones where they say so.

#### 117. One fixed view for every deterministic session (amends decision 15; round-2 R01, R05)

A session's view is decided when it is created and keeps that kind of view for its whole life. Ordinary live play follows the host's game view: the editor's Game panel image, kept by `EditorPlayController::SyncGameView` (which `EditorApp::OnUpdate` calls every frame), and the Runtime's framebuffer. Every deterministic session keeps `GetDeterministicViewSize(project.Window)`, the project's window size in logical pixels and at least 1 x 1 (`PlaySession.h`), in both hosts and on every display scale, and no host resizes it. Deterministic sessions are a test run's suites and replays (Preflight scratch sessions and the verification of a `test.run` recording included), `input.record` and `input.replay` sessions (fixed even outside lockstep), the Runtime's `--replay` and its automation recording and replay restarts, and every session started in lockstep. Hosts still map live device positions into the session's view: the editor through the displayed Game image, scaling into a fixed view when the sizes differ; the Runtime onto its framebuffer in live play and by stretching a fixed view over the window otherwise. Runtime test screenshots use the session's view. Rationale: `ReplayHeader` stores no view, and the project's window is the same in the project and in the exported `Game.pak`, so view-dependent replays and tests reproduce across headless and windowed editors, `test.run` and the Dist export without a format change. Accepted consequences: the Game panel renders a deterministic session at the panel's aspect, which can differ from what its scripts measure; on scaled displays the Runtime extracts test and replay sessions at the logical size and renders them at the framebuffer's resolution with the same aspect.

#### 118. Grid regions never end inside the view (amends decision 7; round-2 R02)

An orthographic grid covers the whole frustum footprint around the view axis's crossing of y = 0, and its view-plane fade (range twice the view's half diagonal) never reaches zero inside the footprint. When the footprint reaches beyond 100 cells from the crossing, the fade range shrinks to the largest view-plane radius whose plane ellipse fits within 100 cells and the region is cut to that ellipse, so the line count stays bounded and the grid fades out before it ends; a plane seen exactly edge-on draws nothing. In perspective the scale is the maximum of the camera's height, `NearClip` and half the distance from the camera's ground point to the nearest point of the visible ground polygon, so a narrow or grazing view that sees only distant ground gets coarser cells that still reach it. The default poses produce the same grid as before.

#### 119. The scene image item exists only for presses (amends decision 18; round-2 R03)

The scene image's `InvisibleButton` is submitted only in a frame with a left, right or middle click, or while it is the active item. Between presses an ID-less `Dummy` stands in, and while the image is hovered and the gizmo does not own the pointer the view claims `ImGuiKey_MouseWheelY` itself. An idle hover therefore never makes the image the hovered item, and ImGuizmo's activation rule (no item hovered this frame or the previous one) accepts a press made in the first frame the pointer is over a handle.

#### 120. Pose failures are located at the pose (amends decision 20; round-2 R04)

`viewport.camera` validates and locates every non-pose member on its own, so a failure of the final whole-camera `SetCamera` is a pose failure and is located at `/target` when sent, else `/position`, else the last member sent.

#### 121. The layout's last save happens while the panel handler is registered (amends decisions 32 and 44; round-2 R07)

`EditorApp::OnShutdown` first saves the current `imgui.ini` (the open project's `Library/Editor/imgui.ini`, the read-only cache's, or the launcher's user-data file) through `ImGuiLayer::SetIniFilePath({})`, while the layer's `[EnginePanels]` handler is registered and the project lock is held; Dear ImGui then keeps no ini file, so neither the layer's destruction nor `DestroyContext` saves again. Round 1 switched to the user-data file at shutdown instead, which saved the project file after the handler was gone and lost the open panels on every restart. The layer's owner stops the ini before destroying the layer; Architecture §8 documents the shutdown save.

#### 122. A window close is held only when the prompt can be drawn (amends decisions 41 and 101; round-2 R08)

`EditorHostClose::RequestClose(bool canPrompt)` receives whether the editor has an ImGui layer. An editor that cannot draw the Save / Don't save / Cancel prompt (a windowed `--renderer none` editor) never holds a close: with unsaved changes in a writable project it logs a warning, writes the `AutosaveReason::Exit` recovery generation as Don't save does, and closes.

#### 123. `AssetRegistry::GetRevision` (amends decision 52; round-2 R09)

The registry's revision advances on Add, Update, Remove, a Rename that moves a record, and a Scan whose rebuilt records differ; a rescan that finds the same records leaves it unchanged, a copy keeps it, and any assignment (a dry-run restore, `CloseProject`'s empty registry) moves past both revisions so a cached revision never names other records. The content browser uses it as its registry change signal instead of the record count, so a file and its `.meta` renamed or moved outside the editor refresh the listing without Refresh. Decision 52's exception for an empty folder created outside the editor stands, and one extra listing rebuild after a dry-run request that restores the registry is accepted.

#### 124. One error channel per preference in the Automation panel (round-2 R10)

The Automation panel keeps the last error of the AI-automation preference and of the external-editor preference separately, logs an error only when it differs from that preference's last one, shows both, and clears only the preference that queued successfully.

#### 125. Captured thumbnails are keyed by the render quality (amends decisions 47 and 102; round-2 R11)

`ThumbnailCache::GetRenderQuality()` (the open project's `Rendering.ShadowMapSize` and `SsaoHalfResolution`) is the quality `EditorHostThumbnails` captures with, and it is part of the disk digest and of the in-memory freshness check of captured types (mesh, material, prefab, environment); decoded texture previews do not depend on it. A quality change marks captured images stale, retries remembered failures and makes reuse from disk miss. Every rendering setting that reaches the captures must join both `GetRenderQuality` and the cache key.

#### 126. Error and gesture identities use the scene document epoch (round-2 R12)

`SceneHierarchyPanel` identifies the shown edit scene by `EditorContext::GetSceneDocumentEpoch()` (play: the session serial and scene generation), so edits, undo and redo, project-settings installations and dry runs keep an action error, and replacing or closing the scene, or starting or stopping play, clears it.

#### 127. `Test.Fail` and `Test.Skip` end a behaviour task's flow (amends decision 68; round-2 R14)

A `Test.Fail` or `Test.Skip` raised on a yieldable task the ended case does not own (gameplay work a behaviour spawned) cancels that task inside `EndTestCase`, before the binding yields, so no statement after the call runs, even inside `pcall`, and even when the case was already terminal. A lifecycle callback raising the signal unwinds as before, and the active case is still only attributed, never an owner.

#### 128. Component text and the other retained script strings are bounded (amends decision 59; round-2 R15, R21)

`ScriptLimits.h` adds `MaxScriptComponentStringBytes` (4096): all text in one component field value a script writes, through a proxy assignment or one `Entity:AddComponent` value (strings, map keys, and Variant JSON strings and keys together), counted by the native conversion before any copy, with a located error naming the field before any host mutation. Generated declarations state the limit. The audit of every script string the host keeps across calls also added `MaxScriptTestNameBytes` (256) for `Test.Suite` and `Test.Case` names and `MaxScriptErrorPatternBytes` (1024) for `Test.ExpectScriptError` patterns.

#### 129. The 8 KiB diagnostic cap holds after location rewriting (amends decision 64; round-2 R16)

`Log.*`, `print` (including evaluation and load-time capture) and test report messages go through `SandboxAccess::BoundScriptText`: at most 8 KiB of the script's text is assembled, private `engine/<n>:<line>:` locations are rewritten with a bounded rewrite, and only then is longer text cut at a UTF-8 boundary and ended with ` [truncated: <n> bytes]`, where n counts the script's own text before rewriting. The marker counts toward the 8192 bytes.

#### 130. Test report messages use the diagnostic text path (amends decision 64; round-2 R22)

`Test.Fail`, `Test.Skip` and `Test.Expect*` read their messages with `Lua::CheckDiagnosticText`, which copies only a prefix of the VM string, rewrites private chunk locations and bounds the result like a log entry, so `test.run`, JSON and JUnit rows never show `engine/N:` and message length never fails the call.

#### 131. A faulted creation batch defers the rest (amends decision 62; round-2 R23)

When a nested entry of the current slice has latched a memory or timeout fault, `CreateInstances` stops: entities not yet made are created at the next `SynchronizeInstances`, and made instances whose `OnCreate` has not run are marked deferred; the next `SynchronizeInstances` after recovery runs the deferred `OnCreate`s, after that synchronization's enable and disable transitions, in (`ExecutionOrder`, canonical) order, keeping each instance's table identity, and runs each only once; that is usually the next phase's start, but it can be a synchronization a binding triggers inside another callback (`Entity:SetActive`, `AddComponent("Script")`, `SetParent`). Outermost batches are unchanged; this replaces decision 62's "share that one memory fault" wording for creation batches.

### CI fix

#### 132. Tests that hold a test run use a stepped server clock (lead, found by the Release CI stage)

`test.run` advances its runner for `TestRunFrameBudget` (50 ms of the server's wall clock) per Pump, so a test that needs a run still active after one Pump must not depend on how many runner steps the build completes in that time. Such tests inject `AutomationServerSpecification::WallClock` advancing 100 ms per reading (or `UtilityPanelClock::Stepped` in the panel fixture), so every Pump advances the run by exactly one step in every configuration; two round-1 tests that passed in Debug and finished the run within one Pump in Release were converted.

### Third round: identities and retained script data

Two out-of-scope observations from the second round's streams were fixed (decisions 133 and 134), and an audit of every place the host retains script-provided data added bounds (decisions 135 to 138); the fourth round closed the three gaps the audit left open (decisions 139 to 141).

#### 133. Inspector gestures survive settings changes (extends decision 126)

The inspector's owner identity uses `EditorContext::GetSceneDocumentEpoch()` for the edit scene (the project binding and the play session's serial and scene generation are unchanged), and `ReflectedEditController::Commit` checks scene, asset and play targets against the scene document epoch plus the open scene's own revision; only a project-settings target conflicts when settings are installed. The commit still validates the edited values against the current scene (a serialized copy and the script schemas); project settings never constrain component values on these paths, so committing after a settings change is the same as committing before it. A pending inspector edit or name edit therefore survives a project-settings change and its undo, and a scene replacement still discards it.

#### 134. The script error stream is bounded (extends decision 59, amends decision 64)

`ScriptErrorStream::Add` stores `BoundScriptError(error)`: the message is cut at `MaxScriptLogMessageBytes` (8 KiB) on a UTF-8 boundary with the ` [truncated: <n> bytes]` marker, and the traceback keeps the innermost `MaxScriptErrorTracebackFrames - 1` (31) frames followed by one marker frame `[truncated: <n> more frames]`. A stream keeps at most `MaxScriptErrorEntries` (256) distinct errors and evicts the one whose latest occurrence is oldest; an evicted error that recurs starts a new count. The cursor never decreases. `CaptureFailure` reads a raised string in place on the Lua stack and bounds it through `SandboxAccess::BoundScriptText`, so the host never copies a huge error string, and hosts publish script-error events from the bounded copy. `script.errors` gains the boolean `evicted`, true when an error newer than `since` was evicted; a boolean rather than a count, because an exact count would need unbounded eviction history. `CompleteUtf8Prefix` moves to `Core/Utf8`, and the unused `SandboxAccess::RewritePrivateLocations` is removed.

#### 135. Test runner retention limits

A test row keeps at most `MaxScriptTestFailuresPerCase` (32) script-driven failures, the last of them `[truncated: <n> more failures]`, and a suite at most `MaxScriptTestFailuresPerSuite` (1024); the runner's own failures (timeouts and the like) are always kept. Claimed script errors are released at once, and a case keeps at most `MaxScriptTestUnclaimedErrors` (256) unclaimed ones; overflow counts as an unexpected error and fails the case. A suite registers at most `MaxScriptTestCasesPerSuite` (4096) cases (a located error at registration), and `Test.Inject*` adds at most `MaxScriptInjectedInputsPerTick` (1024) events per input tick.

#### 136. `require` requests and edges are bounded

A `require` request longer than `MaxScriptRequireBytes` (1 KiB) is refused before it is copied or recorded, and each resolver records at most `MaxScriptRequireEdges` (16,384) distinct edges (a Validation error beyond).

#### 137. Entities pending destruction count toward the entity limit

Entities destroyed in a tick stay in the registry until the destroy flush. `PlaySession::CheckEntityCapacity` therefore also requires live + new + pending-destroy entities to stay within twice `MaxEntities`, using the new `Scene::GetPendingDestroyCount()`, so a script that creates and destroys entities in a loop cannot grow the registry past that bound within one tick. Architecture §5.7 records it.

#### 138. Retained data the audit leaves as it is

Batch results keep every operation's result: batch files are authored and each is bounded by its own size. Load-time `ScriptData` names and fields are limited by the native conversion budget. Log entries, the event log, debug draw lists and text, entity names and tags, component text, test names, messages and patterns, `Scene.Load` parameters, coverage counters, physics events and the single retained errors were verified bounded already.

### Fourth round: the audit's remaining bounds

#### 139. Audio one-shots are bounded per frame

A session's `AudioSystem` admits at most `MaxAudioOneShotsPerFrame` (1024) `PlayOneShot` requests between two `Update` calls, which covers one frame's fixed steps, `OnUpdate` and `OnLateUpdate`; evaluations while no frame runs count toward the next frame. Arguments are validated first, and every admitted request counts whatever its outcome, because each costs a voice start. A later valid request starts nothing and fails with `InvalidState` naming the limit, which scripts receive as a located error. Handles of ended or stolen one-shot voices are pruned after every voice start and at every `Update`, so the system tracks at most `MaxAudioVoices` (64). The constant lives in `Scene/AudioSystem.h` so the bound holds for every caller; `ScriptLimits.h` points to it.

#### 140. Test screenshots are bounded per suite

The runner admits at most `MaxScriptTestScreenshotsPerSuite` (256) `Test.Screenshot` calls per suite, counted before the host is called and reset at each suite start. A later call reaches no host, writes no file and raises a located error naming the limit; unhandled, it fails its case like any script error, and the run goes on and publishes its report.

#### 141. Missing-asset diagnostics are bounded

An `AssetRef` may name any unregistered handle (Architecture §7.1; `Assets.IsValid` exists for that), so the bound is in the asset manager rather than at the script boundary. A manager keeps `ASSET_MISSING` diagnostics for at most `MaxAssetMissingDiagnostics` (1024) distinct handles. Past that, a use of another unknown handle still gets the placeholder but is neither recorded nor logged; one summary `ASSET_MISSING` diagnostic (no asset, no path, subject `suppressed`, logged once) counts those uses. It counts uses rather than handles, because an exact distinct count would need unbounded memory (as in decision 134). A recorded handle that loads frees its slot, a registry scan removes the summary with the reference type mismatches, and dry runs restore the count. Like the other reference diagnostics, the summary never blocks play or export. Because a manager can now hold over a thousand diagnostics, reports compare keys without copying and insert new diagnostics in sorted position instead of rebuilding the merged list.

## Review and verification

- **Method.** The lead reviewed each milestone at `81fe89e` with independent lenses (five for M9, six for M10 and M13), each reading its scope against the Architecture, the Roadmap, the milestone's ADRs and `ReviewChecklist.md`. An adversarial verifier then tried to refute every candidate by tracing the code and the vendored sources, with numerical replays where a claim depended on arithmetic. A finding survived as CONFIRMED or PLAUSIBLE with the verifier's reasoning, and its severity was set after verification. Refuted candidates are recorded with the reason and are neither counted nor fixed: M9 F25, F41 and F45; M10 F05, F35, F41, F70 and F74; M13 F11, F22, F31, F57 and F64. Duplicates were merged (M10 F73 into F56, F65 into F02 and F69 into F03; M13 F59 into F48). Strict PreCommit was green on the reviewed head.
- **Counts.** 185 verified findings: M9 42 (7 major, 25 minor, 10 nit), M10 78 (2 blocker, 23 major, 39 minor, 14 nit) and M13 65 (3 blocker, 19 major, 31 minor, 12 nit). Every finding is fixed. Findings a stream could only finish partly in its own files are completed by decision 115.
- **Regression tests.** Each behaviour fix carries a regression test that fails without it; the streams verified this by reverting each fix and running its test. Test-quality findings are fixed by strengthening or replacing the test so that it can fail; the decisions above name them, except M13-F21, which adds a real `PlaySession` test with configured `Input.Actions` that reads action states from scripts in the fixed and frame phases and asserts the located `INPUT_UNKNOWN_ACTION` error with its did-you-mean hint. Style and documentation findings are fixed by the edit itself; those not named above are M10-F12 (include order in the autosave and recovery files), M13-F13 (dead `Sandbox.cpp` helpers removed) and M13-F14 (file-local helpers as static functions in `Utils`). No test was skipped to close a finding; vacuous assertions were replaced by observable checks, or removed with the real coverage named (decision 46), and moved cases kept their bodies.
- **Later rounds.** The first full CI run on the integrated fixes failed only its Release unit stage: two round-1 tests depended on how many runner steps one Pump completes (decision 132). The second review round (decisions 117 to 132) found 26 problems in the fix diff (2 major, 15 minor, 9 nit): regressions the first round introduced (R01 and R05 the script view of recordings and replays, R02 the orthographic grid edge, R03 the first-frame gizmo press, R04 the pose error location, R08 the close without a prompt, R09 the content browser after external moves, R14 `Test.Fail` in behaviour tasks), incomplete fixes (among them R07, the open panels still lost at shutdown, which M10-F79 had reported) and documentation that did not match the diff. Three streams fixed them with regression tests shown to fail without the fix; style findings R06, R13, R18 and R27 were fixed by moving a test, correcting a comment, making helpers static and giving `Bindings/Script.cpp` its mirrored test file. The third and fourth rounds (decisions 133 to 141) fixed the two observations the second round's streams reported outside their scope and every unbounded retention of script-provided data an audit found, each with a test shown to fail with the bound disabled.

Gate results: on the final tree, strict `PreCommit.py` passed (unit 3243 in Debug with only the 31 child-process targets skipped, GPU 251 under Vulkan 1.4 and 251 under 1.3, golden 25 of 25 on nvidia-61x, automation 240 with method coverage 86 of 86, and lint, lint self-test, format, build configuration, fixtures and references clean), and full `CI.py` passed (zero-warning MSVC Debug, Release and Dist builds; unit and GPU suites in Debug and Release; Release goldens and automation; portability generation for vs2026, gmake, ninja and xcode4; clang-cl Release and Dist builds). Linux and macOS verification remains with remote CI under ADR 0011.

## Consequences

- Shadows pancake per fragment on every device. A new caster pipeline must not rely on hardware depth clamp or on `VK_EXT_depth_clip_enable`, and every other pipeline keeps NVRHI's default depth clip.
- Data views draw only capture annotations. New annotation kinds go to `AnnotationDraw`, ordinary view decorations to `DebugDraw`. Depth-tested debug lines carry the 8-footprint offset, so a new line consumer gets it automatically, together with its show-through band.
- Scripts and injected input use view coordinates. A new host maps device positions with `MapToViewCoordinates`, and game input in the editor starts with a capturing click.
- A selection is bound to one scene instance; a Play selection held across `Scene.Load` reads as empty, and UI actions refused by a test run are disabled or report a visible error.
- Autosave is keyed by the document epoch and publishes once per safe point. A path that replaces the scene document goes through `SetScene` or `CloseScene`, and new `AutosaveReason` values are appended. A new method that discards the open scene's unsaved work belongs in the deny rule of decision 40.
- Thumbnails are content-keyed and never block a safe point. A new kind of thumbnail input must contribute to the digest and to the input snapshot. An external edit that keeps both the size and the modification time of every input is not detected; this is the accepted cost of stamp-based keys.
- External editors are detached at shutdown, and `Process::Detach` requires `CaptureOutput = false`.
- Script strings the host retains are bounded by `ScriptLimits.h`; a new binding that stores script text checks it with `Lua::CheckBoundedString` before any mutation. Entity, proxy and `AssetRef` values are interned, so scripts may use them as table keys, but iteration order over object keys stays nondeterministic and gameplay guidance keeps requiring ID-keyed sets.
- Evaluations never fault instances, test runs never pause on script errors, and contacts for unstarted instances are held. New one-shot script paths use the report-only fault policy, and new script entry points publish their phase through `IScriptHost::OnScriptPhase`.
- Recordings start from the saved scene, are invalidated by any play-scene edit through `SceneEdit::Commit`, and end when invalidated; a replay that stops early is a failed playback with its outcome. New play-scene mutation paths must commit through `SceneEdit`.
- Deterministic sessions (test runs, recordings, replays, lockstep) see the project's window size in both hosts and on every display scale; only ordinary live play follows the game view. A new deterministic session kind must use `GetDeterministicViewSize`.
- Every host-retained datum a script can grow is bounded by a documented constant (`ScriptLimits.h`, `MaxAudioOneShotsPerFrame`, `MaxAssetMissingDiagnostics`); a new binding or host path that retains script data must add its bound and a test.
- The wall-clock exceptions are now four: the windowed "FrameLoop" child, `test_busy_watchdog_reports_phase`, `test_headless_play_without_lockstep_is_throttled` and `test_runaway_script_busy_watchdog`.
- Accepted costs: one extra read of a requested scene file at play start; hidden lines within 8 pixel footprints behind a surface show through; inline audio decoding in every editor, as ADR 0019 decision 6 accepted.
- Linux and macOS behaviour of these fixes is verified by remote CI, which stays non-blocking under ADR 0011.
