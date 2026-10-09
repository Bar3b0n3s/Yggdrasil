# Roadmap

Status: authoritative plan, v1.2, 2026-10-05. Section references (§) point to `Docs/Architecture.md` v1.2, whose Appendix D lists the review findings this plan schedules. v1.2 incorporates the accepted M0 deviations of `Docs/Decisions/0002-m0-deviations.md` (M0 deliverables; `config.h` moves to the M1 contract task).

The roadmap goes from an empty repository to a finished engine and editor (M15), then to two demo games built by an AI agent through editor automation (M17 Tetris, M18 Rolling Ball 3D), after a time-boxed dry run (M16).

## Rules for every milestone

1. **Green or not done.** A milestone is complete when `python Scripts/CI.py` passes on Windows x64, with every stage that exists at that point running in the configurations the §15.8 matrix names for it (builds in Debug, Release and Dist; unit, GPU and editor FeatureTest in Debug and Release; exported FeatureTest in Release and Dist; the `determinism` stage across all of them). A recorded review (§15.9) and a commit follow. Because simulation is deterministic across configurations (§9.1), a replay or expected hash recorded in one configuration must verify in all of them; a configuration-dependent result is a bug, never a reason to store per-configuration expectations. Linux and macOS are *generation-checked* by the `portability` stage; they are never reported as verified without CI on that OS.
2. **Session-sized tasks.** Each milestone is split into tasks that one engineer or agent finishes in one session, each with its own tests. A task never leaves the tree red.
3. **Interfaces first.** The first task of a milestone ("contract" task) commits the public headers of the milestone's modules with documented signatures, stub implementations returning `Unsupported` whose first statement is the contract-stub marker `ENGINE_CONTRACT_STUB();` (`Core/Base.h`), and failing-by-design tests marked `doctest::skip`. Parallel tasks then implement against frozen headers; changing a frozen header requires the contract owner's review. The contract commit alone runs the gate in contract mode (`PreCommit.py --contract`, which passes `Lint.py --allow-contract-stubs` and `Test.py --allow-skips`); every other commit is strict, so a marker or a skipped test outside the child-process targets (`Test::ChildTargetSuite`) cannot survive into a milestone commit (`Docs/Decisions/0004-contract-stub-gate.md`).
4. **File ownership.** Parallel workstreams own disjoint directories. Shared integration files have a single owner per milestone: the premake files, `Scripts/ModuleRules.json`, `BuiltinComponents` (`Scene/Components/BuiltinComponents.h`), `Engine/Source/Engine/Scripting/RegisterBindings.cpp`, `EditorCore/Automation/RegisterMethods.cpp` and `Docs/Reference/*` (generated). Other workstreams send changes to these files through the owner (or land them in the final integration task).
5. **Parity.** Every editor-visible capability lands with its automation method and a Python test in the same milestone (P6). Every new component, field, enum value, project setting, importer/extension, script function, method, property or callback gets its FeatureTest coverage, in every run mode it supports (§15.6), at the latest in M14, and from M14 on in the same change.
6. **Approvals first.** Every operator approval (downloads, vendoring) is requested in M0's approvals task, never in the middle of a milestone.

## Milestone dependency graph

| Milestone | Depends on | Can run in parallel with |
|---|---|---|
| M0 Bootstrap | — | — |
| M1 Core | M0 | — |
| M2 Platform and app loop | M1 | — |
| M3 Reflection, scene, serialization | M1 | M2 (after M1), M5 |
| M4 EditorCore, automation core, MCP | M2, M3 | M5 |
| M5 Graphics foundation | M2 | M3, M4 |
| M6 Assets | M3, M5 (GPU upload only) | M4 (late) |
| M7 Walking skeleton | M4, M5, M6 | — |
| M8 Renderer I | M7 | M11, M12 |
| M9 Renderer II | M8 | M10 (panels not needing picking), M11, M12 |
| M10 Editor UI | M7 (M9 for picking/outline) | M11, M12 |
| M11 Physics | M7 | M8–M10, M12 |
| M12 Audio | M7 | M8–M11 |
| M13 Scripting | M11, M12 | M9, M10 |
| M14 FeatureTest and docs | M10, M13 | — |
| M15 Export, hardening, engine + editor complete | M14 | — |
| M16 Breakout dry run | M15 | — |
| M17 Tetris | M16 | — |
| M18 Rolling Ball 3D | M16 (M17 recommended first) | M17 (different agent) |

---

## M0 — Bootstrap

**Scope.** Workspace, build scripts, quality tooling and agent guidance, with empty projects that build in every configuration. The deterministic floating-point setup and all operator approvals are settled here, because later milestones depend on them.

**Deliverables**
- `premake5.lua`, `Dependencies.lua` (`ApplyFirstPartySettings`, `UseNVRHI`, `UseJoltPhysics`, `UseSpdlog`, `UseMiniaudio`, `UseLuau`, `UseGLFW`, `UseImGui`; defines copied from each `VENDOR.md`), workspace-scope Dist `NDEBUG`.
- **Deterministic floating point (§2.2, §9.1).** First-party projects use the precise FP model (MSVC `/fp:precise`; GCC/Clang `-fno-fast-math -ffp-contract=off` on every architecture, clang-cl `/clang:-fno-fast-math /clang:-ffp-contract=off`; not `-ffp-model=precise`, which implies `-ffp-contract=on`, §2.2). `Vendor/JoltPhysics/premake5.lua` changes from upstream's MSVC `floatingpoint "Fast"` and ARM64 `-ffp-contract=on` to the precise model and the same flags, and defines `JPH_CROSS_PLATFORM_DETERMINISTIC` in every configuration. `UseJoltPhysics()` defines it for every consumer. `Vendor/JoltPhysics/VENDOR.md` records the change in its build-configuration tables (option `CROSS_PLATFORM_DETERMINISTIC` = ON, the FP flags, the ~8 % cost); Jolt's sources stay byte-identical to upstream.
- `Engine/premake5.lua` (StaticLib, PCH, `dependson { "Shaders" }`), a `Shaders` utility project (custom build rule on `Resources/Shaders/Shaders.json` with `buildinputs` for every `**.slang`/`**.h` and a stamp output; for ninja the rule sits on `Engine` itself, §2.2), `Editor/premake5.lua` (`EditorCore` StaticLib + `Editor` ConsoleApp, both `removeconfigurations { "Dist" }`), `Runtime/premake5.lua` (WindowedApp in Dist, `/MAP`; macOS `-Wl,-rpath,@executable_path/../Frameworks`), `Tests/premake5.lua` (`removeconfigurations { "Dist" }`).
- `Engine/Source/EnginePCH.h|.cpp`, `Engine/Source/Engine/Core/Base.h` (`Scope`, `Ref`, `CreateScope`, `CreateRef`, flag-enum helpers), `Engine/Source/Engine/Platform/Windows/App.manifest`, stub mains (`Editor/Source/Editor/EditorMain.cpp`, `Runtime/Source/Runtime/RuntimeMain.cpp`, `Tests/Source/TestMain.cpp`).
- Scripts: `Setup.py` (premake download + SHA-256, SDK checks), `Generate.py`, `Build.py`, `Test.py` (unit suite; `--config`), `Format.py`, `Lint.py` (CheckIncludes step with the explicit layer-5 DAG and the NVRHI-free snapshot-header rule, banned APIs incl. CRT transcendental functions on the simulation path, clang-tidy naming with regex fallback, header self-containment, JSON validity), `CheckBuildConfig.py` (ABI defines, `JPH_CROSS_PLATFORM_DETERMINISTIC`, FP model), `CI.py` (stages setup → generate → lint → build → unit → portability, with the §15.8 configuration matrix), `PreCommit.py`, `Scripts/Lib/`, `Scripts/ModuleRules.json`, `Scripts/Premake/CompileCommands.lua`.
- `.clang-tidy` (naming rules of Appendix A), `.gitignore` additions (`Library/`, `Tools/MCP/.venv/`, `bin/`, `.mcp.json`), `AGENTS.md`, `CLAUDE.md`, `Docs/ReviewChecklist.md`, skills `build-and-test` and `commit-review` (§13.10), `.github/workflows/ci.yml` (optional until a remote exists).
- **Approvals task.** A single request to the operator, recorded in `Docs/Decisions/0001-approvals.md`, lists every approval the roadmap needs, with source, license, size and checksum: the two default Poly Haven HDRIs and the Inter font (M6), MikkTSpace vendoring (M6), and the pinned FLAC, MP3 and OTF format fixtures (M14). Milestones use the recorded answers; a declined item takes its documented fallback (Appendix C).

