# 0018 — M10 editor UI contract

- **Status:** public contract frozen in `dd5d57d`, with approved implementation amendments below. Milestone acceptance is recorded by the reviewed implementation commit.
- **Date:** 2026-10-09.
- **Base:** `b7b972d` (M8 renderer, M11 physics, M12 audio integrated).
- **Authority:** Architecture §4.13, §5.6, §12, §13.4–§13.8; Roadmap M10. Local contract checks only here; the parent owns the scheduled gate, review, contract commit and final integration. Remote CI remains non-blocking (ADR 0011).

## Decisions

### 1. CPU state and the UI

`EditorContext` owns `EditorViewportState` and `EditorUiState`. Camera state is still the existing `ExplicitRenderCamera`, exposed by the existing `GetSceneViewCamera`; it is not duplicated. `EditorCamera`, `GizmoController`, `ReflectedEditController`, `EditorActions`, preferences, thumbnails and autosave live in **EditorCore**. Tests links EditorCore and Engine, not the Editor executable, so it can test these classes without UI symbols. This refines Roadmap M10's file-location shorthand. ImGui drawers, icons, panels and the ImGuizmo adapter live in Editor.

`EditorLayer` is one UI owner, not a generic layer stack. Its safe-point work and its ImGui traversal are separate. Every §12.2 panel has a public header and marked implementation stub. Persistent data comes from EditorContext; panel-private state is only presentation (search, scroll, open widgets). Services in `EditorPanelContext` are borrowed; they outlive the layer and are never retained by an individual draw call.

### 2. Actions, continuous edits, selection and origins

`EditorActions` queues typed-method requests from UI traversal and runs the existing reflected handlers at the safe point with **User** origin and `ui` provenance. It is not an in-process agent client: the existing `AutomationServer::EnterInvocation` marks those Agent. Its implementation must preserve host availability/read-only/parameter/revision checks, own a request context for pending operations, and invoke cancellation. It must not accidentally bypass these checks just because it bypasses TCP. Agent pause/deny controls do not block human UI commands.

The gizmo captures a preview scene copy at activation, edits only that copy while dragging, and commits the final validated poses in one short `SceneEdit` at release. The scene viewport extracts the preview, not the live scene. ESC/focus loss drops it with no history entry. No live scene tracker spans frames. A revision, session or scene change causes Conflict before a write, preserving the intervening agent action. The Roadmap acceptance name about “one merged undo step” remains verbatim: all gesture changes are consolidated into that one release command. This follows §12.3's activation/release semantics rather than committing every mouse move. In Play/Simulate, the final write is to the runtime scene only and returns undo index 0; Stop discards it (§5.6).

Reflected drawers likewise edit an owned `Value` until release. Maps validate unique keys; variants use the real owner/type/key and resolver. Unresolved variants remain raw, read-only and diagnosed. Multi-edit validates every target before a command. Asset and settings edits still write through existing commands (ADR 0010 decision 18); no new dirty-asset store is introduced.

Selection records `SceneTarget`. Existing `edit.select` resolves the shown play scene but the old one-argument `EditorContext::SetSelection` filters against the edit scene; integration must fix that mismatch. The new explicit-target overload validates all UUIDs first. Selection summaries, hierarchy and inspector must use that target. Stop restores edit selection by UUID; runtime scene replacement clears its selection. A UI play-scene edit is visibly tinted and transient. Do not implement edit-scene gizmos while a session runs.

### 3. M9 boundary, dimensions and picking

M10 introduces no renderer picking-result type or competing render flag enum. `EditorViewportOptions` is a CPU preference object for `viewport.setOptions`; `EditorViewportHost::ConfigureSceneSnapshot` maps it to M9's `RenderViewFlags` on the copied snapshot. The scene view sets `EditorOverlays`, maps Grid/Colliders/Icons/Wireframe, adds Selection for selected entities and Picking when needed; Gizmos enables ImGuizmo interaction. The game view does not inherit editor overlays. M9 owns `RenderAnnotations`, `RenderQualitySettings`, `PickTable`, mesh `PickId`, `AsyncPicker`, `RenderStats`, `viewport.pick`, `scene.raycast` and `stats.get`.