**Acceptance**
- Debug, Release and Dist build with zero warnings at `/W4 /WX`; `Tests` runs `"Smoke: test runner starts"` and exits 0.
- `CheckBuildConfig.py` passes on the real workspace and fails on three fixture workspaces under `Tests/Data/BuildConfig/`: `MismatchedJoltDefine/` (a consumer without `JPH_PROFILE_ENABLED`), `MissingDeterministicDefine/` (a consumer without `JPH_CROSS_PLATFORM_DETERMINISTIC`) and `FastMathJolt/` (Jolt with `floatingpoint "Fast"`).
- `"Jolt: version ID includes JPH_CROSS_PLATFORM_DETERMINISTIC"` (the feature bit is set in `JPH_VERSION_ID` and `VerifyJoltVersionID()` passes).
- Generated `.slnx` has `Build=false` for EditorCore, Editor and Tests in Dist (checked by `CheckBuildConfig.py`).
- Touching only a `.slang` file makes an IDE (MSBuild) build re-run `CompileShaders.py` (`"Shaders: slang-only change is not skipped by the up-to-date check"`, scripted with `Build.py`).
- `Lint.py` fails on seeded fixtures: an upward include, a forbidden layer-5 edge, NVRHI included from `RenderSnapshot.h`, `throw` in first-party code, `std::rand`, `std::sin` in `Scene/`, a member without `m_`, `json::at` outside `Core/Json`.
- Portability stage: `premake5 --os=linux gmake`, `--os=linux ninja`, `--os=macosx xcode4` generate successfully.
- `PreCommit.py` runs end to end; the approvals record exists.

**Parallelization**
| Stream | Owns |
|---|---|
| A Build system | `premake5.lua`, `Dependencies.lua`, `*/premake5.lua`, the Jolt premake/VENDOR.md FP change, `CheckBuildConfig.py`, fixtures under `Tests/Data/BuildConfig/` |
| B Scripts | `Setup.py`, `Generate.py`, `Build.py`, `Test.py`, `CI.py`, `PreCommit.py`, `Scripts/Lib/` |
| C Lint tooling | `Lint.py`, `.clang-tidy`, `ModuleRules.json`, `Premake/CompileCommands.lua`, lint fixtures |
| D Guidance | `AGENTS.md`, `CLAUDE.md`, `Docs/ReviewChecklist.md`, `.claude/skills/*`, `.github/workflows/ci.yml`, approvals request |

---

## M1 — Core

**Scope.** Layer-0 infrastructure and the test harness (§4.4–4.12).

**Deliverables** (`Engine/Source/Engine/Core/`): `Assert.h|.cpp`, `Log.h|.cpp`, `RingBufferSink.h|.cpp`, `LogContext.h`, `Error.h|.cpp`, `Result.h`, `UUID.h|.cpp`, `UUIDGenerator.h|.cpp`, `Hash.h|.cpp` (XXH64, FNV-1a), `Random.h|.cpp` (xoshiro256**), `DetMath.h|.cpp` (§4.12), `Time.h`, `Clock.h|.cpp` (System, Manual, Scripted), `FixedStepScheduler.h|.cpp`, `Json/JsonReader.h|.cpp`, `Json/JsonWriter.h|.cpp` (incl. `std::map` key ordering for `Map` fields), `FileSystem.h|.cpp`, `VfsPath.h|.cpp`, `VirtualFileSystem.h|.cpp`, `Mounts/{NativeDirectoryMount,MemoryMount,OverlayMount}.h|.cpp`, `BinaryReader.h|.cpp`, `BinaryWriter.h|.cpp`, `Jobs/JobSystem.h|.cpp`, `Jobs/MainThreadQueue.h|.cpp`, `EventLog.h|.cpp`, `Handle.h`, `Profiler.h|.cpp`.
Tests support (`Tests/Source/Support/`): recording assert handler, death-test registry and runner, `ExpectLog` listener, `TempDirectory`, `GlmApprox.h`, per-test timeout.

**Acceptance** (`Tests/Source/Engine/Core/*Tests.cpp`)
- `"Result: ENGINE_TRY propagates the error with context"`, `"Error: hint and location survive WithContext"`.
- `"Hash: XXH64 matches reference test vectors"`; `"UUID: hex round trip and 6-digit prefix matching"`; `"UUIDGenerator: seeded generator is reproducible"`.
- `"FixedStepScheduler: table of frame deltas yields expected step counts and alpha"` (≥ 30 rows incl. clamping and dropped time); `"ScriptedClock: cycles its delta table"`.
- `"DetMath: within 1 ULP of correctly rounded references over seeded tables"` (references from `Scripts/GenerateDetMathReference.py`, `Docs/Decisions/0007-detmath-reference-oracle.md`); `"DetMath: output hash over 1,000,000 seeded inputs matches the committed value"` (passes identically in Debug and Release).
- `"OverlayMount: writes stay in memory and never reach the lower mount"`.
- `"VfsPath: rejects parent escapes, absolute paths, NUL and reserved names"`; `"VirtualFileSystem: case mismatch is a validation error"`; `"FileSystem: atomic write survives an injected failure"`.
- `"JsonWriter: floats use the shortest round-trip form"`; `"JsonReader: errors carry JSON pointers"`; `"JsonWriter: load then save is byte-identical"`.
- `"BinaryReader: 10,000 seeded mutations never crash and always return Result"`.
- `"JobSystem: inline mode preserves submission order"`; `"MainThreadQueue: completions drain in order"`.
- Death test `Core/AssertFires` exits 4 with the message; a meta-test proves an undeclared `ENGINE_CORE_ERROR` fails its test case.

**Parallelization**: contract task freezes `Base.h`, `Assert.h`, `Log.h`, `Error.h`, `Result.h`, and adds `Engine/Config/entt/ext/config.h` (§4.5) with the `Engine/Config` include directory in `ApplyFirstPartySettings()` and `ModuleRules.json` (moved from M0: it routes `ENTT_ASSERT` to the `ENGINE_CORE_ASSERT` this task freezes). Then A Log/Assert/Error (+ assert handler support), B UUID/Hash/Random/DetMath/Clock/Scheduler, C Json/FileSystem/VFS (incl. overlay)/BinaryReader-Writer, D Jobs/MainThreadQueue/EventLog/Handle/Profiler, E test support. Owners per file group; no shared files except `Engine/premake5.lua` (owner A).

---

## M2 — Platform and application loop

**Scope.** §4.1–4.3, §4.13 (crash handler, project lock).

**Deliverables** (`Platform/`): `Window.h|.cpp` (GLFW window with `GLFW_NO_API`; minimized wait), `GlfwLibrary.h|.cpp` (process-level `glfwInitVulkanLoader` hook, platform hint chosen once, `glfwInit`), `Events.h`, `Input/InputState.h|.cpp` (`Inject`, `LatchStep`, `LatchFrame`; up-positive stick Y), `Input/InputActionMap.h|.cpp` (incl. `Invert`), `Input/KeyCodes.h|.cpp`, `Process.h|.cpp`, `CrashHandler.h` + `Windows/CrashHandlerWindows.cpp` + `Posix/CrashHandlerPosix.cpp`, `Paths.h|.cpp` (app name for user data), `ProjectLock.h|.cpp` (`LockFileEx`/`flock`, pid readable), `PollingFileWatcher.h|.cpp`, `Socket.h` + `Windows/`/`Posix/` impls. `App/`: `ProcessContext.h|.cpp` (process-level init, §4.1), `Application.h|.cpp`, `EngineContext.h|.cpp`, `EntryPoint.h|.cpp`, `ExitCode.h`, `FrameLoop.h|.cpp` (minimized throttle; the frame-boundary `vk::SystemError` catch lands with the device in M5). Editor and Runtime mains run an empty `Application` subclass.

**Acceptance**
- `"InputState: a tap between two steps reports Pressed and Released on the next step"`, `"InputState: zero-step frames keep step edges pending"`, `"InputState: multi-step frames report an edge once"`, `"InputState: frame edges are independent of step edges"`.
- `"InputActionMap: axis combines keys and gamepad with dead zone"`; `"InputState: stick Y is up-positive and Invert flips it"`.
- `"Window: null platform creates a window and accepts injected events"`; `"ProcessContext: initializes GLFW once with the chosen platform and tears down in reverse"`; `"Tests: windowed child process runs a windowed case"` (`--windowed-child`).
- `"ProjectLock: a second lock fails and names the pid; killing the holder releases it"`.
- `Tests --crash-child` writes a crash report and exits 4 (`"CrashHandler: child crash produces report and exit code 4"`).
- `"Process: captures exit code and output"`; `"Socket: loopback echo round trip"`; `"PollingFileWatcher: create, modify and delete are reported once after debounce"`.
- `Editor --headless --frames 10` (stub) exits 0 using `ManualClock`; Editor windowed opens and closes under a 10 s timeout test; a minimized window idles (CPU time per second below a threshold, windowed child test).

**Parallelization**: A Window/GLFW library/Events/Input/actions, B Process/CrashHandler/Paths/ProjectLock, C FileWatcher/Socket, D ProcessContext/Application/EngineContext/EntryPoint/FrameLoop (contract task freezes `Events.h`, `InputState.h`, `ProcessContext.h`, `Application.h`, `EngineContext.h`).

---

## M3 — Reflection, scene, serialization

**Scope.** §5 (data side), §6.1–6.3 formats.

**Deliverables**: `Reflection/` (`TypeRegistry`, `TypeInfo`, `FieldInfo` (incl. `MinMagnitude`, `RunModes`), `EnumInfo`, `Value`, `FieldType::Map` and `FieldType::Variant` with `VariantSchemaResolver`, finite-value enforcement, `JsonSchema` (`additionalProperties` for maps), RFC 7386 map patching, `FuzzySuggest`); `Scene/` (`Scene`, `Entity`, `ChangeTracker`, `Components/*.h` for **every** component of §5.3 as data, incl. `RigidBody.EnhancedInternalEdgeRemoval` and the collider minimums, `Components/BuiltinComponents.h`, `Registration/{Core,Rendering,Physics,Audio,Scripting}Registration.cpp` with `EntityLevel`/`NoShortcut` flags, `TransformSystem` (writable `WorldPosition`/`WorldRotation`), `SceneSerializer` with **structural pre-validation and repair loading** (§6), `Migrations`, `Prefab` + `PrefabInstantiator` (override key `PrefabEntityID`, `Variant` values), state hash); `Project/ProjectSettings` (reflected; `Input.Actions` as `Map`, `Simulation.MaxEntities`, `Export.Icon/Version/Company`, `Testing` suites and replays) + `ProjectSerializer`; `Tests/Data/Project/AllSettings.eproj`.

**Acceptance**
- Registry-parametrized suite (§5.4) passes for every component and reflected struct: default round trip, 200 random round trips (incl. `Map` and `Variant` fields through fixture resolvers), validation of out-of-range and non-finite values, schema validates output (script/automation parts are added in M4/M13).
- `"Map: keys serialize sorted and merge-patch deletes on null"`; `"Variant: value validated against the resolved schema; unresolvable values are preserved with a diagnostic"`.
- `"SceneSerializer: every fixture under Tests/Data/Scenes re-saves byte-identically"` (`.jsonl` and `Automation/` exempt); `"SceneSerializer: unknown components are preserved"`; `"Migrations: v0 fixture upgrades to v1"`; `"SceneSerializer: newer Version fails with UnsupportedVersion"`.
- One test per structural defect fixture in `Tests/Data/Scenes/Invalid/` (duplicate IDs, zero ID, dangling parent, parent cycle, child before parent, inconsistent prefab link, duplicate unique component): strict loading returns a located `Result` and never asserts; repair loading applies the documented fix and reports it.
- `"Scene: canonical order is independent of insertion history"`; `"Scene: reparenting rejects cycles"`; `"TransformSystem: matches reference matrices"`; `"Transform: WorldPosition and WorldRotation setters round-trip through the parent inverse"`.
- `"ProjectSettings: AllSettings.eproj sets every field to a non-default value"` (the observation half of gate 8 grows with each consumer milestone).
- `"Prefab: instance IDs are Hash64(root, prefabEntity)"`, `"Prefab: field override survives a prefab edit"`, `"Prefab: external EntityRef into an instance survives update"`, `"Prefab: user-added children are preserved"`.
- `"SceneSerializer: serializer copy has the same state hash"`; `"SceneSerializer: 10,000 seeded mutations of a scene file never crash"`.

**Parallelization**: contract task freezes `TypeRegistry.h` (incl. `Map`/`Variant`), `Scene.h`, `Entity.h`. A Reflection, B Scene/Entity/hierarchy/TransformSystem/ChangeTracker, C serializer + pre-validation/repair + migrations + project settings, D component structs and registrations (split by category file), E prefabs (after B and C land).

---

## M4 — EditorCore, automation core, MCP bridge

**Scope.** The headless, renderer-less editor driven by agents (§12.1, §12.3, §13.1–13.8). From here on every milestone adds its methods and Python tests.