Application calls OnRender before ImGui BeginFrame/OnImGuiRender. Rectangles measured in UI frame N apply to rendering N+1. Each displayed image reports its **actual** pixel dimensions and generation. Clicks use `ToViewportPixel`: top-left origin, floor, right/bottom exclusive, letterbox bars excluded. Hidden, zero-size, non-finite and minimized rectangles do not render or receive input. The camera retains the last nonzero rendered aspect (default 640×360, as amended by the parent's actual scene/game extent contract). Game resolution is free (both dimensions zero) or fixed and letterboxed. Fixed resolution never changes click mapping to use the enclosing panel instead of the image.

`EditorViewportState::GetPixelSize(view)` supplies M9's method-context adapter with independent actual scene/game image extents, initially 640x360 for headless use. `SetPixelSize` publishes a completed image's extent or both zero when unavailable; hidden/minimized views return InvalidState. `GetSceneSize` retains the last usable scene aspect separately, initially 640x360, and is never used to make a hidden image appear available. Game resolution preferences change the reported extent only after rendering. Screenshot captures do not publish viewport extents.

The injected host adapts M9's `AsyncPicker::Request(texture, pickTable, viewGeneration, PickRequest{X,Y,FrameIndex,SceneRevision,Sequence})` and `Poll(ticket,currentFrameIndex,viewGeneration)`. It owns the ticket/result, waits the documented two-frame delay without blocking UI, and discards results with stale sequence/revision/generation before resolving the UUID. Empty hits clear selection only for the current click. A missing M9 adapter returns Unsupported; it is never counted as working picking.

`Editor/Icons.cpp` emits first-party UI vectors and scene-view `DebugDrawList` primitives. Engine screenshot annotation code cannot call it; M9 owns those Engine-side glyph primitives. The integrator must select one producer per displayed icon to avoid duplication. Colliders reuse the existing `AppendColliderDebugDraw`. Scene/game/thumbnail textures use the existing `ImGuiRenderer::AddTexture/RemoveTexture` lifecycle, including delayed retirement.

### 4. Fresh editor screenshots

The existing synchronous `editor.screenshot` remains callable during contract staging. Its new `BeginEditorScreenshot` entry point is an explicit Unsupported stub. Implementation switches its registry entry to `AddPending`, preserving wire params/results/flags, and injects `CompletedUiFrame` plus `RequestUiFrame` alongside the existing `EditorUi` capture.

A request remembers the completed serial at admission and only captures a later completed UI frame. A request after a mutation in the same Pump therefore includes that mutation. A minimized host services the request with a complete offscreen/null-platform UI frame without swapchain acquisition. It must not reuse stale draw data or wait for a user restore. Cancellation releases the request. The actual viewport images must be rendered for the geometry represented by that UI frame; a resize may require a second frame. Integrator owns this scheduling seam in EditorApp/Application and preserves GPU-error handling.

### 5. Autosave, fatal errors and recovery

Autosave is pure CPU and has one owner whose EditorContext outlives it. `Update(nowSeconds)` receives monotonic unscaled time from the host's injected frame clock. It attempts a dirty-scene save at two-minute intervals, no catch-up bursts. Read-only, dry-run, no-project and clean states are quiet no-ops; an explicit save in read-only mode is PermissionDenied. Before Play/Simulate, commit or cancel the active UI gesture, then save the current committed edit scene. Play-scene mutations are never autosaved over the edit scene.

Publish at main-thread safe points after UI/automation edits and before GPU work. Publication owns serialized bytes and resolved native destination paths. The fatal hook may run on a worker: it must never read ECS/editor/VFS state, touch GPU, wait on a lock the failing thread might have interrupted, or allocate a fresh scene copy. Use bounded publication slots with atomic ownership transitions, retaining a usable published slot while a new one is prepared. A fatal claimant prevents its slot's reclamation. Never rely on `atomic<shared_ptr>` being lock-free. Project close calls the nonblocking `Autosave::Reset` handshake: it atomically closes writer admission and returns false while an earlier claim remains active. Until it returns true, the host keeps the original LoadedProject, writer lock, service and claimed slots alive and defers unmount/replacement to later safe points; no spin or premature unlock is allowed. A fatal claimant never waits for Reset. Hook shutdown/lifetime synchronization also precedes destruction. A main-thread GPU failure at a known safe boundary refreshes current committed CPU state before writing. Other failing threads use the last complete published revision; edits in flight may be absent. A failure interrupted during publication must return promptly using an older complete slot or the existing durable generation, never deadlock. Fatal I/O remains best-effort under host OOM or disk failure; failure must not recurse into FatalError or the ordinary logger.

The disk format is `Library/Autosave/<generation>/...` plus a final atomic manifest pointer. Generation IDs are opaque, validated identifiers. Write payloads first, hashes and metadata second, publish the manifest last. Ignore incomplete generations. Retain the configured bounded number of complete generations. Filesystem atomic replacement is the existing FileSystem contract, not a new promise of power-loss durability/fsync.

There is **no project UUID or scene UUID** in the current formats. Recovery identity is the canonical `.eproj` path and captured project fingerprint, canonical source scene path and captured source hash/size/write-time (or a generated in-session token for an untitled document), plus schema/version and payload hashes. A moved project, a changed/replaced/recreated source or `.eproj`, a traversal/alternate scheme/reserved name, or a symlink/reparse escape is never silently adopted. Inspect canonical containment against the locked project before reading/writing; recheck source fingerprints at Recover. Same path alone is not identity. Freshness follows the parent-approved equality-only fingerprint and dirty-revision rule: changed fingerprints conflict, clean/byte-identical payloads are not offered, and persisted manifest sequences order complete generations across restarts. No timestamp ordering is used. File recreation with exactly the same bytes and metadata is indistinguishable using these formats and is treated as the same source; do not claim a stronger identity guarantee.

`FileInfo::ModificationTime` is opaque and supports equality only. Freshness is established by a manifest recording a dirty revision derived from the captured source fingerprint, with that fingerprint still matching on inspection and adoption. Never order opaque timestamps or compare them against wall-clock epochs. A changed fingerprint makes recovery stale/conflicting; a clean or byte-identical payload is not offered. Generation ordering uses an explicitly persisted sequence, not directory iteration or opaque modification times. An untitled document uses its captured in-session token.

Recovery validates the complete generation and deserializes into temporary CPU objects before replacing the live scene once, dirty and with empty history. It **does not overwrite source asset files**; this makes adoption atomic without a multi-file filesystem transaction. Explicit Save later uses the normal provenance path. Native assets and settings are already written through and have nothing to recover. `project.open {recover:true}` publishes the opened editor state only after this preparation; error releases the new lock and leaves the launcher. Without a newer valid generation it opens normally, `recovered=false`. `recover:false` opens normally and reports `recoveryAvailable` without adoption; it is not an inspection-only call. After that open, EditorLayer keeps the recovery modal reachable through the explicitly injected `EditorPanelContext::Recovery` services, even though the launcher has closed. Its memory-only GetOffer returns an owned offer with a host-issued ID bound to the project open epoch and edit revision. QueueDecision copies Accept/Decline for the next safe point. Accept revalidates both bindings and calls Autosave::Recover on the same open project/lock; Decline only dismisses that offer for that open epoch, preserving sources, history and durable recovery. Intervening edits/project replacement cause Conflict or cancellation. UI never issues a second project.open; automation can choose recover=true on its initial open. Save discards only matching recovery; one saved scene must not delete another scene's recovery.

### 6. Content, templates and preferences

Thumbnail in-memory and queue keys include a nonzero host-issued project generation and canonical cache root in addition to handle/version/size/renderer-format version. ThumbnailCache starts unbound; BindProject binds the open project after acquiring its lock, with a never-reused generation even when reopening the same path. Before unmount/private-cache removal, Reset cancels queued work, drops images/errors and withdraws the binding; the host retires old thumbnail ImGui keys. Request/Queue/Find reject a different request generation with Conflict and an unbound service with InvalidState. Pump must not render or write an old project's request against new mounts. UI queues work; safe-point Pump does bounded work; Draw performs a memory-only lookup. Tests inject CPU images. Production uses offscreen rendering for visual previews, decoded texture data where suitable, procedural type icons otherwise. No dummy PNG is counted as a rendered thumbnail. Read-only projects use their private cache. Failed generations do not overwrite valid cached PNGs and are remembered until input changes. Headless thumbnails use the same path.

Basic3D adds a generated start scene with camera **and AudioListener**, directional sun, built-in Studio environment, post-process and ground/static box collider. It should validate with zero errors and no missing-listener warning, not merely suppress known audio diagnostics. C implements the project creation branch and template folder files and preserves CreateProject rollback/provenance. The contract's branch explicitly returns Unsupported; it does not pretend Empty is Basic3D. No library, icon bundle or asset download is required.

Preferences use `user://Editor.json`, preserving RecentProjects and unknown members. The Automation panel manages the existing editor listener and session file, not a second editor. CLI explicit automation and one-shot exclusion take precedence. Pausing rejects new agent work with Busy (except session/discovery); already admitted operations finish or cancel normally. Denying mutations also guards command/file-write boundaries, since `project.validate {fix}` is not globally flagged Mutates. User origin and sandboxed dry runs remain allowed. Request telemetry is bounded. The external-editor command is executable plus argv substitutions, never shell text.

Script creation/type-check UI remains explicitly unavailable until M13's real service arrives; M10 must not report a fake type-check or successful unsupported script operation. All available asset operations already have automation equivalents.

### 7. Method contracts

All four new methods use reflected camelCase params/results, standard presence handling, finite validation and ifRevision. None is a new MCP tool: §13.8 reaches them through engine_call.

| Method | Params/result | Mutates | Dry run | Batch | Launcher/Runtime |
|---|---|---|---|---|---|
| viewport.camera | optional position/target; current position/target | false | false | false | false/false |
| viewport.frame | required nonempty entities; fitted pose and resolved summaries | false | false | false | false/false |
| viewport.setOptions | optional grid/gizmos/colliders/icons/wireframe; all current values | false | false | false | false/false |
| editor.state | NoParams; panels/mode/selection/selectionTarget/lockstepOwner/uiFrame/sceneChangedOnDisk | false | false | true | false/false |

They work without a GPU. Camera edits validate the complete pose atomically; frame resolves every entity before moving and uses the existing bounds behavior. These are editor-session changes, never scene dirty/history changes. Existing project.open adds optional recover and result booleans; existing project.create's template enum adds Basic3D. Their registrations/catalogue are parent integration files, not silently changed by this worker. Until wired, a raw RPC recover is rejected as unknown; a typed recover call reaches the marked Unsupported helper. Basic3D likewise reaches the explicit marked helper.

### 8. Contract review corrections

The seven review findings are resolved at contract level; no feature implementation is claimed. Marked stubs and skipped regression cases remain intentional.

- Fatal autosave claims a single disk-writer lease as well as an immutable snapshot slot. The lease covers payloads, manifest publication and retention; normal and fatal writes cannot publish concurrently. Reset closes admission atomically with claims and polls quiescence without blocking. A pending Reset prevents Save/Publish from reopening admission. After successful Reset, the host may Publish for a new open epoch. The host must integrate the close handshake before every project unlock/unmount, including shutdown; merely clearing a pointer is insufficient.
- Recovery freshness uses the parent's existing amendment, not an alternative clock. Opaque ModificationTime is equality-only; a stored dirty revision derives from a captured source fingerprint which still matches. The regression cases exercise equal/wrapped timestamps, changed hashes with equal metadata, clean/identical payloads, and persisted ordering after restart.
- Already-open recovery is a host-injected CPU action, outside ImGui traversal. The host caches an offer at open, owns queued decisions, rechecks the original offer/project/revision at application, and reports completion/errors to the modal. Decline changes only the current offer. No new RPC, second open or lock handoff is introduced.
- Thumbnail binding and cancellation are explicit. Closing/reopening a project retires pending work and results before its mounts disappear, even when asset handles and versions match the next project. No stale request is silently rebound.
- EditorViewportImage now carries the source RenderSnapshot/RenderContext FrameIndex and extracted SceneRevision alongside Generation. EditorViewportClick copies all three from the displayed image; EditorUiState::CompletedFrame is a different clock and is forbidden here. RequestPick queues during Draw. The host drains that exact image/table/frame only from the parent's Application::OnRenderSubmitted(frameIndex, submissionId), after SceneRenderer::OnSubmitted, before image overwrite. Frame-slot reuse includes picker copy submissions. Camera/scene/resize/new-click invalidation and deleted UUIDs discard results before selection changes; stale misses cannot clear a newer selection. ImGuizmo hover/active states suppress picking.
- Fresh minimized screenshots request the parent's `Application::RequestOffscreenUiFrame() -> Status`; errors propagate. The host uses normal rendering hooks for that frame and the same post-submission handoff. This worker does not change Application or GPU profiler contracts.
- GLM vector parameters in the original M10 headers/definitions now use const references, and ProjectMethods.cpp includes are alphabetized. The parent-added SetPixelSize(view, size) must use the same const-reference size signature when integrating this patch; its actual-size semantics and existing tests are unchanged.

Review regressions are added to the existing Autosave, RecoveryMethods, EditorLayer and ThumbnailCache suites and Python autosave suite. EditorViewportHost scenarios in EditorLayerTests are B/integrator-owned (A coordinates the file): out-of-order clicks, camera changes, resizing, scene swaps, deleted UUIDs, stale misses, ImGuizmo suppression, exact post-submission pairing and UI/render frame identity divergence. CPU doubles cover rejection deterministically; actual copy submission/retirement remains covered by the parent's GPU host scaffolds and M9 picker tests.

## Frozen headers and exact implementation ownership

Every new header listed below and the public additions to EditorContext.h, ProjectManager.h, ProjectMethods.h and ScreenshotMethods.h are proposed frozen interfaces. Private storage/helpers may be added by the implementation owner. Public changes require the contract owner's review. No stream edits another stream's files.

| Owner | Production files | Tests |
|---|---|---|
| **A — panels/drawers** | `Editor/EditorLayer.h|.cpp`, `Editor/EditorPanelContext.h`, `Editor/Panels/SceneHierarchyPanel.h|.cpp`, `InspectorPanel.h|.cpp`, `Editor/Drawers/ReflectedDrawers.h|.cpp`; `EditorCore/EditorActions.h|.cpp`, `EditorUiState.h|.cpp`, `EditorSelection.cpp`, `EditorCore/Inspector/ReflectedEditController.h|.cpp` | mirrored `EditorActionsTests`, `EditorUiStateTests`, `Inspector/ReflectedEditControllerTests`; new `EditorSelectionTests`; `Tests/Source/Editor/EditorLayerTests.cpp` except screenshot/GPU integration cases coordinated with integrator |
| **B — viewport/camera/gizmo** | `EditorCore/Viewport/{EditorCamera,EditorViewportState,GizmoController}.h|.cpp`; `Editor/Viewport/{EditorViewportHost.h,GizmoOverlay.h|.cpp}`; `Editor/Panels/{SceneViewportPanel,GameViewportPanel}.h|.cpp`; `Editor/Icons.h|.cpp` | mirrored `EditorCore/Viewport/*Tests.cpp`; `Tests/Automation/test_editor_viewport.py`; additional Editor process input/resize/pick tests routed via integrator |
| **C — content/thumbnails/launcher/template** | `Editor/Panels/ContentBrowserPanel.h|.cpp`; `Editor/{ProjectLauncher,FolderPicker}.h|.cpp`; `EditorCore/Thumbnails/ThumbnailCache.h|.cpp`; `EditorCore/Project/{Basic3DTemplate,ProjectManager}.h|.cpp`; `Resources/Templates/Projects/Basic3D/**` (created in implementation) | mirrored thumbnails/basic template tests; existing ProjectManagerTests additions; `Tests/Automation/test_basic3d.py`, `test_thumbnails.py`; launcher/folder tests |
| **D — utility panels** | `Editor/Panels/{ConsolePanel,DiagnosticsPanel,ProjectSettingsPanel,StatsPanel,AutomationPanel,UndoHistoryPanel}.h|.cpp`; `EditorCore/EditorPreferences.h|.cpp`; `EditorCore/Automation/EditorAutomationControls.h|.cpp` | mirrored preference/control tests; listener preference case in `Tests/Automation/test_editor_ui.py` via E as that file's owner |
| **E — autosave and methods** | `EditorCore/Autosave/Autosave.h|.cpp`; `EditorCore/Automation/{ViewportMethods,EditorMethods,RecoveryMethods}.h|.cpp`; `EditorCore/Automation/FreshEditorScreenshot.cpp`; `EditorCore/Automation/ProjectMethods.h|.cpp` (including C's requested Basic3D registration); E implements service hooks as requests to integrator for shared files | mirrored autosave and new method tests; `Tests/Automation/test_autosave.py`, `test_editor_ui.py`; new fresh-screenshot/selection round trips |
| **Integrator / parent** | `Editor/EditorApp.h|.cpp`, `EditorCore/EditorContext.h|.cpp`, `EditorCore/Play/EditorPlayController.h|.cpp`, `EditorCore/Automation/{AutomationServer,EditorMethodContext,ScreenshotMethods,EditMethods,RegisterMethods}.h|.cpp`; premake, ModuleRules, all Engine/App/ImGui bridges, generated catalogue/reference, Tools/MCP, root authoritative docs | existing RegisterMethods/ScreenshotMethods/PlayController/EditMethods/EditorContext tests; `Tests/Source/Golden/{GoldenTests,EditorDefaultLayoutTests}.cpp`, existing EditorViewport/EditorScreenshotOptions/EditorFaultInjection tests; MCP schema/conformance/launcher tests |

Unqualified production paths in the table are relative to `Editor/Source/`. Mirrored unit tests are under `Tests/Source/`. A owns EditorLayer composition; other streams submit wiring to A. Integrator owns EditorApp scheduling, service construction/destruction, listener mutation and method registration. E owns the one Python `test_editor_ui.py` file so D/integrator send additions instead of concurrent edits. Parent owns this ADR after review and owns all cross-milestone merges.

## Shared integration checklist

### Implementation contract review

The contract owner reviewed the first implementation reports and approved these additions:

- `EditorPanelContext::Gizmos` borrows the host's single `GizmoController`. Its lifetime follows the other injected services; only the Scene view consumes its preview.
- `EditorViewportImage::Camera` owns the camera copied from the same rendered snapshot as its texture, frame index, revision and generation. The gizmo must not use a newer live camera to interpret an older displayed image.
- The reflected drawer context may inject a synchronous reference search returning owned UUID, label and path candidates. The caller supplies scene/asset access and honors the field's asset filter; drawers never retain component pointers. Missing search injection preserves direct reference entry. Search failures are displayed, and selecting a result follows the existing single-command edit path.
- `AutosaveSpecification::WriteFile` may inject a native atomic writer for deterministic failure/interleaving tests. The default remains `FileSystem::WriteFileAtomic` without backups. The callback is covered by the sole writer lease and the fatal-path lifetime restrictions. Only `WriteFatalSnapshot` may catch exceptions from that best-effort write and return `IoFailure`, without logging, allocating an error or recursively invoking fatal handling; the nonallocating lease release still executes. Architecture §4.6 and the lint boundary list record this exception boundary.

These additions do not mark M10 complete. The parent still owns live host construction, target-aware selection on Stop, recovery identity handoff, UI test integration and the strict final gates.

The second implementation review approves `Autosave::OpenRecoveredProject` with a transferred locked project and validated offer, and a borrowed `AutomationServerSpecification::AutosaveService`. Recovery must preserve its original fingerprint and untitled token in the same long-lived service that later cleans up an explicit save. Validation happens before mounting or changing editor state; failure retains the launcher and releases only the transferred lock. A missing service refuses recovery rather than silently discarding identity.

Automation controls use server-owned policy, bounded request activity, and transactional preference-owned listening. `AutomationServerSpecification::Listen` retains launch intent; `PreferenceListeningAllowed` is false for one-shot runs. The UI injects memory-only preference state and a queued change callback; disk and listener changes occur at the safe point. A source-editor helper builds executable and separate argv values using single-pass `{path}`/`{line}` expansion, never shell text; the host retains the spawned process until exit.

Tests compile the production `Editor/` UI sources directly with their PCH disabled, excluding `EditorApp.cpp` and `EditorMain.cpp`. This enables real headless ImGui interaction tests without making EditorCore depend on ImGui or linking the Editor executable. Process/graphics host behavior remains tested through the built Editor.

Selection integration records which scene owns the selected UUIDs. Legacy selection calls and `edit.select` address the shown scene (play while running, otherwise edit); `edit.getSelection` resolves the recorded target. Stop retains only UUIDs still present in the edit scene and changes the target to Edit. Opening/replacing/closing an edit scene clears the target and asset selection. A dry run restores UUIDs, selection target and selected asset together. No new wire parameter is added to `edit.select`.

1. Create CPU services after EditorContext and before EditorLayer, inject the viewport host, controls and thumbnail renderer. Keep the demo until contract staging ends; then replace it with EditorLayer. Use the existing ImGuiLayer ini-path API. Reset default layout only when no saved layout exists or explicitly requested.
2. Register new method types before freeze, methods in RegisterMethods, Basic3D and recover fields in ProjectMethods, and editor.screenshot as pending. Regenerate Tools/MCP/catalog.json; update method/schema counts and examples. All new methods require in-process and Python calls for coverage.
3. Honor current-loop ordering for pending screenshots/resize/input. Retire every texture key before shutting down render services. Route M9 flags/picking/stats without copying or changing M9 files in this contract branch.
4. Wire target-aware selection in EditMethods, MakeSelection, context close and play stop/load; integrate the CPU gizmo preview into scene-view extraction and keep the game view on the runtime scene.
5. Add the autosave hook with the publication/fatal lifetime rules above, invoke before play, after command safe points and before GPU operations, inspect recovery on UI/CLI opens and clear only matching recovery after save. EditorLayer drains human actions before AutomationServer::Pump; the host then publishes the complete post-Pump snapshot before GPU operations. The pre-play hook belongs in EditorPlayController::Start (or its injected service) so both UI and RPC paths run it; an EditorApp toolbar-only hook is insufficient. Inject time in tests; do not add sleeps.
6. Implement human-origin action contexts without relaxing read-only, dry-run/provenance or validation rules. Integrate pause/mutation denial in server admission and actual editor write boundaries; expose bounded request telemetry. Preserve listener/session behavior and explicit CLI intent.
7. Replace the **ImGuiDemo** golden with **EditorDefaultLayout** after visual review, preserving golden GPU coverage. Replace the old fatal-hook text assertions in EditorFaultInjection tests with dirty-scene autosave/recovery assertions; preserve every fault scenario. Keep these files integrator-owned.
8. Update the MCP crash/recovery probe: it currently treats *any* file under Library/Autosave as a recovery. It must recognize a complete published manifest, not incomplete payloads. Add bridge tests and preserve attached-editor ownership semantics.
9. Run the parent's scheduled contract gate only for the contract snapshot. Implementation/final milestone gates are strict: no stub marker or non-child skip may remain. Do not present a header compile as a full build or claim remote platforms verified.

## MCP recovery publication probe

The bridge advertises an autosave candidate only when `Manifest.json` publishes a complete generation with matching
sequence, confined regular files, bounded sizes, payload XXH64 and project/source byte fingerprints. Stray payloads,
temporary writes and unpublished directories cannot advertise recovery. It checks the manifest again after the read.
The standard-library implementation shares the engine's seed-zero XXH64 reference vectors and adds no dependency.
Opaque C++ file-clock values are validated as uint64, not converted to Unix times or ordered. Final timestamp equality
and scene semantics remain the editor's responsibility; this read-only crash diagnostic never adopts recovery or changes
ownership of an attached editor.

## Lifecycle integration amendment

The contract owner adds `EditorLifecycleCallbacks` to `EditorContext`, with fallible `BeforePlay`, `AfterSceneSaved`
and `BeforeProjectClose` hooks. These main-thread callbacks capture host services that outlive the binding. The host
clears the binding only after the fatal hook and asynchronous writers are quiescent, before destroying those services.
Empty callbacks preserve CPU-only contexts. This places UI and automation on the same autosave boundary.

`EditorPlayController` calls `PrepareForPlay` after validating the requested session, before copying its scene. A failed
pre-play save prevents the session from starting. `MarkSceneSaved` now returns `Status`: the durable file write and clean
save point precede recovery cleanup. Cleanup failure is reported with a hint that the scene was saved; the saved scene
is not marked dirty again. Dry runs omit both lifecycle side effects.

`CloseProject` now returns `Status` and runs the close hook before stopping play or releasing the scene, mounts or lock.
A refused close leaves the project open for retry. The destructor verifies successful closure; the concrete host must
finish its writers and remove the borrowed callbacks before normal destruction. Regression tests exercise close refusal
and retained locks, durable-save ordering and failed cleanup, dry-run exclusion, and failed/successful pre-play preparation.

## Scaffold and verification record

### Editor layout and fatal-path acceptance

`EditorDefaultLayout` replaces the M5 `ImGuiDemo` golden after review of the actual headless editor screenshot.
It creates a Basic3D project, opens its scene and calls `editor.screenshot` with isolated preferences. The first layout
and an explicit reset select SceneViewport and ContentBrowser; existing saved layouts and later user tab choices stay
intact. The reviewed 1600×900 image shows the hierarchy, scene, inspector and content browser without machine paths.
A repeated Debug capture matched every pixel on nvidia-61x. The legacy LitScene fixture explicitly disables shadows
and SSAO and still matches its existing reference exactly; no image-comparison threshold changed.

`MakeEditorAutosaveFatalHook` is a private host helper shared by EditorApp and fault tests. Its callback only attempts
to write previously published CPU bytes through the autosave writer lease. It never reads live scene state or uses the
GPU. Tests exercise the same callback during device loss and GPU timeout, recover the published dirty scene, and verify
that later unpublished edits are absent. The host quiesces this callback before clearing lifecycle bindings or destroying
the autosave service. Normal save/close and the fatal path therefore share the same recovery publication contract.

### Recovery decisions and live fault injection

The private `EditorHostRecovery` helper owns the authoritative recovery bytes and queues UI decisions. Each offer is
bound to the current project epoch, project identity and editor revision; acceptance rechecks the binding at both
enqueue and execution. Monotonic offer IDs survive reset. Repeated inspection in one epoch preserves dismissal. Acceptance
uses the existing autosave service and lock. Decline of the matching offer only dismisses it in memory, including after
a binding conflict, so stale recovery cannot trap the user in the modal. Failures remain visible without adopting or
overwriting scene state. Real ImGui interaction tests exercise acceptance, decline, intervening edits and project changes.

EditorLayer initializes ImGuizmo once at the start of each production UI frame, after ImGui frame setup. Interaction
tests use that same entry point without a harness-only BeginFrame call, so gizmo hover and selection behavior exercise
the real frame lifecycle.

The contract owner approves the borrowed `AutomationServerSpecification::QueueDeviceLost` callback for the test-only
`debug.deviceLost` hook. Dispatch only queues intent. The host consumes it after publishing the post-dispatch CPU
snapshot and the next real rendering submission takes the fatal path. The hook requires an open project, enabled
test hooks and a rendering host; it is absent from public catalogues, MCP tools, Runtime, batches and dry runs.
Its regression edits a running editor, injects the fault, then recovers that exact dirty edit in a fresh editor while
checking the original source bytes and all GPU validation diagnostics.

### Third implementation review

The parent approves three queued content-host services on `EditorPanelContext`: a memory-only thumbnail texture lookup,
draining copied OS-drop paths for the current project epoch, and queued audio preview play/stop. Draw callbacks perform
no file I/O or asset loading. The host validates the epoch, performs work at a safe point, reports failures, and retires
texture registrations before rebinding the cache. Asset drag payloads remain exactly sixteen hex digits plus NUL.

Dispatcher admission and activity hooks run on the main thread. Each started request receives a nonzero monotonic
sequence, a Started event before admission/lookup, and exactly one Succeeded, Failed or Cancelled terminal event.
Notifications have outcomes even without responses. Never-started disconnected requests emit nothing; pending requests
are admitted once. Result-production failures remain failures even after disconnect. Empty callbacks preserve existing
Runtime behavior. The server retains only the newest 256 events.

The editor policy also guards actual commands and project file writes by command origin. Denied agent mutations return
PermissionDenied; humans and disposable dry runs remain allowed. SceneEdit checks before committing either edit or play
changes and cancels on denial, covering optional validator fixes and transient edits outside persistent command history.

The contract owner approves `AssetWriter::MutationGuard = UniqueFunction<Status()>` and `SetMutationGuard`.
This optional main-thread guard runs at the beginning of Write, Remove, Move and CreateDirectories, before VFS access
or any side effect, including backups and destination-parent creation. Failure is returned unchanged and produces no
watcher update or listener event; an empty guard preserves existing behavior. The guard also runs during dry runs:
SetDryRun(true) alone does not bypass admission. EditorContext binds CheckMutationPermission, covering manager-generated
metadata from explicit refresh and implicit path resolution without marking read methods as mutating. Humans, passive
refresh and disposable EditorDryRunScope overlays remain allowed. The callback borrows EditorContext; its services remain
alive while the manager drains, and the binding is cleared after CloseProject and before editor member destruction.

Application submission callbacks run after executing the scene/UI list and before presentation; frame pacing tracks the
device's final submission after the callback. A fresh UI request can render into an offscreen target while minimized,
retaining the last usable extent and creating new ImGui draw data. `ImGuiLayer::BeginFrame` accepts an optional retained
display-size flag for this path; normal frames preserve the previous backend behavior.

Frame-loop diagnostics expose an owned application snapshot of completed frames, measured FPS, CPU frame duration
before throttling, and cumulative scheduler dropped seconds. The optional monotonic diagnostic clock enables exact
tests and never drives simulation or pacing. Counters survive loop configuration changes; hosts subtract a baseline
for each play session. Stats callbacks only copy these observations and each displayed renderer's history through
`MakeStatsViewSummary`, retaining exact CPU and delayed GPU frame identities and explicit unavailable samples.

Successful project-settings installation advances the editor revision after the file write and in-memory replacement.
Undo and redo use the same path; failed writes/validation do not advance it and dry runs restore the original base.
The settings panel's interaction regression demonstrated why this is required: a draft captured before an agent edit
must conflict instead of overwriting the newly installed settings.

The initial staged contract adds 90 named C++ and 14 Python acceptance scaffolds, including both exact Roadmap unit names, EditorDefaultLayout, and all four named Python acceptance cases. Each new case is explicitly skipped and fails by design when forced; existing tests are not skipped or weakened. Runtime symbols are not linked from the Editor executable into Tests.

Initial staged-contract selected-file verification on this worktree: includes, banned APIs (clang-query), contract markers, naming (clang-tidy 22.1.3), standalone headers (Clang 22.1.3), and Python syntax/style passed with zero findings. The selected set contained 89 C++ files (37 headers) and 5 Python files; contract mode reported 100 marked stubs and 104 skipped cases. Format was clean and git diff --check passed. Logs are local ignored files under bin/M10Contract. No full linked build, PreCommit, CI, commit or push was run by this worker. Full gate/review/commit scheduling remains with the parent.

The unstaged review correction adds 24 C++ and 2 Python regression scaffolds. Focused contract-mode lint on the corrected files passed with zero findings: include/layer rules, banned APIs, contract markers, clang-tidy naming, 11 standalone headers and Python checks. Its scope contained 21 C++ files and one Python file, with 31 marked stubs and 63 intentional contract skips. Scoped formatting and whitespace checks passed. These checks do not execute the skipped regressions or establish feature completion. The original staged contract remains unchanged; only the unstaged correction patch is handed to the parent, with no whole gate, staging or commit here.

### Synthetic device-loss diagnostics

Contract-owner-approved amendment for M10's dirty-scene crash/recovery test: `GpuDiagnostics::SetDeviceLost(bool injected = false)` publishes a sticky synthetic-loss marker together with the lost flag. Both bits share one atomic state, published with release ordering and read with acquire ordering. `IsDeviceLossInjected()` is true after `SetDeviceLost(true)` or the CLI device-loss injection reaches its first submission. Ordinary `SetDeviceLost()`, later native reports, `ResetCounts`, and `RaiseDeviceLost` never clear synthetic provenance. `GetInjectedFault()` continues to report only the immutable command-line configuration.

`GraphicsDevice::DescribeDeviceFault` never calls `vkGetDeviceFaultInfoEXT` for synthetic loss, because Vulkan requires an actually lost native device for that query (VUID-vkGetDeviceFaultInfoEXT-device-07336). Healthy devices and unavailable extensions retain their existing responses; CLI injection retains its existing explanatory text. A dynamically injected loss reports `no fault information (the device loss was injected)`. Native loss without synthetic marking retains the existing fault-description query and diagnostics.

`debug.deviceLost` remains a test-hooks-only method on a rendering editor with an open project. Its callback queues intent only. EditorApp publishes the current CPU autosave snapshot at the safe point before consuming the request with `SetDeviceLost(true)`; the next real submission follows the normal fatal path. No rendering or live-state access occurs inside the fatal autosave callback. The test requires no validation errors or warnings, verifies unchanged source bytes, and recovers the dirty entity through a fresh Editor process under Vulkan 1.4 and 1.3.

Regression coverage includes native-only loss, sticky mixed native/synthetic reports, cross-thread observation of the paired flags, CLI fault behavior, and description/teardown of a healthy native device marked synthetically lost.