**Deliverables**
- `EditorCore/`: `EditorContext` (one open scene), `CommandHistory`, `Command`, `SceneEdit` + `SceneEditCommand`, `CompositeCommand`, `ProjectSettingsCommand`, `ProjectManager` (Empty template; takes the project lock, `--read-only`), `ProjectValidator` (codes available so far; stable diagnostic ids; selective fixes), `Automation/AutomationServer` + domain method files, `ProvenanceRecorder` (committed `Automation/Provenance.json`).
- `Engine/Automation/Protocol/`: framing, TCP server, sessions/handshake, `MethodRegistry` (reflected params/results; `supportsDryRun`, `availableInLauncher`; case-insensitive enum parsing), `PendingOperation` (cancellation on disconnect), watchdog with phase marker, `_meta` builder (incl. `transcriptLine` intake), bounded-output offload.
- Methods: `session.*`, `rpc.discover`, `project.create|open|save|info|getSettings|setSettings|validate|upgrade`, `scene.new|open|save|tree|query|get|diff` (`scene.open` with `save`/`discardChanges`/`reload`/`repair`), `entity.*` except `entity.bounds` (M6, needs meshes), `component.*`, `edit.*` (dry runs through `OverlayMount` and `supportsDryRun`), `log.read`, `events.read`, `docs.get`. Test-only hook `debug.stall {ms}` (enabled by `--automation-test-hooks`, never a tool, excluded from coverage).
- `Editor --headless --renderer none --automation` (launcher state without `--project`), `--batch`, `--upgrade`, `--read-only`, `--dump-reference` (method catalogue part).
- `Tools/Automation/engine_client.py`; `Tools/MCP/` (`run.py` with venv re-exec, `engine_mcp/{server,connection,launcher,transcript}.py`: attach-instead-of-spawn via session files and the project lock, `editor_attach`, always-on transcript into `<Project>/Automation/BuildLog.jsonl`, compact tool schemas, `catalog.json`, `requirements.lock`, `tests/`); Setup.py venv creation and generated `.mcp.json`; `Tests/Automation/` suite and method-coverage counting.
- Skill `add-automation-method`.

**Acceptance**
- `"SceneEditCommand: 10,000 random operations undo to byte-identical JSON and redo to the final state"`; `"CompositeCommand: a failing child undoes executed children"`.
- Python: `test_batch_rollback_reports_failed_op`, `test_ref_substitution`, `test_dry_run_leaves_revision_unchanged`, `test_dry_run_writes_no_file_event_trash_or_provenance`, `test_dry_run_rejects_unsupported_op_with_failed_op`, `test_if_revision_conflict`, `test_bad_token_rejected_and_closed_after_three`, `test_http_probe_closes_socket`, `test_oversized_frame_closes`, `test_busy_watchdog_reports_phase` (via `debug.stall`), `test_meta_reports_new_warning`, `test_large_result_offloaded`, `test_enum_values_case_insensitive_and_echoed_canonically`, `test_launcher_state_allows_only_project_methods`, `test_second_editor_on_locked_project_exits_3`, `test_read_only_editor_denies_mutations`, `test_disconnect_cancels_pending_operations` (via a test-hook pending operation), `test_validate_fix_selected_ids_only`, `test_scene_open_dirty_requires_save_or_discard`, `test_provenance_records_method_request_and_transcript_line`, `test_upgrade_rewrites_and_records_provenance`.
- MCP: `test_launch_attaches_to_running_editor_with_automation`, `test_launch_reports_editor_already_open_without_automation`, `test_transcript_written_without_env_var`, `test_tool_schemas_are_compact`, `test_run_py_reexecs_into_venv`.
- `"Framing: random byte streams never crash the decoder"` (C++).
- MCP: the SDK client lists the catalogue tools without a running editor, launches a headless editor through `editor_launch`, calls `entity_create` and `scene_tree`; killing the editor makes the next call return `EditorCrashed`; the generated `.mcp.json` works with no `python` on `PATH`.
- Method coverage: every registered method has at least one Python test.

**Parallelization**: contract task freezes `Command.h`, `EditorContext.h`, `MethodRegistry.h`, param-struct conventions, the provenance format. A commands/history/SceneEdit/dry-run overlay, B protocol/transport/auth/watchdog/disconnect, C method handlers (split by domain file) + validator + provenance recorder, D Python client + MCP bridge (attach, transcript, compact schemas) + Python tests. Owner of `RegisterMethods.cpp`: C.

---

## M5 — Graphics foundation

**Scope.** §8.1, §8.4 (infrastructure), §8.11–8.14.

**Deliverables**: `Graphics/` (`VulkanDispatch.cpp` (process-level loader init), `GraphicsDevice` (null-checked `Result` wrappers for every NVRHI `create*`; `VK_EXT_device_fault` when available), `DeviceSelection` (pure; `shaderStorageImageExtendedFormats` and per-format storage checks, Bloom format fallback), `Swapchain` (dispatcher C entry points, `VkResult` state machine), `FramePacer` (`pollEventQuery` + bounded `vkWaitSemaphores` on the queue timeline, `GpuHang` after 10 s), `HostImageUpload` (Vulkan 1.4 `hostImageCopy` path with deferred release; staging fallback), `OffscreenTarget`, `Readback`, `RenderTargetPool`, `GpuProfiler`, `GpuResourceTracker`, `GpuDiagnostics` (device-lost flag from the message callback; `--gpu-inject-fault`), `ShaderLibrary`, `PipelineFactory`), the `App/FrameLoop.cpp` frame-boundary `vk::SystemError` catch, `Scripts/CompileShaders.py`, `Resources/Shaders/{Shaders.json, Shared/ViewConstants.h (projection kind, ortho extents), Common/*.slang, Passes/Triangle.slang}`, `ImGui/` (`ImGuiRenderer`, `ImGuiLayer`, `ImGuiGlfwImplementation.cpp`; `imgui.ini` under `user://` before a project opens), `Testing/ImageCompare`, `Tests/Source/Support/HeadlessGpuFixture`, golden infrastructure in `Test.py`; methods `viewport.screenshot` (clear/triangle scene) and `editor.screenshot` (ImGui demo window), registered at the M4/M5 merge with their Python tests (`Docs/Decisions/0009-m5-decisions.md` decision 33); test hook `ENGINE_VULKAN_LOADER` env var (non-Dist) to simulate a missing loader.

**Acceptance**
- `"DeviceSelection: scoring table"` (CPU only; incl. missing storage-format support and the Bloom fallback).
- GPU (synchronization validation on): `"GraphicsDevice: create and destroy with zero validation messages"` under API 1.4 and `--vulkan-api=1.3`; `"Readback: clear colour is exact"`; golden `Triangle`; `"Rasterizer: CCW triangle survives back-face culling"`; `"Swapchain: resize, minimize and out-of-date recover"` (windowed child process, timeout-guarded, out-of-date handled as a `VkResult`); `"GpuResourceTracker: live counts return to zero"`; `"ImGuiRenderer: texture create/update/destroy protocol"`; golden `ImGuiDemo` via `editor.screenshot`; `"Texture upload: host-copy and staging paths read back identically"` (host copy reported as skipped with the reason when unsupported; the staging path runs under both caps).
- Fault injection: `--gpu-inject-fault=device-lost` and `=hang` exit 4 with a crash report (and, in the editor, an autosave); `=oom-texture` yields a placeholder plus diagnostic; a null `CreateTexture` never crashes.
- CPU: `"Shaders: LayoutsMatchReflection"`, `"Shaders: SharedStructsMatchReflection"`, `spirv-val` clean.
- `Editor` with `ENGINE_VULKAN_LOADER=missing` exits 3 with the loader message.

**Parallelization**: A loader/device/selection/creation wrappers/host image upload, B swapchain/present/frame pacing/fault handling, C shader pipeline (script, library, factory, reflection tests incl. image formats), D ImGui renderer, E readback/ImageCompare/golden harness/screenshot methods.

---

## M6 — Assets

**Scope.** §7 (except Environment, Audio and Script importers, which land in M8, M12 and M13).

**Deliverables**: `Asset/` (`AssetHandle`, `AssetRef` parsing, `AssetType`, `AssetMetadata` (incl. dependency metas), `AssetRegistry` + scan diagnostics, the `AssetManager` interface, `RuntimeAssetManager`, loaders per type, placeholders, `AssetDiagnostic`, `PakReader`, `PakMount`, built-in procedural meshes and textures, `EngineAssets.json`, `IEnvironmentBaker`, `IScriptDiagnosticsProvider`); `AssetPipeline/` (`EditorAssetManager`, `ImporterRegistry`, `AssetCache`, engine cooked cache (`bin/EngineCache`, `--bake-engine-assets`), `AssetWriter` (no-echo writes), `TextureImporter`, `GltfImporter` (dependency closure, URI rejection, `TEXCOORD_1`/`COLOR_0` diagnostics, standalone-texture reuse), `MaterialImporter`, `SceneImporter`, `PrefabImporter`, `FontImporter`, `PakWriter`, `AssetDependencyGraph`); hot reload via `PollingFileWatcher` with generation-checked jobs and `SceneChangedOnDisk`; `Renderer/GpuResourceCache` (mesh/texture upload only, through the M5 upload paths); commands `AssetEditCommand`, `AssetMoveCommand`, `AssetDeleteCommand`; methods `asset.*` (`asset.import` copies a glTF's dependency closure), `prefab.*`, `project.refreshAssets`, **`entity.bounds`** (moved here from M4, because it needs meshes); `Tests/Data/Generate/MakeGltfFixtures.py` (deterministic hand-built glTF fixtures: box, textured, normal-mapped with/without tangents, alpha modes, embedded/data-URI, external `.bin` + `.png`, `..` and `http:` URIs, `TEXCOORD_1` occlusion, `COLOR_0`, unsupported required extension); `Scripts/FetchAssets.py` and the committed default HDRIs + Inter font (approved in M0).

**Acceptance**
- `"GltfImporter: every fixture imports"`, `"GltfImporter: required Draco extension is rejected with a precise message"`, `"Importers: importing twice gives identical hashes"`, `"GltfImporter: reimport keeps sub-asset handles"`, `"GltfImporter: parent-escaping, absolute and http URIs are rejected"`, `"GltfImporter: TEXCOORD_1 and COLOR_0 raise their diagnostics"`, `"GltfImporter: an image with its own Texture meta is referenced, not duplicated"`.
- `"AssetRegistry: moved file keeps its handle"`, `"AssetRegistry: duplicate handle diagnostic and auto-fix"`, `"AssetRegistry: case mismatch diagnostic"`, `"AssetRegistry: dependency metas move and trash with their owner"`.
- `"HotReload: an editor write causes no echo reimport"`, `"HotReload: a stale job completion is dropped"`, `"HotReload: a changed open scene raises SceneChangedOnDisk and is not reloaded"`.
- `"AssetCache: corrupted entry is rebuilt"`; `"Pak: round trip"`, `"Pak: flipped byte is detected"`, `"Pak: writer is deterministic"`.
- `"AssetManager: missing texture yields the Missing placeholder and logs once"` (with `ExpectLog`).
- `"HotReload: changed texture swaps and bumps the version"`.
- Python: asset create/set/move/delete/undo, prefab create/instantiate/apply/revert/unpack, refresh after external write, `test_import_gltf_copies_dependency_closure`, `test_entity_bounds_world_aabb`.

**Parallelization**: contract task freezes `AssetManager.h`, `IAssetImporter.h`, cooked format headers, the dependency-meta format. A Asset core + registry, B texture/material/font importers, C glTF importer + fixtures generator, D caches (project, engine) + pak + PakMount, E commands + methods + hot-reload races + Python tests, F built-in meshes/textures.

---

## M7 — Walking skeleton

**Scope.** Edit → play → export → run, end to end, with minimal rendering (§5.6–5.7, §14 v0).

**Deliverables**: `Session/PlaySession` (serializer copy, the §5.7 step order incl. the step-0 interpolation snapshot, `TransformSystem::Update` before `PreStep`, transform/destroy flushes and empty physics/script/audio hooks, lockstep, seeded runtime UUIDs, state hash, `Simulation.MaxEntities` cap); `Scene/RenderExtraction` (interpolation with `InterpolationResetTag` rules, §5.2); `Renderer/RenderSnapshot` (NVRHI-free), `SceneRenderer` with prepass + forward Lambert (one directional light) + Linear tonemap; `Runtime/Source/Runtime/RuntimeApp` (app name from the manifest); `EditorCore/Export/Exporter` v0 (validate scenes, cook, paks with the **target configuration's** SPIR-V and **no environments yet** (they need M8's baker), copy Runtime + `Redist/*.dll`, manifest, smoke test); `Build.py` CRT copy; methods `play.*` (`play.step` with the 50 ms per-frame budget; `stateHash` in `play.state`/`play.step`), `input.inject`, `project.export`; headless non-lockstep throttle; `Engine/Automation/Methods` runtime subset skeleton; Runtime `--automation` (non-Dist).

**Acceptance**
- `"PlaySession: play then stop leaves the edit scene byte-identical"`; `"PlaySession: step order matches the documented sequence"` (instrumented hooks).
- `"Interpolation: script-moved entities interpolate; frame-phase writes, teleports and new entities do not"`; `"PlaySession: entity cap raises an error, never crashes"`.
- Python: `test_lockstep_600_ticks_render_last`, `test_play_step_respects_frame_budget_and_reports_state_hash`, `test_disconnect_releases_lockstep_and_pauses_play`, `test_headless_play_without_lockstep_is_throttled`, `test_tap_event_seen_once_pressed_and_released`, `test_export_tiny_project` (paks, exe, CRT DLLs present), `test_exported_user_data_folder_uses_manifest_name`, `test_exported_runtime_headless_exits_zero` (`--frames 120 --expect-no-errors`), `test_exported_screenshot_matches_editor` (ImageCompare), `test_export_is_deterministic` (two exports, identical paks), `test_runtime_missing_manifest_exits_3`, `test_runtime_automation_subset` (play.step + screenshot against the exported build).

**Parallelization**: A PlaySession + lockstep + play/input methods, B extraction + minimal renderer, C Runtime app + runtime asset manager, D Exporter + Build.py redist + export method.

---

## M8 — Renderer I (PBR, IBL, post)

**Scope.** §8.3 passes 1, 4, 7–12, 14; §8.5, §8.6, §8.9, §8.10 (debug draw, text).

**Deliverables**: forward PBR with material binding sets (push constants in the set-0 layout), light list and culling (`RENDER_LIGHT_LIMIT_EXCEEDED`), `EnvironmentBaker` + `EnvironmentImporter` (cooked bakes reused under `--renderer none`, `Unsupported` otherwise) + DFG LUT + SH9, built-in environments baked into the engine cache and added to `Engine.pak`, deterministic blue noise (`Renderer/BlueNoise.cpp`), skybox, tonemappers/exposure/dither, bloom (with the `RGBA16_FLOAT` storage fallback), FXAA, transparency, `DebugRenderer`, `TextRenderer`, debug-view specialization constants (Lit, Albedo, Normals, Roughness, Metallic, Emissive), CPU reference implementations in `Tests/Source/Support/RenderReference.*`, golden scenes in `Projects/FeatureTest/Assets/Scenes/Golden/`.

**Acceptance**: GPU oracle tests (furnace for BRDF, prefilter and SH; DFG vs CPU; prefilter vs CPU 16²; seam continuity; tonemapper curves at 1,024 points); goldens `MaterialGrid`, `IblOnly`, `Tonemappers`, `Bloom`, `AlphaModes`, `DebugDraw`, `Text`, `GltfFixture`; `"Pipelines: count matches the expected total"`; `"GpuResourceCache: live counts return to baseline after unloading a scene"`; `"HotReload: texture change re-uploads"`; `"BlueNoise: output hash matches the committed value"`; `"Shaders: RW texture image formats match the C++ formats"`; Python `test_environment_import_and_screenshot`, `test_renderer_none_environment_import_uses_cached_bake`, `test_export_with_renderer_none_uses_prebaked_engine_cache`.

**Parallelization**: A PBR forward + materials + lights + the DFG LUT (it depends only on the BRDF and is generated at device startup, not at import; ADR 0013 decision 7), B IBL baker + importer + skybox, C post (tonemap, bloom, FXAA, dither), D debug draw + text, E CPU references + golden scenes.

---

## M9 — Renderer II (shadows, GTAO, editor rendering)

**Scope.** §8.3 passes 2, 3, 5, 6, 13; §8.7, §8.8, §8.10 (picking, outline), §8.13 annotations.

**Deliverables**: CSM + PCSS + stabilization + cascade blending (generic frustum-corner fit for both projections), spot shadow atlas (`RENDER_SPOT_SHADOW_BUDGET`), projection-aware depth pyramid + GTAO (orthographic radius and view vector) + denoise + AO application, `EntityId` picking with async readback, CPU raycast (`scene.raycast`, `viewport.pick`), selection outline, grid, procedural icons, remaining debug views, per-pass `RenderStats` + `stats.get`, screenshot annotations, the snapshot's `PickTable` and `RenderViewFlags` (`Colliders` appends the collider visualization through `BuildColliderDebugDraw`, at the snapshot's alpha through `ColliderDebugDrawOptions::Alpha` so a running play view's wireframes sit on their interpolated meshes, and `AppendColliderDebugDraw`, `Docs/Decisions/0016-m8-m11-m12-integration.md` decision 3).

**Acceptance**: `"Shadows: cascade splits"`, `"Shadows: sub-texel camera move leaves the light matrix unchanged"`, `"Shadows: orthographic frustum corners fit"`, `"Projection: orthographic reverse-Z mapping and view-ray reconstruction"`, `"GTAO: screen radius for perspective and orthographic"` (CPU); GPU shimmer test; goldens `ShadowsNear`, `ShadowsFar`, `ShadowsOrtho`, `SpotShadows`, `GtaoOn`, `GtaoOff`, `GtaoOrtho`, `SelectionOutline`, `AnnotatedScreenshot`; `"Picking: returns the UUID at known pixels"`; `"Raycast: CPU ray hits expected triangle"`; Python `test_stats_report_pass_timings`, `test_viewport_pick_deterministic`. 1080p frame time logged (warning only).

**Parallelization**: A CSM/PCSS (both projections), B spot atlas, C depth pyramid + GTAO (both projections), D picking/outline/grid/icons, E CPU raycast + viewport methods + annotations + stats.

---

## M10 — Editor UI

**Scope.** §12.2, §12.4, autosave/recovery (§4.13).

**Deliverables**: `Editor/Source/Editor/` (`EditorApp`, `EditorLayer`, dockspace layout, every panel in §12.2 incl. the "Allow AI automation" preference and the `SceneChangedOnDisk` banner, `EditorCamera`, `GizmoController` (logic, separate from ImGuizmo calls), inspector drawers (incl. `Map` and `Variant`), thumbnails, `ProjectLauncher`, `Icons.cpp` (procedural vector icons), Basic3D template), `EditorCore/Autosave` (CPU only), methods `viewport.camera|frame|setOptions`, `editor.state`, `project.open {recover}`.

**Acceptance**: `"GizmoController: a drag produces one merged undo step"`; `"CommandHistory: UI and agent commands interleave in one history"`; golden `EditorDefaultLayout` (`editor.screenshot`, headless null platform); Python `test_autosave_and_recovery_after_kill`, `test_basic3d_template_validates_clean`, `test_thumbnails_generated_headless`, `test_attach_to_windowed_editor_with_automation_allowed` (headless null-platform editor with the preference set); windowed manual smoke checklist executed by the developer is *not* a gate (all gates are automated).

**Parallelization**: A dockspace + hierarchy + inspector, B viewport + camera + gizmo, C content browser + thumbnails + launcher + templates, D console/diagnostics/settings/automation/undo panels, E autosave/recovery + methods.

---

## M11 — Physics

**Scope.** §9.

**Deliverables**: `Physics/` (`PhysicsEngine` (process level, from `ProcessContext`), `PhysicsWorld`, `PhysicsLayers` filters (Sensor–Static and Sensor–Sensor excluded), `Shapes` (only through `ShapeSettings::Create`; `IsValidScale`; compound sub-shape user data → collider entity), `ContactBuffer`, `Queries` (collider and body entities in hits, body and collider bounds), `CharacterController` (inner body + `CharacterContactListener`)), `Scene/PhysicsSystem` (composition rules incl. shared static compounds and implicit sensor bodies, `EnhancedInternalEdgeRemoval`, pre/post step, canonical write-back, sorted event dispatch interface consumed by scripting in M13, synthesized exits at destroy/disable flushes), `Scene/PhysicsComposition` (the composition rules, one function shared by the session, the validator and the visualization), `Scene/PhysicsValidation`, collider debug draw (edit and play; `Scene/ColliderDebugDraw`), Simulate mode, `physics.bodyInfo` (`Automation/Methods/PhysicsMethods`), `PHYSICS_*` validation codes incl. `PHYSICS_ADJACENT_STATIC_BODIES`, `PHYSICS_DYNAMIC_TRIGGER`, `PHYSICS_ALL_DOFS_LOCKED`, `PHYSICS_INVALID_SHAPE`, `PHYSICS_LIMIT_EXCEEDED`, the shared test support `Tests/Source/Support/PhysicsTestScene`.

**Acceptance**: every test listed in §9.7 (`"Physics: state hash identical with 0, 1 and 8 Jolt threads"`, `"Physics: 200-body pile hash equals the committed constant"` (identical in the Debug and Release unit runs), `"Physics: contact event order identical across runs"`, `"Physics: trigger detects a sleeping body"`, `"Physics: trigger detects a character through its inner body"`, `"Physics: CCD prevents tunnelling at 200 m/s"`, `"Physics: kinematic platform carries a resting box"`, `"Physics: destroy inside a contact callback is safe"`, `"Physics: sphere rolling across 20 boxes of one compound keeps |vy| < 0.05"`, `"Physics: compound contact names the child collider"`, `"Physics: destroying or disabling a partner synthesizes exits"`, `"Physics: Transform write in OnFixedUpdate teleports the body in the same tick"`, `"Physics: invalid shapes, all-DOF-locked and Dynamic-trigger bodies give diagnostics, never asserts"`, …); Python `test_simulate_mode_ball_falls`, `test_body_info_reports_contacts`, `test_validate_reports_adjacent_static_bodies_and_fix_adds_parent_body`.

**Parallelization**: A engine/world/shapes/layers/filters, B PhysicsSystem composition + sync + events + synthesized exits, C queries + bounds + character controller with inner body, D debug draw + methods + validation.

---

## M12 — Audio

**Scope.** §10.

**Deliverables**: `Audio/` (`AudioEngine`: `ma_engine` always device-less, an engine-owned `ma_device` whose callback reads the engine, device re-creation on notifications, simulation-time pulls while lockstep or a test run owns time, no-threading resource manager in test and headless modes, capture buffer for `Test.CaptureAudio`; `AudioVfs`, `AudioTypes`, `AudioDecoder`, voices, groups, `SoundSynth`), `Asset/AudioClipData`, `AssetPipeline/AudioImporter`, `SoundEffectImporter`, built-in `.sfx` presets, `Scene/AudioSystem` (`AUDIO_MULTIPLE_PRIMARY_LISTENERS`; `PlayOneShot` defaults), asset-browser preview hook (`EditorCore/Audio/AudioPreview`), `audio.stats`, `asset.create {type: SoundEffect}`, the format fixtures' generator `Tests/Data/Generate/MakeAudioFixtures.py`.

**Acceptance**: §10.4 tests; `"AudioEngine: a stopped Null-backend device is re-created and the voice cursor is unchanged"`; `"AudioEngine: three failed re-creations leave the engine device-less with one warning"`; `"AudioEngine: lockstep pulls exactly 800 frames per tick and the device reads nothing"`; `"AudioEngine: streamed playback captured twice is bit-identical"`; `"SoundSynth: presets hash to committed values"`; Python `test_sound_effect_create_and_play_in_lockstep` (voice visible in `audio.stats`).

**Parallelization**: A engine + VFS + device ownership + time ownership, B synthesizer + importers + presets, C AudioSystem + components + methods.

---

## M13 — Scripting

**Scope.** §11 (incl. §11.10 test scripts), plus `script.*`, `play.waitFor`, `input.record|replay`, `test.run`.

**Deliverables**: `Scripting/` (`ScriptEngine`, `TrackingAllocator` (soft limit + headroom, second-breach stop), `Sandbox` (DetMath-rebound `math`), `LoadTimeVm` (pure standard libraries, `Script`/`Field`/`Color`/`Quat`/`Math`/`Test.Suite`, load-time `require` with a shared budget), `RequireResolver`, `Watchdog` (nested callbacks inherit the deadline), `ScriptApiRegistry` (`Module(...).Function`, `Type(...).Method/Property/Operator/Constructor`, string-enum value counters, `RunModes`), `LuaHelpers` (finite checks, `ProtectedCall`, `ProtectedResume`), `Bindings/{Script,Entity,Components,Scene,Input,Time,Physics,Audio,Assets,Task,Random,Math,Quat,Color,Debug,Log,Application,Test}.cpp`, `TaskScheduler` (yield rule), `ScriptError`), `Testing/FeatureTestRunner` core (suite collection, sequential case threads, timeouts, isolation, quit handling, filter, result schema; §11.10), `Session` replay recorder/player (tick-0 recording, capture of all applied input, header with scene handle, parameters, version and final hash), `AssetPipeline/ScriptImporter` (kinds, require edges in the dependency graph, `IScriptDiagnosticsProvider`), `AssetPipeline/ReplayImporter` (precompiled `Expect` bytecode), script field drawer, hot reload (module patching with dependents, chain rollback, deferral in deterministic sessions), `EditorCore/Scripting/ScriptTypeChecker.cpp`, `Engine.d.luau` generator, script templates (Behaviour, Module, Test) with type-checked fixtures, `SCRIPT_*` (incl. `SCRIPT_NOT_A_BEHAVIOUR`), `TEST_SUITE_INVALID` and `INPUT_UNKNOWN_ACTION` codes, skills `add-script-api`, `luau-gameplay` (class pattern, cast idiom, yield rule, load-time VM, follow-camera idiom); from M12 (ADR 0015 decisions 5 and 12): the FeatureTest runner sets `PlaySessionSpecification::OwnsAudioTime` for every suite, a decision on deterministic decoding of test runs in windowed editors, and `Audio.SetGroupVolume`/`GetGroupVolume` bound through `PlaySession::GetAudioSystem`.

**Acceptance**
- `"ScriptApi: every registered function, method, property, operator and constructor has a test"` (registry vs test registry); `"Callbacks: order and phases match §5.7"`; `"Sandbox: escape suite"`; `"Watchdog: infinite loop raises timeout"`, `"Watchdog: no error raised from a GC interrupt"`, `"Watchdog: OnCreate inside Instantiate inherits the outer deadline"`; `"Allocator: soft limit raises a script error and the VM stays usable"`, `"Allocator: second breach stops the session with a structured error"`; `"Errors: location, traceback and instance disabling"` (callbacks and resumed threads); `"Tasks: Task.Wait in OnStart raises the documented error"`, `"Tasks: Task.Wait inside Task.Spawn resumes at the expected tick"`; `"LoadTimeVm: a multi-file require graph imports within one budget"`, `"LoadTimeVm: engine API at module top fails with a located message"`, `"ScriptImporter: Board.luau change re-extracts Game.luau"`; `"HotReload: instance state survives"`, `"HotReload: editing a required module updates dependents"`, `"HotReload: deferred while lockstep owns time"`; `"TestRunner: cases run sequentially as threads with per-case timeouts"`, `"TestRunner: scene restarts between suites"`, `"TestRunner: quit ends the suite and honours ExpectQuit"`, `"TestRunner: filter matches suite/case"`; `"Bindings: NaN and Inf are rejected with located errors"`; play-session fuzz (10,000 seeded calls, no crash, no assert, nothing invalid reaches Jolt); `"TypeChecker: fixtures report expected diagnostics"` (templates, every Field kind, cast idiom, test suite, seeded mistakes); `GenerateDocs.py --check` clean for `Engine.d.luau`.
- Python: `test_script_write_returns_diagnostics`, `test_wait_for_predicate`, `test_record_requires_tick_zero`, `test_record_captures_test_inject_and_parameters`, `test_record_and_verify_replay`, `test_replay_strict_hash`, `test_test_run_record_writes_replay`, `test_runaway_script_busy_watchdog`, `test_test_run_reports_failures_with_locations`, `test_entity_update_rejects_non_behaviour_script`.

**Parallelization**: contract task freezes `ScriptApiRegistry.h`, `LuaHelpers.h`, proxy interfaces, the test result schema and the replay header. A VM/sandbox/load-time VM/require/watchdog/allocator, B binding layer + Entity + component proxies + shortcut rule, C API modules (two engineers, split by binding file), D tasks + callbacks + errors + hot reload + importers, E test runner + replay recorder/player + methods, F type checker + d.luau + templates + fixtures. Owner of `RegisterBindings.cpp`: B.

---

## M14 — FeatureTest and documentation

**Scope.** §15.5–15.6, generated reference, complete skills.

**Deliverables**: `Testing/` coverage gates 1, 1b, 2–9 with per-mode evaluation and the cross-mode union merge in `Test.py` (§15.6), seeded-gap fixtures (`Tests/Data/CoverageGaps/`, one per gate and, for the per-mode gates, one per mode), validation fixtures for every code (`Tests/Data/Validation/`), `Projects/FeatureTest` (scenes per §15.5, incl. `Timing.scene` with a scripted clock, `Formats.scene` with `Assets/Formats/`, runtime-fault-only `Errors.scene` with suite overrides, `QuitApp.scene`, and `Determinism.replay` with its committed hash; produced by committed batch scaffolds `Projects/FeatureTest/Scaffold/*.jsonl`; suites under `Assets/Tests/` bound through `Testing.Suites`), format fixture generators (`Tests/Data/Generate/MakeFormatFixtures.py`, `Tests --write-format-fixtures`) and the approved FLAC/MP3/OTF fixtures, compile/type error fixtures (`Tests/Data/Scripts/Errors/`), the settings observation test over `AllSettings.eproj` (gate 8), `GenerateDocs.py` (Components, ScriptAPI, Automation, Validation, FileFormats, schemas, MCP catalogue), doc-example type checking with stub modules, skills `add-component`, `golden-images`, `game-building` (incl. the autopilot workflow and replay re-recording), `game-debugging` completed.

**Acceptance**: `Editor --headless --project Projects/FeatureTest --run-tests` exits 0 with every editor-mode gate green, in Debug and Release; the `Timing` suite passes with 0, 1, 2 and 5 steps per frame; `Determinism.replay` matches its committed hash in both editor configurations; each seeded-gap fixture makes exactly its gate (and mode) fail and names the gap; `GenerateDocs.py --check` clean; every Luau example in the docs type-checks; the automation-method gate is green.

**Parallelization**: A gates + per-mode evaluation + merge + gap fixtures, B FeatureTest scenes/suites for rendering, physics, audio, formats, C scenes/suites for scripting API, callbacks, prefabs, input, timing, errors, D docs generation + skills + validation fixtures + settings test.

---

## M15 — Export, hardening: engine + editor complete

**Scope.** §14 complete, the three-mode FeatureTest and the `determinism` stage (§15.8), T6 hardening, the completion gate.

**Deliverables**: exporter validation complete (incl. `--renderer none` with cooked bakes and `TEST_SUITE_INVALID`), notices generation, `Platform/Windows/ResourcePatcher` (icon group + `VS_VERSIONINFO` from `Export.Icon/Version/Company`), macOS bundle (rpath, `Info.plist`, `.icns`) and Linux RPATH code paths (generation-checked), Dist map-file check, **testing exports** (`project.export {testing: true}`, `--testing`: `"Testing": true` manifest, `Assets/Tests/**` always included, replays cooked with precompiled expectations, `Testing` settings in the TOC), the Dist-resident replay player and test-mode semantics (`Debug.Break` counted no-op, `Application.Quit` ends the suite, EditorOnly members excluded), the `export` and `determinism` CI stages, MSVC ASan option and stage, long fuzz stage (incl. the play-session binding fuzz), 10k-frame soak per FeatureTest scene, portability stage with clang-cl when available, perf capture to `bin/TestResults/Perf.json`.

**Acceptance**: the testing export of FeatureTest passes `--feature-test` in **Release and Dist**, with every gate green **per mode** (§15.6), and the merged union over editor, Release and Dist covers every registered entry; each per-mode seeded-gap fixture fails as expected; the `determinism` stage shows identical final state hashes for `Determinism.replay` and every FeatureTest replay across Editor Debug, Editor Release, exported Release and exported Dist, equal to the committed values; `"Export: resources carry the project icon, version and company"`; `"Export: user data goes to the manifest Name folder"`; `"Dist: map file contains no automation, Analysis, ImGui or AssetPipeline symbols"`; ASan run of unit + feature suites clean; soak memory growth < 1%; fuzz stage (1M mutations per reader) clean; full `CI.py` green including `hardening`; a completion review checks every section of `Docs/Architecture.md` against the code and records any deviation as an ADR.

**Parallelization**: A exporter + packaging + resources + notices, B testing exports + Dist replay player + three-way FeatureTest + determinism stage, C hardening stages (ASan, fuzz, soak), D portability + perf capture.

---

## M16 — Breakout dry run (time-boxed)

**Scope.** Exercise the agent workflow before the required demos. Time box: this milestone ends after at most 4 working sessions, whatever the polish of the game.

**Deliverables**: `Tests/Projects/Breakout` built by an agent through the MCP bridge only (paddle, ball with physics, brick grid from a prefab, score text, lives, synthesized sounds, win/lose); its committed `Automation/BuildLog.jsonl` and `Automation/Provenance.json`; one test suite and one replay recorded from tick 0; a friction list resolved as engine changes (methods, hints, docs, skills), each with tests. The dry run also exercises the operational rules once: attaching to an already-open editor, one `project.upgrade` after an engine change that alters canonical output, and one replay re-recording.

**Acceptance**: `AuditProvenance.py --project Tests/Projects/Breakout` passes from a fresh clone (nothing read from `Library/`); its suites and replay pass in `Test.py --suite games`, and the replay's final hash is identical in the editor and the Dist export; `project.validate` clean; Dist export smoke exits 0; every friction item is closed or recorded as an ADR.

**Parallelization**: one agent builds the game; engineers fix friction items in parallel in their owning modules.

---

## M17 — Tetris (built through automation)

**Scope.** §13.11. Built by an agent in a fresh `Projects/Tetris` using only MCP tools (scripts through `script.write`).

**Features**: 10×20 visible well (22 rows), 7 tetrominoes with SRS rotation and wall kicks, 7-bag randomizer, gravity by level, soft drop, hard drop, lock delay, DAS/ARR, ghost piece, hold, next preview, line clears with scoring 100/300/500/800 × level, levels every 10 lines, game over and restart, HUD texts, synthesized sounds, PBR look with soft shadows, GTAO, bloom.

**Acceptance**
- `Testing.Suites` green: `Board.test.luau` (empty scene: kicks at both walls and floor, 1–4 line clears and scores, 7-bag yields each piece once per bag, top-out ends the game, hold swaps once per piece) and `Tetris.test.luau` (bound to `Main.scene`: integration cases driven by `Test.InjectAction`).
- `Opening.replay` and `LineClear.replay`, recorded from tick 0, verify with `strictHash`; replaying twice gives identical final state hashes (`input.replay` reports them); the hashes are identical in the editor and in a testing Dist export.
- `StartScene` and `Export.BuildScenes` set; `project.validate` clean; Dist export smoke (`--frames 300 --expect-no-errors`) exits 0; the GTAO, soft shadows and bloom look is checked on screenshots from the orthographic camera, attached to the milestone review.
- `AuditProvenance.py --project Projects/Tetris` passes from a fresh clone against the committed `Automation/Provenance.json` and `Automation/BuildLog.jsonl`.

**Parallelization**: the game is built by one agent. Engine fixes discovered during the build land as separate changes by module owners.

---

## M18 — Rolling Ball 3D (built through automation)

**Scope.** §13.11 (Rolling Ball paragraph). Built in a fresh `Projects/RollingBall` using only MCP tools.

**Features**: three levels of tracks, ramps, curves, narrow beams, gaps and moving platforms built from track prefabs, each level's track forming one static compound under a `Static` level-root body (no seam bumps); a physics ball (`EnhancedInternalEdgeRemoval`) steered by camera-relative torque with air control; follow camera on the interpolated `RenderPosition`; checkpoints; kill plane with respawn at the last checkpoint (`RigidBody:Teleport`); goal trigger; level timer carried across levels through `Scene.Load` parameters; HUD (time, level); synthesized roll, checkpoint, fall and win sounds; HDRI sky, PCSS sun, GTAO.

**Acceptance**
- `Assets/Tests/RollingBall.test.luau` green: respawn at the last checkpoint, moving platform carries the ball, timer persists across `Scene.Load`, goal loads the next level, and the **main-path support check** (samples every level's `PathNode` polyline every 0.25 m; a downward `Physics.Raycast` must hit the `Track` layer everywhere except inside `Gap` markers; uses `Physics.GetColliderBounds` to report the offending piece).
- `Level1.replay`, `Level2.replay` and `Level3.replay` are recorded by per-level autopilot suites through `test_run {record}` (Level 2 and 3 with the carried-timer `Parameters` in the header). Each reaches the goal under `input.replay {verify: true, strictHash: true}` in lockstep **without** the autopilot; repeated runs give identical state hashes, also in a testing Dist export.
- `project.validate` clean (no `PHYSICS_ADJACENT_STATIC_BODIES`); Dist export smoke exits 0; `AuditProvenance.py --project Projects/RollingBall` passes from a fresh clone against the committed provenance and transcript.

**Parallelization**: one agent per game; M17 and M18 may run concurrently with different agents once M16 is closed, because they touch disjoint project folders.
