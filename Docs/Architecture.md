# Architecture

Status: **authoritative**, v1.2, 2026-10-05. This document supersedes the four design proposals it was synthesized from. v1.1 resolves the principal-engineer review of v1.0; Appendix D indexes each finding and where it is resolved. v1.2 incorporates the accepted M0 deviations of `Docs/Decisions/0002-m0-deviations.md` (Appendix D, last paragraph). Where this document and `Docs/CodeStyle.md` overlap, CodeStyle governs formatting and naming; this document settles every topic CodeStyle marks **[Architecture]** (see Appendix A).

The display name "Yggdrasil" appears only in the premake workspace name, the `ENGINE_PRODUCT_NAME` define (editor window titles, and the user-data folder of the Editor and Tests) and documentation. Exported games use their manifest `Name` instead (§14.1). Code says `Engine`.

**Provenance.** Structure and scope discipline come from the simplicity proposal (highest mean judge score). The correctness core comes from the robustness proposal: UI-free `EditorCore`, canonical iteration order, sorted physics events, seeded runtime UUIDs, per-step input latching, serializer-based Play copy, and test infrastructure. The automation layer comes from the AI-control proposal. Renderer verification and quality details come from the rendering proposal. Every flaw and uncovered requirement the judges raised is resolved in the text; Appendix B indexes where.

---

## 1. Goals, non-goals, guiding principles

### 1.1 Goals
- **G1 Spec features.** glTF PBR meshes with materials and textures, gizmo placement, IBL from Poly Haven HDRIs, soft shadows, GTAO, HDR with tonemapping, Jolt physics authored as components and driven by scripts, miniaudio, Luau scripting with entity creation, destruction and prefab spawning, an editor, and export to a standalone runtime.
- **G2 Rock-solid.** Every subsystem is unit-testable without a window, GPU or audio device. Bugs assert, expected failures are values, nothing fails silently.
- **G3 Deterministic.** The same source built with the same toolchain on the same platform, given the same inputs and seed, produces the same state hash. This holds whatever the frame rate, the Jolt thread count and the build configuration (Debug, Release, Dist). Recorded replays and expected hashes are therefore valid in every configuration (§9.1, §15.8).
- **G4 Full AI control.** An agent builds, tests and exports a game through the editor automation API alone. The proof is Tetris and Rolling Ball 3D, built by an agent through automation with a committed transcript and a file-provenance audit.
- **G5 Verifiable completeness.** The FeatureTest project mechanically proves that every component, component field, enum value, project setting, asset format, script function, method, property, callback and automation method is exercised, in the editor and in exported Release and Dist builds. A gap fails CI.
- **G6 Honest platform status.** Windows x64 is built and tested on every change. Linux and macOS are compile-targeted and generation-checked locally. They are labelled verified only when CI on that OS is green.

### 1.2 Non-goals (v1)
Each can be revisited through an ADR in `Docs/Decisions/`.
- Graphics backends other than NVRHI-Vulkan. Ray tracing, bindless rendering, GPU-driven rendering, render-graph aliasing.
- TAA, histogram auto-exposure, clustered light culling, point-light shadows, colour grading LUTs, SSR, GI beyond IBL plus AO.
- GPU texture compression (no approved encoder), EXR import, OGG import (see Appendix C), mesh instancing.
- Skeletal and morph animation (glTF skins and animations are skipped with a warning), particles, terrain, networking, visual scripting, native gameplay modules, mobile, consoles, web.
- Live nested prefabs (nested instances are flattened when a prefab is created), deleting prefab-member entities inside an instance (deactivate or unpack instead).
- Multiple script components per entity (use child entities), a UI framework beyond `TextComponent`, ImGui multi-viewport OS windows, native file dialogs.
- Native Wayland (Linux runs X11, including XWayland sessions), cross-OS export (each host exports for itself), bit-exact cross-OS or cross-compiler determinism. Jolt itself is built cross-platform deterministic (§9.1), but the CRT `pow` behind Luau's general `^` operator is not guaranteed to match across C runtimes.
- Code signing and notarization.

### 1.3 Guiding principles
1. **Pure core, thin effectful shell.** Step scheduling, device scoring, cascade math, light packing, undo, pak layout, input latching and coverage evaluation are pure functions with table-driven tests. Device wrappers stay thin.
2. **One source of truth per concept.** The type registry drives the inspector, serialization, Luau proxies, automation schemas, validation, docs and coverage. The script API registry drives bindings, `Engine.d.luau`, docs and counters. The method registry drives dispatch, the MCP catalogue and coverage. Generated files are committed and staleness-checked by CI.
3. **One path per job.** One mutation path (Commands), one load path (cooked bytes through the same loader in editor and runtime), one scene-copy path (the serializer), one render path (headless equals windowed).
4. **Deterministic by construction.** Fixed step, seeded RNG and runtime UUIDs, canonical iteration order wherever the order is observable, sorted physics events, simulation time only, precise floating point everywhere, and deterministic transcendental functions on the simulation path (§4.12).
5. **Errors are values at boundaries.** Programmer errors assert. Expected failures return `Result<T>`. Script faults are isolated. Fatal environment failures exit with a documented code.
6. **Parity.** Anything a human can do in the editor, an agent can do through automation on the same code path. No editor feature lands without its automation method and test.
7. **Bounded and located.** Every automation response is size-bounded. Every error carries a code, a location and, where possible, a hint.
8. **Pays for itself.** A new abstraction needs a written justification in its PR. The non-goals list is a scope guard.

---

## 2. Repository layout and build system

### 2.1 Layout
```
D:/Engine
├─ premake5.lua  Dependencies.lua  AGENTS.md  CLAUDE.md (@AGENTS.md)  .mcp.json (generated by Setup.py, gitignored)
├─ .clang-format  .clang-tidy  .editorconfig  .gitattributes  .gitignore
├─ .claude/skills/<name>/SKILL.md          (§13.10)
├─ .github/workflows/ci.yml                (calls Scripts/CI.py; optional until a remote exists)
├─ Engine/    premake5.lua
│             Config/entt/ext/config.h      (routes ENTT_ASSERT to ENGINE_CORE_ASSERT)
│             Source/EnginePCH.h|.cpp  Source/Engine/<Module>/...  (include root: Engine/Source)
├─ Editor/    premake5.lua (projects EditorCore = StaticLib, Editor = ConsoleApp)
│             Source/EditorPCH.h  Source/EditorCore/...  Source/Editor/...  (include root: Editor/Source)
├─ Runtime/   premake5.lua  Source/Runtime/{RuntimeMain.cpp, RuntimeApp.h|.cpp}
├─ Tests/     premake5.lua  Source/TestsPCH.h  Source/TestMain.cpp  Source/Support/
│             Source/Engine/<Module>/*Tests.cpp  Source/EditorCore/*Tests.cpp
│             Source/ThirdParty/*Tests.cpp  (configuration of vendored libraries, e.g. Jolt's version ID)
│             Data/ (fixtures + LICENSES.md)  Golden/<DeviceClass>/*.png
│             Automation/test_*.py  Projects/Breakout/ (dry-run project, §18)
├─ Resources/ Shaders/{Shared/*.h, Common/*.slang, Passes/*.slang, IBL/*.slang, Shaders.json}
│             Scripting/Engine.d.luau (generated)  Templates/{Projects,Scripts}/
│             Environments/*.hdr (2 x CC0 Poly Haven, 1k)  Fonts/Inter-Regular.ttf (SIL OFL)
│             EngineAssets.json  LICENSES.md   (editor icons and blue noise are generated in code, §8.4, §12.2)
├─ Vendor/<Lib>/ (as vendored; premake5.lua + VENDOR.md each)   Vendor/premake/bin/premake5.exe
├─ Scripts/   *.py entry points  Lib/ (shared helpers)  Premake/CompileCommands.lua  ModuleRules.json
├─ Tools/     MCP/{run.py, engine_mcp/, catalog.json (generated), requirements.lock, tests/}
│             Automation/engine_client.py (stdlib JSON-RPC client shared by MCP and tests)
├─ Projects/  FeatureTest/  Tetris/  RollingBall/
└─ Docs/      Architecture.md  Roadmap.md  CodeStyle.md  ReviewChecklist.md  Decisions/
              Reference/ (generated: Components, ScriptAPI, Automation, Validation, FileFormats)
```
A game project is `Projects/<Name>/<Name>.eproj`, `Assets/**` (each asset with a `.meta` sidecar), `AGENTS.md` (game notes), `Automation/` (committed: `BuildLog.jsonl`, the MCP transcript, and `Provenance.json`, §13.12) and `Library/` (cache, autosave, `Editor.lock`, offloaded automation results; gitignored).

### 2.2 Premake
Windows uses x64 host tools as well as an x64 target: the workspace sets `preferredtoolarchitecture "x86_64"`. `CheckBuildConfig.py` checks the generated project setting, with a seeded regression fixture (ADR 0019 decision 9).
`premake5.lua` declares `workspace "Yggdrasil"`, `configurations { "Debug", "Release", "Dist" }`, `architecture "x86_64"` on Windows and Linux and `"ARM64"` on macOS, `startproject "Editor"`, and `OutputDir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"`. Vendor projects are included under `group "Dependencies"`: `Vendor/GLFW`, `Vendor/NVRHI`, `Vendor/imgui`, `Vendor/ImGuizmo`, `Vendor/Luau` (projects `Luau`, `LuauAnalysis`), `Vendor/JoltPhysics`, `Vendor/miniaudio`, `Vendor/spdlog`. Header-only libraries (glm, EnTT, nlohmann/json, doctest, cgltf, stb) have no project; their implementation TUs live in Engine (`Asset/ThirdParty/StbImplementation.cpp`, `CgltfImplementation.cpp`, `ImGui/ImGuiGlfwImplementation.cpp`, all `NoPCH`). The exception is stb_vorbis, miniaudio's Ogg Vorbis decoder: it compiles inside the miniaudio project through the local translation unit `Vendor/miniaudio/miniaudio_vorbis.c` (the stb_vorbis declarations, `MINIAUDIO_IMPLEMENTATION` with `miniaudio.h`, then the stb_vorbis implementation; `Vendor/stb` is on that project's include path), which replaces upstream's `miniaudio.c` (ADR 0015 decision 2).

**Workspace-scope rule (ODR).** `filter "configurations:Dist" defines { "NDEBUG" }` is set at workspace scope and nowhere else. `NDEBUG` changes the layout of vulkan.hpp's dispatcher (`Vendor/NVRHI/VENDOR.md`), so it must be identical for NVRHI and every consumer. The vendored Luau project additionally defines `NDEBUG` for its own TUs in Release; this is allowed because no Luau layout depends on it and no Luau TU includes vulkan.hpp.

**`Dependencies.lua`** defines `IncludeDir`, `ApplyFirstPartySettings()` and one `Use<Lib>()` function per library. Each function's defines are copied verbatim from that library's `VENDOR.md`:

| Function | Consumer settings (must match the vendored project) |
|---|---|
| `UseNVRHI()` | `VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1`; Windows `VK_USE_PLATFORM_WIN32_KHR`, `NOMINMAX`; no other `VULKAN_HPP_*`; never link `vulkan-1` |
| `UseJoltPhysics()` | exactly the table in `Vendor/JoltPhysics/VENDOR.md`: `JPH_OBJECT_STREAM`, `JPH_OBJECT_LAYER_BITS=16`, **`JPH_CROSS_PLATFORM_DETERMINISTIC` in every configuration**, SSE4.1/4.2 on x86_64 (`/arch:SSE4.2` for MSVC, `-msse4.2 -mpopcnt` for GCC, Clang and clang-cl), `JPH_DEBUG_RENDERER JPH_PROFILE_ENABLED JPH_ENABLE_ASSERTS` in Debug and Release, `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED` in MSVC Debug, `JPH_NO_DEBUG` in Release and Dist |
| `UseSpdlog()` | `SPDLOG_COMPILED_LIB`, `SPDLOG_USE_STD_FORMAT`, MSVC `/utf-8 /Zc:__cplusplus` |
| `UseMiniaudio()` | `MA_NO_ENCODING`; macOS `MA_NO_RUNTIME_LINKING` + CoreAudio frameworks |
| `UseLuau(analysis)` | include dirs only; `links { "LuauAnalysis", "Luau" }` when `analysis`, else `{ "Luau" }` |
| `UseGLFW()` | `GLFW_INCLUDE_NONE`; platform link libraries from `Vendor/GLFW/VENDOR.md` |
| `UseImGui()` | none (unmodified `imconfig.h`); ImGuizmo include `Vendor/ImGuizmo/src` |
| first-party only | `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_ENABLE_EXPERIMENTAL`, `GLM_FORCE_CTOR_INIT`, `JSON_USE_IMPLICIT_CONVERSIONS=0`, `WIN32_LEAN_AND_MEAN`, `ENGINE_PLATFORM_{WINDOWS,LINUX,MACOS}` |

All vendor include directories use `externalincludedirs` with `externalwarnings "Off"`. `Scripts/CheckBuildConfig.py` generates the workspace into a temp directory for every generator and toolset the build or CI uses (vs2026 with MSVC and with clang-cl, gmake with GCC and with Clang, xcode4), parses every generated project and computes each compile unit's effective defines in command-line order (undefines and `-D`/`/D` in compiler options count). It fails if, for any target and configuration, an ABI-relevant define differs between a vendor project and a consumer that includes its headers. The families come from each `VENDOR.md`: `JPH_*`; `SPDLOG_*`; `MA_NO_*`, `MINIAUDIO_IMPLEMENTATION`; `VULKAN_HPP_*`, `VK_USE_PLATFORM_*`, `NOMINMAX`, `VK_ENABLE_BETA_EXTENSIONS`, `NVRHI_SHARED_LIBRARY_*`; Luau's `LUA_USE_LONGJMP`, `LUA_API`, `LUACODE_API`, `LUA_VECTOR_*`; Dear ImGui's and ImGuizmo's `IMGUI_*`, `ImTextureID`, `ImDrawIdx`, `USE_IMGUI_API`, `IMGUIZMO_NAMESPACE`; GLFW's `_GLFW_*`, `GLFW_DLL`; and `NDEBUG` for every library. It also fails if `JPH_CROSS_PLATFORM_DETERMINISTIC` is missing from JoltPhysics or any consumer; if a Jolt consumer uses other instruction-set options than JoltPhysics (Jolt derives `JPH_USE_AVX2` and the like from the compiler's `__AVX2__` macros, which `JPH_VERSION_ID` does not encode); or if any first-party or Jolt project uses a non-precise floating-point model (`/fp:fast`, `FloatingPointModel=Fast`, `/fp:contract`, `/Qfast_transcendentals`, `-ffast-math` or any of its components, or any effective `-ffp-contract` other than `off`). At runtime `JPH::VerifyJoltVersionID()` is checked at startup and a mismatch is fatal. The version ID encodes `JPH_CROSS_PLATFORM_DETERMINISTIC`, so a consumer built without the define cannot run.

**Floating-point model (determinism, §9.1).** Every first-party project and JoltPhysics compile with precise floating point. On MSVC this is `/fp:precise` (premake `floatingpoint "Default"`), which since VS 2022 never generates FMA contractions. GCC and Clang use `-fno-fast-math -ffp-contract=off` on every architecture, and clang-cl the same options as `/clang:-fno-fast-math /clang:-ffp-contract=off`. Clang's default model is already precise, so this is precise without contraction; `-ffp-model=precise -ffp-contract=off` is not used, because `-ffp-model=precise` implies `-ffp-contract=on` and Clang reports the override as `-Woverriding-option`, an error under `-Werror` (for the same reason premake's `floatingpoint "Precise"` is not used). `-fno-fast-math` also resets the `-ffast-math` that premake's xcode4 generator adds for `optimize "Full"` (Dist). The vendored Jolt premake file therefore replaces upstream's MSVC `floatingpoint "Fast"` and its ARM64 `-ffp-contract=on`. That is a build-configuration change recorded in `Vendor/JoltPhysics/VENDOR.md`; Jolt's source stays unmodified (Roadmap M0). Jolt documents the cost of cross-platform determinism at roughly 8 %, which is accepted. In exchange, replays and expected hashes are valid in every configuration.

**`ApplyFirstPartySettings()`** (Engine, EditorCore, Editor, Runtime, Tests): `language "C++"`, `cppdialect "C++23"`, `staticruntime "off"`, `warnings "Extra"`, `fatalwarnings { "All" }`, `multiprocessorcompile "On"`, exceptions on, RTTI at compiler default (first-party code never uses `dynamic_cast` or `typeid`; lint-enforced). MSVC adds `/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor` and the precise FP model above. GCC/Clang add `-Wshadow` and the precise FP flags above. Windows Debug executables link with `/STACK:2097152` (Luau analysis recursion). Each Windows executable embeds `Engine/Source/Engine/Platform/Windows/App.manifest` (PerMonitorV2 DPI, UTF-8 active code page, long paths).

| | Debug | Release | Dist |
|---|---|---|---|
| Defines | `ENGINE_DEBUG` | `ENGINE_RELEASE` | `ENGINE_DIST`, `NDEBUG` (workspace) |
| Optimize / symbols | Off / On | Speed / On | Full + `linktimeoptimization "On"` (xcode4: `LLVM_LTO = YES`, because premake ignores `linktimeoptimization` there) / On (PDB/dSYM archived in `bin/Symbols`, never shipped) |
| Engine asserts, Trace logs | on | on | off |
| Vulkan + NVRHI validation | on by default | `--gpu-validation` (always on in tests) | never |
| Projects built | all | all | Engine, Runtime, vendor (`removeconfigurations { "Dist" }` on EditorCore, Editor, Tests) |
| Runtime kind | ConsoleApp | ConsoleApp | WindowedApp (`entrypoint "mainCRTStartup"`) |

PCH: `EnginePCH.h` (Engine), `EditorPCH.h` (compiled separately by EditorCore and Editor), `TestsPCH.h`. Runtime uses none. Headers never include a PCH.

**Shaders** are compiled by a dedicated `Shaders` project (`kind "Utility"`, group `Engine`) into `bin/<OutputDir>/Shaders/` (§8.12). Its single custom build rule runs `python Scripts/CompileShaders.py --config %{cfg.buildcfg}` on `Resources/Shaders/Shaders.json`. The rule declares `buildinputs` for every `Resources/Shaders/**.slang` and `**.h` file and `buildoutputs` for a stamp file `bin/<OutputDir>/Shaders/.stamp`, so Visual Studio's fast up-to-date check re-runs it when only a shader changes. `Engine` declares `dependson { "Shaders" }`. The ninja generator is the exception: premake 5.0.0's ninja generator supports neither Utility projects nor `dependson` edges to them, so for ninja `Engine/premake5.lua` attaches the same rule to `Engine` itself and every Engine object depends on the stamp. A prebuild step on the Engine static library is not used, because the IDE skips it when no C++ input changed. Dev builds mount `engine://` at `<repo>/Resources` plus `engine://Shaders` at that directory, through the `ENGINE_REPO_ROOT` define, which is absent in Dist. Exported games read `Engine.pak`, which contains the target configuration's SPIR-V (§14.2).

**`Scripts/Premake/CompileCommands.lua`** adds a custom action, `premake5 compile-commands`, that writes `compile_commands.json` (clang-style flags derived from the same project settings) for clang-tidy.

### 2.3 Python scripts (Python 3.10+, stdlib only; `Tools/MCP` uses a venv created by Setup.py)
Every script has `--help`, `--json` output and a non-zero exit code on failure.

| Script | Responsibility |
|---|---|
| `Setup.py` | Check Python, Git, `VULKAN_SDK` (≥ 1.4.350, `slangc`, `spirv-val`), MSVC via vswhere, Linux packages (`xorg-dev`). Download the pinned premake 5.0.0 into `Vendor/premake/bin/` (SHA-256 verified; the directory is gitignored) when absent. Create `Tools/MCP/.venv` from the hash-locked `requirements.lock`, then write `.mcp.json` with the venv interpreter's absolute path (§13.8), so no `python` on `PATH` is assumed. |
| `Generate.py [--action]` | `premake5 vs2026` (Windows), `gmake`/`ninja` (Linux), `xcode4` (macOS); then `compile-commands`. |
| `Build.py --config C [--project P]` | MSBuild via vswhere, or make/ninja/xcodebuild. After building Runtime on Windows, copies the app-local CRT (`vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll` from `VC/Redist/MSVC/<newest>/x64/Microsoft.VC145.CRT`) into `bin/<OutputDir>/Runtime/Redist/`. |
| `CompileShaders.py [--program P] [--force]` | The only place slangc flags are defined (§8.12). |
| `Test.py --suite unit,gpu,golden,feature,automation,determinism,games [--config C] [--require-gpu] [--vulkan-api 1.3\|1.4] [--update-golden] [--junit] [--allow-skips]` | Runs the suites (§15) in the configurations given by the §15.8 matrix. The gpu suite runs once per Vulkan API cap (1.4, then capped with `--vulkan-api=1.3`; `--vulkan-api` restricts it to one). JUnit XML into `bin/TestResults/<Suite>-<Config>[-Vulkan13\|-Vulkan14].xml`. Without `--require-gpu`, a GPU or golden test case that finds no Vulkan device passes without running and the run ends with the status warning; with it, the test case fails. The automation suite (`Tests/Automation` and `Tools/MCP/tests` in the MCP virtual environment, then the method coverage gate) does the same for its tests that start a rendering editor (`harness.require_gpu`, ADR 0009 decision 33). A gpu run under the 1.4 cap also warns about skipped host-image-copy cases, and a golden run about missing goldens (smoke mode) and about candidates `--update-golden` wrote. Every run fails on, and names, any test case skipped outside the child-process targets (`Test::ChildTargetSuite`), from the Tests binary's `--no-skip --list-test-cases --reporters=xml` listing of the run's selection; `--allow-skips` is contract mode (Roadmap rule 3, `Docs/Decisions/0004-contract-stub-gate.md`). |
| `Format.py [--check]` | clang-format 22.x over first-party C++; skips `Vendor/`. |
| `Lint.py` | `CheckIncludes` step (module rules and the explicit layer-5 DAG from `ModuleRules.json`, plus the NVRHI-free rule for `RenderSnapshot.h`/`DebugDrawList.h`, §3), banned-API scan (incl. `std::sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`/`exp`/`log`/`pow` in simulation-path modules, §4.12), clang-tidy naming checks, header self-containment (each first-party header compiled alone), Python syntax via `ast.parse` (also against the Python 3.10 grammar; not `compileall`, which writes `.pyc` files into the tree and checks only the running interpreter's grammar) + AST style checks, JSON validity of all project files, and the `contract` step: `ENGINE_CONTRACT_STUB` anywhere but its definition in `Core/Base.h` (`contract-stub`) and `doctest::skip` outside a decorator expression that carries `doctest::test_suite(Test::ChildTargetSuite)` (`test-skip`); `--allow-contract-stubs` is contract mode (Roadmap rule 3). `--mode` selects the checkers that need clang tools (clang-tidy naming, clang-query json access): `auto` uses each tool that is installed and the regex fallback otherwise, `clang` requires them, `regex` forces the fallbacks. |
| `GenerateDocs.py [--check]` | Runs `Editor --headless --renderer none --dump-reference`; writes `Docs/Reference/*`, `Resources/Scripting/Engine.d.luau`, `Tools/MCP/catalog.json`. `--check` fails on any diff. |
| `CheckBuildConfig.py` | Vendor/consumer define consistency, the Jolt instruction set and the precise floating-point model, for every build target (§2.2). |
| `GenerateDetMathReference.py [--check] [--self-test]` | Writes `Tests/Source/Engine/Core/DetMathReferenceData.h`: seeded DetMath arguments and their correctly rounded results, computed with exact arithmetic and byte-for-byte reproducible (§4.12, `Docs/Decisions/0007-detmath-reference-oracle.md`). `--check` compares a fresh run with the committed file; `--self-test` checks the rounding routine and known values only. |
| `FetchAssets.py hdri <id> --res 1k|2k|4k --dest <dir>` | Poly Haven download restricted to `api.polyhaven.com` and `dl.polyhaven.org`, md5-verified. Also `font` for the pinned Inter release (sha256), and `fixture` for the pinned format fixtures that are not generated first-party: the OTF font and the public-domain MP3 and Ogg Vorbis files (§15.5; sha256, sources and licenses in `Tests/Data/LICENSES.md`; WAV and FLAC are written by `Tests/Data/Generate/MakeAudioFixtures.py`, ADR 0015 decision 25). Downloads need operator approval, which is collected up front in Roadmap M0. |
| `Export.py --project P --config Dist --out D [--testing]` | Wraps `Editor --headless --export`. |
| `AuditProvenance.py --project P` | Demo acceptance audit against the committed `Automation/Provenance.json` and transcript (§13.12). |
| `CI.py [--stages ...] [--configs ...] [--contract] [--gpu-optional]` | The single entry point (§15.8). Strict by default; `--contract` passes `--allow-contract-stubs` to `Lint.py` and `--allow-skips` to `Test.py`, exactly like `PreCommit.py --contract`; `--gpu-optional` runs the gpu, golden and automation stages without `--require-gpu` (hosted runners without a GPU). |
| `PreCommit.py [--contract] [--gpu-optional]` | Generate, the lint-stage static checks (`CheckBuildConfig.py`, `Lint.py`, format check), Debug build, unit, gpu, golden, feature and automation suites (the automation suite against the Debug editor, in the MCP virtual environment `Setup.py` creates; `--gpu-optional` on a machine without a Vulkan device). Required before every commit (§15.9). Strict by default; `--contract` (contract mode: `Lint.py --allow-contract-stubs`, `Test.py --allow-skips`) only for the commit of a milestone's contract task. |

---

## 3. Module architecture and dependency rules

All engine code is in `namespace Engine`. Sub-namespaces: `Utils`, `Detail` (CodeStyle), `Automation` (method handler free functions), `ScriptBindings` (Luau C functions), `Lua` (Scripting's checked helpers and protected VM entry points, §11.4/§11.7; ADR 0019) and `Test` (test support). EditorCore, Editor, Runtime and Tests code also lives in `namespace Engine`; editor classes carry an `Editor` prefix where a name would be ambiguous (`EditorContext`, `EditorCamera`).

| L | Module (`Engine/Source/Engine/…`) | Contents | May include | Third-party (public headers / .cpp) |
|---|---|---|---|---|
| 0 | `Core` | Base, Assert, Log, Result/Error, UUID, Hash (XXH64, FNV-1a), Random, DetMath, Time/Clock (System, Manual, Scripted), FixedStepScheduler, Json (Reader/Writer), VFS, FileSystem, BinaryReader/Writer, JobSystem, MainThreadQueue, EventLog, Handle, Profiler | — | glm, spdlog (Log.h), `nlohmann/json_fwd.hpp` / nlohmann |
| 1 | `Reflection` | TypeRegistry, TypeInfo, FieldInfo, EnumInfo, Value, JSON Schema generation, fuzzy name suggestion | Core | — |
| 1 | `Platform` | Window, Input, Process, CrashHandler, Paths, PollingFileWatcher, Socket; `Windows/`, `Linux/`, `MacOS/`, `Posix/` subfolders | Core | — / GLFW, OS headers |
| 1 | `Graphics` | GraphicsDevice, DeviceSelection, Swapchain, ShaderLibrary, PipelineFactory, RenderTargetPool, Readback, GpuProfiler, GpuResourceTracker | Core, Platform | NVRHI, Vulkan-Headers |
| 1 | `Physics` | PhysicsEngine, PhysicsWorld (ECS-agnostic), PhysicsTypes, PhysicsDiagnostics, layers, PhysicsShape, contact buffer, queries, CharacterController; it takes mesh geometry as vertex and index arrays and layers as names, since it cannot include `Asset` or `Project` | Core, Reflection | — / Jolt (pimpl, `Private/`) |
| 1 | `Audio` | AudioEngine (ECS-agnostic), voices, groups, AudioTypes (the shared vocabulary, `Attenuation` and `AudioGroup` included), AudioVfs (the VFS bridge), AudioDecoder (probing for the importer, decoding for the engine), SoundSynth | Core | — / miniaudio |
| 1 | `Automation/Protocol` | JSON-RPC framing, transport server, sessions, MethodRegistry, PendingOperation, watchdog | Core, Reflection, Platform | nlohmann |
| 2 | `Asset` | AssetHandle, AssetRef, AssetType, AssetMetadata, AssetRegistry, the `AssetManager` interface, `RuntimeAssetManager` (paks), cooked formats and loaders (CPU data: MeshData, TextureData, MaterialData, EnvironmentData, AudioClipData, ScriptData, FontData, ReplayData), built-ins, pak reader, `IEnvironmentBaker`, `IScriptDiagnosticsProvider` | Core, Reflection, Platform | — |
| 3 | `Renderer` | RenderSnapshot, DebugDrawList, SceneRenderer, SceneTargetFormats, RenderPrepare, PassBindingCache, BrdfLut, the passes (SkyboxPass, BloomPass, TonemapPass, FxaaPass, DebugRenderer, TextRenderer), GpuResourceCache, StaleMirrorSchedule, EnvironmentBaker, BlueNoise, TextLayout, camera math | Core, Platform, Graphics, Asset | NVRHI |
| 3 | `ImGui` | ImGuiRenderer (NVRHI backend), ImGuiLayer, GLFW glue, theme | Core, Platform, Graphics | imgui, ImGuizmo / GLFW (the GLFW glue `ImGui/ImGuiGlfwImplementation.cpp`, §2.2) |
| 3 | `Scene` | Scene, Entity, components, component registration, hierarchy, TransformSystem, SceneSerializer, Prefab, PhysicsSystem (with PhysicsComposition, PhysicsValidation and ColliderDebugDraw), AudioSystem (with its private `AudioSourceRuntime`), RenderExtraction | Core, Reflection, Platform, Asset, Physics, Audio, and only `Renderer/RenderSnapshot.h` + `Renderer/DebugDrawList.h` | EnTT |
| 4 | `Scripting` | ScriptEngine, sandbox, bindings, ScriptApiRegistry, TaskScheduler, ScriptError | ≤3 | — / Luau (VM, Compiler, Require) |
| 5 | `Session` | PlaySession (runtime scene + PhysicsWorld + ScriptEngine + voices), step order, lockstep, state hash, replay player | ≤4 | — |
| 5 | `Testing` | FeatureTestRunner (suites, cases, replays, scripted clock), coverage counters and gates, ImageCompare, JUnit writer, result schema | ≤4, `Session` | — / stb_image_write |
| 5 | `Automation/Methods` | Handlers shared by Editor and Runtime (play, observe, input, screenshot, physics, audio) | ≤4, `Session` | — |
| 5 | `AssetPipeline` | `EditorAssetManager` (source → cook → load), importers, cooker, pak writer, SoundEffect cooker, script compile, replay cooker, engine cooked cache, CRT/redist helpers | ≤4 | — / cgltf, stb, `imstb_truetype.h` |
| 5 | `App` | ProcessContext, Application, EngineContext, EntryPoint, FrameLoop | ≤4, `Session`, `Testing`, `Automation/Methods` (never `AssetPipeline`) | — / Vulkan-Headers (the frame-boundary `vk::SystemError` catch in `App/FrameLoop.cpp`, §4.6) |

| Project | May include |
|---|---|
| `EditorCore` (StaticLib, no ImGui) | all Engine modules except `ImGui`; LuauAnalysis, plus the Luau Ast and Common headers its API is written in (only `EditorCore/Scripting/ScriptTypeChecker.cpp`, which `EditorCore` registers as the `IScriptDiagnosticsProvider` used by `ScriptImporter`) |
| `Editor` (exe) | everything |
| `Runtime` (exe) | Engine except `AssetPipeline`; no `Editor*` headers; `ImGui` only under `#if !defined(ENGINE_DIST)` |
| `Tests` (exe) | everything |

`EditorCore` also holds `EngineAssetGenerators` (the editor's generators of the Generated built-ins, ADR 0013 decision 8) and `Audio/AudioPreview` (the asset browser's preview, ADR 0015 decision 17).

Rules, enforced by the `CheckIncludes` step of `Scripts/Lint.py` against `Scripts/ModuleRules.json`:
1. A module includes only lower layers and the same-layer exceptions listed above. Layer 5 is an explicit DAG, written into `ModuleRules.json` as allowed edges: `Session` → {`Testing`, `Automation/Methods`} → `App`, with `AssetPipeline` beside them, where X → Y means Y may include X. `AssetPipeline` includes no other layer-5 module and none of them includes it. Same-layer edges not listed are errors, so a cycle cannot form.
2. `Renderer` never includes `Scene`; it renders a plain `RenderSnapshot`. `Asset` holds CPU data only; the renderer owns GPU mirrors. Physics never includes `Asset`: Scene hands it a mesh's `MeshData` as vertex and index arrays (ADR 0014 decision 2), and it never touches the renderer.
3. Public headers never expose GLFW, Jolt, Luau, miniaudio, cgltf or stb types. NVRHI handles may appear in `Graphics`, `Renderer` and `ImGui` headers; EnTT only in `Scene` headers. In `Core`, spdlog appears only in `Log.h` among public headers (Core `.cpp` files and `Private/` headers may use it), so no other Core header leaks it into every module. `Renderer/RenderSnapshot.h` and `Renderer/DebugDrawList.h`, the only Renderer headers `Scene` may include, include nothing beyond `Core`, `Asset` and the standard library, and in particular no NVRHI. A dedicated lint rule checks their transitive includes.
4. Dependency inversion keeps layers intact. `Asset` defines `IAssetImporter`/`IAssetLoader` registries, the `AssetManager` interface, `IEnvironmentBaker` and `IScriptDiagnosticsProvider`. `AssetPipeline` registers importers and provides `EditorAssetManager` (the Runtime never calls that registration, so the linker drops importers). `Renderer` implements `IEnvironmentBaker`, and `EditorCore` registers the type checker as `IScriptDiagnosticsProvider`. `EditorApp` constructs an `EditorAssetManager` and `RuntimeApp` a `RuntimeAssetManager`, and each injects it into `EngineContext` as `AssetManager&`.
5. No mutable globals except **process-level state** (§4.1): the logger registry, the assert handler, the crash handler, the Luau process state (fast flags, assert bridge and monotonic VM identity counter, ADR 0019), Jolt's allocator hooks, `Factory::sInstance` and its `JobSystemThreadPool`, vulkan.hpp's default dispatcher, and GLFW's library state. `ProcessContext` initializes all of them once from `EntryPoint`, or from the Tests main.

---

## 4. Core

### 4.1 Application lifecycle
```cpp
struct ApplicationSpecification
{
	std::string Name;
	CommandLine Args;
	WindowMode Window = WindowMode::Windowed;       // Windowed | Headless (GLFW null platform)
	RendererMode Renderer = RendererMode::Vulkan;   // Vulkan | None (logic-only)
	GraphicsSpecification Graphics;                 // validation, GPU override, frames in flight (2), API cap
	FrameLoopConfig Loop;                           // FixedHz 60, MaxStepsPerFrame 5, MaxFrameDelta 0.25
	ClockKind Clock = ClockKind::System;            // System | Manual (headless/lockstep)
	std::optional<AudioEngineSpecification> Audio;  // device and decoding; nullopt: GetDefaultAudioSpecification(Window) (§10.1)
};

class Application
{
public:
	explicit Application(ApplicationSpecification specification);
	virtual ~Application();
	int Run();                                      // returns a process exit code (§4.1 table)
	void RequestExit(int exitCode = ExitCode::Success);
	EngineContext& GetContext();
protected:
	virtual Status OnInitialize() { return {}; }
	virtual void OnShutdown() {}
	virtual void OnEvent(Event& event) {}
	virtual void OnFixedStep(const SimStep& step) {}  // 0..N per frame
	virtual void OnUpdate(const FrameTime& frame) {}
	virtual void OnRender(RenderContext& context) {}
	virtual void OnImGuiRender() {}
};
```
There is no layer stack: `EditorApp` and `RuntimeApp` are the only subclasses. `EngineContext` owns every per-context service (`VirtualFileSystem`, `JobSystem`, `MainThreadQueue`, `Window`, `InputState`, `GraphicsDevice*` (null with `RendererMode::None`), `AssetManager&` (injected, §3 rule 4), `AudioEngine`, `TypeRegistry`, `ScriptApiRegistry`, `EventLog`). Engine code receives `EngineContext&` or the specific service explicitly; there is no `Application::Get()`.

**Initialization has two levels**, because GLFW, the Vulkan loader and Jolt's factory are process-global, and GLFW is main-thread-only with one platform per process:
1. **Process level** (`App/ProcessContext`, created once by `RunApplication` or the Tests main and destroyed last): Log → Profiler → CrashHandler → Luau fast flags → `PhysicsEngine::Initialize` (the `Physics` step, configured by `ProcessContextSpecification::Physics`: Jolt allocator, `Factory::sInstance`, `RegisterTypes`, version check, `JobSystemThreadPool` with `WorkerThreads` threads, by default `clamp(hardware concurrency − 2, 1, 4)`; §9.1, ADR 0014 decision 1) → if rendering, the Vulkan loader (`vk::detail::DynamicLoader`, `VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr)`, §8.1) and `glfwInitVulkanLoader(vkGetInstanceProcAddr)` → `glfwInitHint(GLFW_PLATFORM, …)`, with the platform chosen **once** (null for headless, native otherwise) → `glfwInit()`. A process is therefore either windowed or headless, never both.
2. **Per context** (`Application`/`EngineContext`): VFS → JobSystem → Window (`GLFW_NO_API`) → GraphicsDevice → AssetManager → AudioEngine → registries. The AssetManager is injected (`SetAssetManager`), not a step. The AudioEngine is the `Audio` step after Graphics and exists only when `EngineContextSpecification::Audio` is set: every `Application` sets it, from `ApplicationSpecification::Audio` or `GetDefaultAudioSpecification(WindowMode)` (windowed runs: the System device and threaded decoding; headless runs: no device and deterministic decoding), while in-process test contexts usually have none (`GetAudioEngine()` is null and play sessions are silent). It is the context's last member, so it is destroyed first, after its owners (play sessions, the editor's preview) released their voices. `Application` updates it after `OnUpdate` with the frame clock's accumulated unscaled time and unscaled delta, so the frame phase has set the voices before a device-less engine pulls the frame (ADR 0015 decisions 3 and 14).

Each step returns `Status`. On failure, the completed steps are torn down in reverse and `Run` returns `ExitCode::InitFailed`. Teardown is the reverse order and asserts that every live-object counter (GPU resources, Jolt bodies, audio voices, Lua states) is zero. Tests may build several `EngineContext`s side by side in one process, as long as they share that process's GLFW platform (headless or `RendererMode::None`). Windowed GPU tests (swapchain, present, minimize) run in a child process, `Tests --windowed-child=<name>`, spawned like a death test (§4.5).

`Engine/App/EntryPoint.h` exposes `int RunApplication(int argc, char** argv, ApplicationFactory factory)`. It creates the `ProcessContext`, catches `std::exception` as a last resort (→ fatal), and maps outcomes to **exit codes**, which CI, the MCP bridge and the exporter rely on:

| Code | Name | Meaning |
|---|---|---|
| 0 | `Success` | |
| 1 | `Failed` | tests, validation, `--expect-no-errors` or a replay expectation failed |
| 2 | `UsageError` | bad command line |
| 3 | `InitFailed` | no Vulkan loader or suitable device, project failed to load, project locked by another editor (§4.13), required file missing |
| 4 | `Crash` | fatal error (incl. device loss, GPU out-of-memory or hang, §8.14), unhandled exception, signal, assert in a non-debugger run |
| 5 | `Timeout` | `--timeout` or watchdog expiry |

### 4.2 Main loop: fixed-step simulation, variable rendering
```cpp
struct FrameSteps { uint32_t StepCount = 0; double Alpha = 0.0; double DroppedSeconds = 0.0; };
class FixedStepScheduler   // pure; exhaustive table tests
{
public:
	explicit FixedStepScheduler(const FrameLoopConfig& config);
	FrameSteps Advance(double realDeltaSeconds, double timeScale);
	uint64_t GetTick() const;
};
```
`Clock` is an interface. `SystemClock` (steady clock) is used for windowed runs. `ManualClock` advances exactly one `FixedDelta` per frame and is used for headless runs, lockstep play and most tests. With `ManualClock` every frame contains exactly one fixed step and `Alpha` is defined as 1, so screenshots show the exact simulated state. `ScriptedClock` replays a table of frame deltas (cycled). The FeatureTest `Timing` suite (§15.5) uses it to run zero-, one- and multi-step frames, clamping and dropped time end to end, deterministically. At most `MaxStepsPerFrame` (5) steps run per frame; time beyond that is dropped and reported in `stats.get`.

Headless play without lockstep, for example a human watching a headless editor over `viewport.screenshot`, throttles the loop to `FixedHz` frames per wall-clock second, so a `ManualClock` session never runs unthrottled. Lockstep, batch runs and test runs are the only unthrottled modes, and in them time advances only on request.

**Minimized windows** (0×0 framebuffer) skip extraction, rendering and present. The loop then waits in `glfwWaitEventsTimeout(FixedDelta)` instead of `glfwPollEvents`, so the CPU idles instead of spinning without GPU pacing. Simulation and audio keep running in real time. The editor keeps pumping automation, and lockstep is unaffected. Games that want to pause on minimize react to `WindowFocusEvent` through `Application.IsFocused()`.

Frame:
1. `Window::PollEvents()` (or `WaitEventsTimeout` when minimized) → events → `InputState` (real devices and injected events share this path).
2. `MainThreadQueue::Drain()`: job completions, asset swaps, file-watcher results, in submission order. Stale job completions are dropped (§7.5).
3. `AutomationServer::Pump(budget 4 ms)` (Editor; Runtime non-Dist with `--automation`): runs queued requests at a safe point and polls pending operations.
4. `steps = scheduler.Advance(clock.Delta(), timeScale)`. In lockstep, the steps are those requested by `play.step`, at most as many as fit the 50 ms per-frame budget (§13.6).
5. `OnFixedStep` × steps → `PlaySession::FixedStep` (§5.7).
6. `OnUpdate(frame)` → `PlaySession::FrameUpdate` (script `OnUpdate`/`OnLateUpdate`, §5.7), editor camera and UI. `frame.Alpha` is already known here, so `Transform.RenderPosition` and `Time.GetInterpolationAlpha()` return this frame's interpolated values (§5.2).
7. `OnRender`: snapshot extraction with interpolation `Alpha`, `SceneRenderer`, ImGui, present, `runGarbageCollection()`. `App/FrameLoop.cpp` wraps the whole frame in the single allowlisted frame-boundary `catch` for `vk::SystemError` (§4.6, §8.14).

### 4.3 Platform, window, input
- **Window** is one concrete GLFW class (no per-OS hierarchy), created with `GLFW_CLIENT_API = GLFW_NO_API`. GLFW itself is initialized at process level (§4.1). `Headless` processes use `glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL)` (the null backend is built on every OS in the vendored GLFW 3.5.1), so time, input, window logic and `imgui_impl_glfw` run unchanged without a display. GLFW is initialized after `glfwInitVulkanLoader(vkGetInstanceProcAddr)` so it shares the engine's loader. Framebuffer size and window size are handled separately (Retina, DPI scale).
- **Events** are `using Event = std::variant<WindowCloseEvent, WindowResizeEvent, WindowFocusEvent, KeyEvent, CharEvent, MouseButtonEvent, MouseMoveEvent, MouseScrollEvent, FileDropEvent, GamepadEvent>`, handled with `std::visit` and an `Overloaded` helper, with a `Handled` flag. There is no event class hierarchy.
- **InputState** is plain data owned by `EngineContext`: keys, mouse buttons, cursor, scroll, up to 4 gamepads (GLFW mappings). `InputState::Inject(const Event&)` is the only write path, used by GLFW callbacks, automation and tests.
- **Phase-aware edges.** `InputState` keeps two latched views. `LatchStep()` runs at the start of every fixed step and records `Pressed`/`Released` edges accumulated since the previous step. `LatchFrame()` runs once per frame before `OnUpdate`. A tap that goes down and up between two steps reports both edges on the next step. Script queries read the step view inside `OnFixedUpdate` and physics callbacks, and the frame view inside `OnUpdate`/`OnLateUpdate`. Edges are therefore never dropped or doubled, whatever the frame rate.
- **Actions.** `InputActionMap` resolves the project's named actions: `Button` actions (any bound key, mouse button or gamepad button) and `Axis` actions (positive/negative bindings plus an optional gamepad axis with dead zone 0.15, combined as the max magnitude). Binding names: `Key.Space`, `Mouse.Left`, `Gamepad.South`, `Gamepad.LeftX`. **Axis convention:** every engine axis is right-positive and up-positive. GLFW reports the stick Y axes as positive-down, so `InputState` negates `LeftY` and `RightY` when it reads GLFW, and `Gamepad.LeftY` bound to `MoveZ` means forward is positive. An axis action may set `"Invert": true` to flip its gamepad axis for players who want it. A unit table test covers both sticks and the inversion.
- In the editor, real input reaches the game only while the Game viewport has focus; injected input always reaches the game and never the editor UI.

### 4.4 Logging
spdlog (compiled library, `std::format`) with three loggers: `Engine` (macros `ENGINE_CORE_TRACE|INFO|WARN|ERROR|CRITICAL`), `App` (client macros `ENGINE_TRACE…`, used by Editor and Runtime) and `Script` (Luau `Log.*` and `print`). Sinks:
- colour console (not in Dist);
- rotating file `<UserData>/<AppName>/Logs/<exe>.log` (5 × 10 MB). `AppName` is `ENGINE_PRODUCT_NAME` for the Editor and Tests and the manifest `Name` for exported games (§14.3), so a shipped Tetris never writes into a "Yggdrasil" folder. The same rule applies to `Crashes/` and `user://`;
- **ring buffer**: 16,384 structured `LogEntry {Seq, TimeNs, Tick, Level, Logger, Message, File, Line, EntityId, ScriptFile, ScriptLine}`. Context comes from a thread-local `LogContextScope` (the script engine sets the entity and script location around callbacks). The editor console, `log.read`, the `_meta` diagnostics delta and crash reports read it by monotonic `Seq`.

Trace is compiled out in Dist (`if constexpr`); Info and above stay in the file sink. A Trace or Info message is never emitted every frame.

**Test policy:** a doctest listener fails any test that logs an Error or Critical it did not declare with `Test::ExpectLog log(LogLevel::Error, "substring");` (RAII scope that also fails if the expected entry never appears).

### 4.5 Assertions
| Macro | Debug / Release | Dist | Use |
|---|---|---|---|
| `ENGINE_CORE_ASSERT(cond, fmt, ...)` / `ENGINE_ASSERT` | evaluated → handler | compiled out | preconditions, invariants |
| `ENGINE_CORE_VERIFY(cond, fmt, ...)` / `ENGINE_VERIFY` | evaluated → handler | evaluated → fatal (exit 4) | invariants whose violation would corrupt data or GPU state |
| `ENGINE_UNREACHABLE(fmt, ...)` | handler | fatal | exhaustive switches (after which code returns a safe fallback per CodeStyle) |

The default handler logs the expression, location and message, breaks into an attached debugger, then calls the crash handler (exit 4). Third-party asserts are routed in: `JPH::AssertFailed`, `Luau::assertHandler()`, and EnTT through `Engine/Config/entt/ext/config.h` (`#define ENTT_ASSERT(condition, msg) ENGINE_CORE_ASSERT(condition, "{}", msg)`, on the include path of every first-party project).

**Tests never throw from an assert handler.** The Tests binary installs a handler that records a doctest failure with the message and then terminates through the crash handler (doctest's crash reporting names the running test case). Expected asserts are tested as **death tests**: `ENGINE_DEATH_TEST("Scene/DestroyedEntityAccess") { ... }` registers a body that runs only when the binary is started as `Tests --death-test=<name>`. The parent test spawns that child through `Platform::Process` and checks exit code 4 plus a stderr substring. Nothing ever unwinds through `noexcept` code, destructors or C callbacks.

### 4.6 Error-handling policy
| Category | Examples | Mechanism | Surfaces as |
|---|---|---|---|
| Programmer error | invalid `Entity`, index out of range, API misuse | `ENGINE_*_ASSERT` / `VERIFY` | crash report (it is a bug) |
| Expected failure | missing file, bad JSON, import or compile error, invalid automation params, unknown component name | `Result<T>` / `Status` | caller → nearest boundary |
| Script fault | Luau runtime error, timeout, memory cap | `lua_pcall`/`lua_resume` → `ScriptError` | script error stream; instance disabled |
| Fatal environment | no Vulkan loader or device (startup), device loss, GPU out-of-memory, GPU hang (bounded wait expired), `std::bad_alloc` | `FatalError(kind, message)`: flush logs, autosave (editor; never touches the GPU), crash report (with `VK_EXT_device_fault` data when available), message box when windowed | exit 3 at startup, exit 4 otherwise |

```cpp
enum class ErrorCode : uint16_t { Unknown, InvalidArgument, NotFound, AlreadyExists, InvalidState, Io, Parse,
	Validation, UnsupportedVersion, ImportFailed, CompileFailed, Script, Gpu, Timeout, PermissionDenied,
	Unsupported, Conflict, Cancelled };
struct ErrorLocation { std::string File; uint32_t Line = 0; uint32_t Column = 0; std::string JsonPointer; UUID Entity; };
class Error
{
public:
	Error(ErrorCode code, std::string message);
	Error&& WithContext(std::string context) &&;   // "while importing 'Assets/Track.glb'"
	Error&& WithLocation(ErrorLocation location) &&;
	Error&& WithHint(std::string hint) &&;         // "did you mean 'Mass'?"
	ErrorCode GetCode() const; const std::string& GetMessage() const; std::string ToString() const;
};
template<typename T = void> using Result = std::expected<T, Error>;   // MSVC 14.51, libstdc++ 14, libc++ 18
using Status = Result<void>;
#define ENGINE_TRY(expr)               // on failure: return std::unexpected(std::move(error))
#define ENGINE_TRY_ASSIGN(decl, expr)  // unique temporary via __LINE__; decl = std::move(*temporary)
```
`Result`, `Status` and every function returning them are `[[nodiscard]]`. Boundaries where an `Error` becomes user-visible: file loaders, importers, automation handlers (JSON-RPC error), script calls (script error stream), shader and pipeline loading, device creation, project open/save, export, and process exit codes.

**Exceptions are enabled** (vulkan.hpp, Luau in C++-exception mode, nlohmann and the standard library need them) **but first-party code never throws** (`throw` is banned by lint). JSON needs no `try` at all: parsing uses `json::parse(text, nullptr, false)` + `is_discarded()`, and typed reads go through `JsonReader`, which records JSON pointers (`/Entities/12/Components/RigidBody/Mass: expected number, got string`); `json::at` and `get<>` outside `Core/Json` are banned. `try`/`catch` appears only in these allowlisted boundary files:
1. `Graphics/GraphicsDevice.cpp`: during instance and device creation, `vk::SystemError` and the `std::runtime_error` thrown by `vk::detail::DynamicLoader` when no loader exists become `Result` errors ("No Vulkan loader found; install a GPU driver with Vulkan 1.3 support").
2. `App/FrameLoop.cpp`: one frame-boundary `catch (const vk::SystemError&)` around the frame. vulkan.hpp's enhanced mode throws from calls NVRHI makes internally, for example `vk::DeviceLostError` from the semaphore waits and `vk::OutOfDeviceMemoryError`. The catch maps `eErrorDeviceLost` to `FatalError(DeviceLost)`, the out-of-memory codes to `FatalError(OutOfMemory)` and everything else to `FatalError(Gpu)`. It never resumes the frame. First-party Vulkan calls (swapchain, bounded waits, host image copy) never throw, because they go through the dispatcher's C entry points and check `VkResult` (§8.1).
3. `Scripting`: every call into Luau goes through `Lua::ProtectedCall`. Bindings are exception-neutral: RAII only, never `catch (...)` around `lua_*` calls (it would swallow Luau errors), and they throw nothing but Luau errors.
4. `EditorCore/Scripting/ScriptTypeChecker.cpp`: `catch (const Luau::InternalCompilerError&)` (including time-limit errors) → diagnostic.
5. `Automation/Protocol/Dispatcher.cpp`: `catch (const std::exception&)` → `Internal` error; asserts in Debug. A `std::system_error` goes first to the host's `SystemErrorHandler` (`DispatcherSpecification::SystemErrors`): the editor maps a Vulkan error (a `vk::SystemError` thrown out of NVRHI inside a method such as `viewport.screenshot`) exactly like item 2, through `RaiseVulkanError`, which item 2's catch calls too (`Docs/Decisions/0009-m5-decisions.md` decision 33; first-party code never rethrows).
6. `Core/Jobs/JobSystem.cpp` worker entry and `App/EntryPoint.cpp`: catch-all → `FatalError`.
7. `EditorCore/Autosave/Autosave.cpp`: only `Autosave::WriteFatalSnapshot` catches exceptions from the best-effort native snapshot write and returns `IoFailure`. It must not log, construct an allocating error or invoke fatal handling again; the writer lease is released without allocation. Ordinary autosave calls retain the normal error path. This boundary prevents a second exception during an already-fatal report (`Docs/Decisions/0018-m10-contract.md`, implementation contract review).

`std::filesystem` is used only through its `std::error_code` overloads (lint-enforced).

### 4.7 Ownership
- `Scope<T>` = `std::unique_ptr<T>`, the default. `Ref<T>` = `std::shared_ptr<T>`, used only for genuinely shared immutable data: loaded assets (`AssetRef<T>` = `Ref<const T>`) and job results. No intrusive counting and no `WeakRef` alias. `CreateScope`/`CreateRef` live in `Core/Base.h`.
- GPU objects are held by NVRHI handles (`nvrhi::TextureHandle` …), each owned by exactly one engine object (§8.14). Jolt objects use `JPH::Ref` where Jolt expects it; miniaudio and GLFW objects are wrapped in RAII types.
- `Handle<T>` is a generational 32+32-bit index into a `HandlePool<T>`, used for audio voices and debug-draw buffers. A stale handle yields `NotFound` or an assert, never undefined behaviour.
- Components reference entities by `UUID` (`EntityRef`) and assets by `AssetHandle`, never by pointer. Code never stores a component reference across a structural change.
- Raw pointers and references are non-owning, never stored beyond the call, except documented back-references such as `Entity::m_Scene`.
- Memory: the default allocator, plus Luau's tracking allocator (§11.1), Jolt's default allocator and `TempAllocatorImpl`, and a per-frame `FrameArena` for the render snapshot and debug draw lists. No general custom allocator in v1.

### 4.8 UUIDs
`UUID` is 64 bits; 0 is invalid. Text form is 16 lowercase hex digits (parsing accepts either case); UUIDs are never written as JSON numbers, because JSON consumers lose precision above 2^53. Automation also accepts a unique prefix of at least 6 hex digits.

`UUIDGenerator` is an instance, never a global:
- **Editor:** xoshiro256** seeded from OS CSPRNG bytes.
- **Runtime (play, export, tests):** `UUID = Hash64(sessionSeed, spawnCounter)`, re-hashed with the next counter on the (astronomically unlikely) collision. Runtime spawns are therefore reproducible, which keeps UUID-sorted event orders deterministic.
- **Prefab instances:** `InstanceID = Hash64(instanceRootID, prefabEntityID)` (§5.5).
- **Sub-assets:** `Hash64(sourceHandle, subAssetKey)` (§7.1).
- **Built-in assets:** reserved range `0x0000000000000001`–`0x00000000000003ff`, named in `Resources/EngineAssets.json`.

`Hash64` is XXH64 (in-house implementation verified against the reference test vectors).

### 4.9 Engine events
Window and input events are the `Event` variant above. Engine-level notifications (`EntityCreated`, `EntityDestroyed`, `ComponentChanged`, `SceneOpened`, `SceneChangedOnDisk`, `PlayStateChanged`, `AssetReloaded`, `AssetImportFailed`, `ScriptErrorRaised`, `DiagnosticsChanged`, `AutomationClientDisconnected`) are appended to `EventLog`, a cursor-based ring of 4,096 entries on the main thread. The editor panels, `events.read` and the `_meta` delta read it. There is no pub/sub event bus; systems call each other directly in the documented step order.

### 4.10 Filesystem and VFS
```cpp
class VirtualFileSystem
{
public:
	Status Mount(std::string_view scheme, Scope<IMount> mount);     // engine:// project:// user:// cache://
	Result<Buffer> ReadFile(const VfsPath& path) const;
	Result<std::string> ReadText(const VfsPath& path) const;        // UTF-8 validated
	Result<Scope<IFileStream>> Open(const VfsPath& path) const;     // streaming (audio)
	Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data);
	bool Exists(const VfsPath& path) const;
	Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive = false) const;
};
```
- Mounts: `NativeDirectoryMount`, `PakMount` (read-only), `MemoryMount` (tests), `OverlayMount` (an in-memory write layer over another mount, used by automation `dryRun`, §13.4). `engine://` = `Resources/` (dev) or `Engine.pak`; `project://` = project root or `Game.pak`; `user://` = `<UserData>/<AppName>/` (§4.4); `cache://` = `<Project>/Library/Cache`; `enginecache://` = `bin/EngineCache` (dev builds only, §7.5).
- `VfsPath` uses forward slashes and rejects `..` escapes, absolute paths, NUL bytes, Windows reserved names and trailing dots or spaces.
- **Case policy:** VFS paths are case-sensitive on every host. On case-insensitive hosts the native mount compares the on-disk spelling and returns `Validation: case mismatch`, so a reference that would break on Linux breaks on Windows too.
- **Atomic writes:** temp file → flush → `ReplaceFileW` (Windows) / `rename` (POSIX), keeping one `.bak`. A crash mid-save never corrupts a file.

### 4.11 Threading and jobs
- **Main thread:** ECS, runtime scripts, physics stepping (Jolt fans out to its own workers), NVRHI command recording and submission, ImGui, automation request execution. Debug builds assert the thread ID at `Scene`, `ScriptEngine` and `GraphicsDevice` entry points. Import-time `LoadTimeVm` instances are isolated and confined to their import worker, after process-level Luau initialization; they touch no scene, runtime VM, host or shared mutable counters (ADR 0019).
- **`JobSystem`:** `max(1, hardware_concurrency - 2)` workers for import, decode, cook, mip generation and file-watcher polling. `Submit(fn) -> JobHandle<Result<T>>` and `ContinueOnMainThread(handle, fn)`. Jobs take and return values; ECS access from a job is forbidden. `JobSystem(0)` runs jobs inline, for deterministic tests.
- **Jolt** uses its own process-level `JPH::JobSystemThreadPool` (threads = `clamp(hw - 2, 1, 4)` unless `ProcessContextSpecification::Physics` gives 0 to 64; `PhysicsEngine::SetWorkerThreadCount` changes it between steps, and tests also run 0 and 8), not an adapter onto `JobSystem`. **miniaudio**: the engine-owned `ma_device`'s data callback runs on miniaudio's device thread and only calls `ma_engine_read_pcm_frames` (§10.1), under the engine's read lock taken with `try_lock`: it outputs silence when it cannot get the lock or while simulation time is owned, so it never waits on the main thread. Its notifications are only queued; `AudioEngine::Update` handles them on the main thread. Every other miniaudio call comes from the main thread. **Automation** has one I/O thread that only frames, parses and queues.
- **No render thread** in v1. Two frames in flight.
- **Uploads inside a frame.** `HostImageUpload` records into non-immediate command lists: the scene renderer resolves textures, materials and environments while the caller's immediate list is open, and NVRHI allows one open immediate list at a time (ADR 0013 decision 22).

### 4.12 Time, randomness and deterministic math
`SimStep {Tick, FixedDelta, Time}` and `FrameTime {DeltaTime, UnscaledDeltaTime, Alpha, FrameIndex}` are passed explicitly; there is no global time. Simulation time is `Tick * FixedDelta`, never the wall clock. `Random` is xoshiro256** with explicit seeding. Each play session owns one stream seeded with `Project.Simulation.Seed ^ Scene.Seed` (or the `play.start` seed); Luau `math.random` and `Random.*` both use it.

**`Core/DetMath`** provides `Sin`, `Cos`, `Tan`, `ASin`, `ACos`, `ATan`, `ATan2`, `Exp`, `Log`, `Pow` and `SinCos`, for `float` and `double`. They are in-house polynomial implementations, written in the style of Jolt's `Trigonometry.h` (MIT, attributed in `LICENSES.md`) and built only from IEEE-exact operations (`+ − × ÷ sqrt`, floor, frexp/ldexp). C runtime transcendental functions differ between C runtimes and may pick CPU-specific code paths at run time, so they are banned on the simulation path. That path covers `Core`, `Scene`, `Physics`, `Scripting` and `Session` code that affects simulated state, including Euler ↔ quaternion conversion, `Transform:Rotate`/`LookAt`, `Quat.*` and `Math.*`. `Lint.py` enforces the ban (§2.3). Rendering, audio and editor code may use `<cmath>`. In the script sandbox, Luau's `math.sin/cos/tan/asin/acos/atan/atan2/exp/log/pow/sinh/cosh/tanh/log10` are rebound to DetMath, like `math.random` (§11.1). Luau's `^` operator is exact for the exponents 2, 3 and 0.5, which the VM special-cases; any other exponent calls the CRT `pow`. That path is covered by the cross-configuration determinism test (§15.8), and the `luau-gameplay` skill recommends `math.pow` for non-trivial exponents. Tests: accuracy within 1 ULP of correctly rounded references over seeded tables (the exact results rounded to nearest, generated with exact arithmetic by `Scripts/GenerateDetMathReference.py` into `Tests/Source/Engine/Core/DetMathReferenceData.h`; never the platform's libm, whose accuracy varies between C runtimes, `Docs/Decisions/0007-detmath-reference-oracle.md`), and the XXH64 of DetMath outputs for 1,000,000 seeded inputs equals a committed value in every configuration.

### 4.13 Crash safety and profiling
- `CrashHandler`: Windows `SetUnhandledExceptionFilter` + `MiniDumpWriteDump`; POSIX `sigaction` on an alternate signal stack, async-signal-safe `write()` only. The report holds build info, the last 256 log lines (copied into a preallocated buffer), breadcrumbs (scene, play state, current automation method, current script callback, frame phase), the stack trace and, after a device loss, the `VK_EXT_device_fault` description when the extension is available. Reports go to `<UserData>/<AppName>/Crashes/` (§4.4).
- The editor autosaves the open scene (when dirty) and dirty native assets to `Library/Autosave/` on entering play and every 2 minutes. Autosave is pure CPU serialization and never touches the GPU, so it also runs from `FatalError(DeviceLost)`. If an autosave is newer than the scene file at open, the editor offers recovery (`project.open {recover: true}` for automation). The MCP bridge reports the crash-report path when the editor dies.
  The M10 contract (ADR 0018) refines this for the existing write-through native-asset/settings commands: only committed dirty scene state needs recovery. Main-thread safe points publish owned serialized bytes and resolved paths; a fatal hook on another thread claims a bounded immutable snapshot without reading live editor state or waiting on a lock. Complete generation manifests publish last. Recovery validates canonical project/source identity and fingerprints before installing a dirty scene in memory; it never overwrites source assets during adoption. `FileInfo::ModificationTime` is equality-only: a dirty revision based on the still-matching captured source establishes freshness, not numerical timestamp ordering. Read-only/dry-run states publish no writable snapshot. Shared fallible lifecycle callbacks prepare autosave before the play copy, clean recovery only after a durable explicit save, and quiesce writers before project close. A refused close retains the scene, mounts and lock for retry; cleanup failure after saving reports that the source was saved (ADR 0018).
- **Single writer per project.** An editor that opens a project takes an exclusive OS lock on `Library/Editor.lock` (`LockFileEx` on a byte range past the pid text on Windows, `flock` on POSIX) and writes its pid into the file, which other processes can still read. The OS releases the lock when the process dies, so a crashed editor never leaves a stale lock. A second editor on the same project refuses to open it: windowed editors show a message naming the pid, and CLI and headless editors exit 3. The exception is `--read-only`, which opens the project with mutations denied, autosave off, a private temporary cache and no writes under the project. Automation attaches to the running editor instead of starting a second one (§13.8).
- `ENGINE_PROFILE_SCOPE("Name")` writes to per-thread ring buffers. GPU timer queries are merged into the same timeline, readable through `stats.get` and exportable as Chrome trace JSON.

---

## 5. ECS and scene

### 5.1 EnTT usage and identity
Each `Scene` owns one `entt::registry` (EnTT 4.0.0 APIs) and a `std::unordered_map<UUID, entt::entity>` index that is used for lookup and never iterated to produce output. `Entity` is a value type `{entt::entity, Scene*}`; every accessor asserts validity. **The UUID is the identity**; `entt::entity` is transient, never serialized and never exposed to scripts or automation. EnTT type hashes differ between compilers, so components are always serialized under registry names.

```cpp
class Scene
{
public:
	static Scope<Scene> Create(const SceneSpecification& specification);   // name, seed, TypeRegistry&, UUIDGenerator&
	Entity CreateEntity(std::string_view name = "Entity", Entity parent = {});
	Entity CreateEntityWithID(UUID id, std::string_view name, Entity parent = {});   // asserts uniqueness (API misuse); loaders pre-validate (§6)
	void DestroyEntity(Entity entity);       // immediate in edit; deferred to a flush point at runtime
	Entity FindEntityByID(UUID id) const;
	Entity FindEntityByPath(std::string_view path) const;   // "/Game/Board", "/Track/Piece[3]"
	void SetParent(Entity child, Entity parent, std::optional<uint32_t> siblingIndex = {}, bool keepWorld = true);
	std::span<const UUID> GetRootEntities() const;          // ordered
	template<typename Func> void ForEachCanonical(Func&& func) const;
	uint64_t GetRevision() const;            // increments on every mutation (automation ifRevision)
	uint64_t ComputeStateHash() const;       // XXH64 of canonical serialization (+ physics velocities at runtime)
};
```
Editor and automation code mutate components only through `Entity::Patch<T>(func)` or the reflected setters, which call `registry.patch<T>` (firing EnTT `on_update`) and notify the scene's change tracker (used by undo, §12.3). Runtime systems may write components directly.

**Canonical order.** EnTT storage order depends on insertion history. Wherever order is observable (script callbacks, serialization, physics body creation, query results, transparent sort ties) the engine iterates in canonical order: root entities in stored order, then depth-first through each entity's ordered children. Loading or copying a scene creates entities in canonical order.

### 5.2 Hierarchy, transforms and active state
- `RelationshipComponent {UUID Parent; std::vector<UUID> Children;}` keeps child order explicitly. Files store only `Parent`; array order defines sibling order, and `Children` is rebuilt on load.
- `TransformComponent` is **local** TRS with a quaternion rotation. `WorldTransformComponent {glm::mat4 Matrix;}` is runtime-only. `TransformSystem::Update` recomputes every world matrix in cached canonical order (rebuilt when structure changes): O(n), no dirty flags, no stale-cache bugs. Gameplay reads of world values between updates (`Transform.WorldPosition`) walk the parent chain on demand. `WorldPosition` and `WorldRotation` are writable (the setter converts through the parent's inverse); `WorldScale` is read-only.
- **Render interpolation.** `PreviousWorldTransformComponent` is runtime-only. At the start of every fixed step (§5.7, step 0), the session copies `WorldTransform` into `PreviousWorldTransform` for **every** entity, whatever moved it: physics, a script in `OnFixedUpdate`, a kinematic platform or automation. Extraction then renders `lerp/slerp(Previous, Current, Alpha)`. Three cases are **not interpolated** (Previous = Current): entities whose transform was written during the frame phase (`OnUpdate`, `OnLateUpdate`, editor gizmo, automation outside a step), together with their descendants; entities created or enabled since the last step; and teleports (`RigidBody:Teleport`, `Transform:Teleport`, automation `entity.update` on a play scene). A runtime-only `InterpolationResetTag` marks such entities until the next snapshot. Scripts read the rendered pose through `Transform.RenderPosition`/`RenderRotation` (read-only, interpolated at this frame's Alpha; equal to the world pose inside the fixed phase) and `Time.GetInterpolationAlpha()`. A follow camera in `OnLateUpdate` therefore tracks `ball.Transform.RenderPosition`, never `WorldPosition`, and stays smooth at any frame rate (idiom in the `luau-gameplay` skill; test in §15.5 `Timing`).
- Reparenting keeps the world transform by default (`keepWorld = false` keeps local). Cycles are rejected with `InvalidArgument`.
- The inspector shows Euler degrees from a per-entity editor-side cache so angles do not flip while dragging.
- **Active state.** `DisabledTag` (serialized as entity `"Active": false`) deactivates an entity and its subtree. The runtime-only `HierarchyDisabledTag` marks effectively disabled entities; render extraction, physics, audio and script updates exclude them. `OnEnable`/`OnDisable` fire when the effective state changes (not at creation). A disabled entity's physics body is removed from the world and re-added on enable.
- Conventions: right-handed, +Y up, metres, kilograms, seconds. Cameras and lights look down local −Z; `Transform:Forward()` is −Z. Angles are degrees in components, scripts and automation. Quaternions serialize as `[x, y, z, w]`. Colours are linear.

### 5.3 Component list
Types: `AssetRef<T>` fields hold an `AssetHandle`; `EntityRef` holds a `UUID`; `color3/color4` are linear `glm::vec3/vec4`; `bool3` is `glm::bvec3`. Every field has a default member initializer. *Virtual* fields are registered getters/setters, accepted by scripts and automation, never serialized.

| Component (registry name) | Fields (defaults) | Flags |
|---|---|---|
| `IDComponent` (`ID`) | `UUID ID` | required, hidden |
| `NameComponent` (`Name`) | `std::string Name{"Entity"}` | required, entity-level; serialized as entity `"Name"` |
| `TagsComponent` (`Tags`) | `std::vector<std::string> Tags` | entity-level; serialized as entity `"Tags"` |
| `RelationshipComponent` | `UUID Parent; std::vector<UUID> Children` | required, hidden; edited through hierarchy operations |
| `DisabledTag` | — | serialized as entity `"Active": false` |
| `TransformComponent` (`Transform`) | `vec3 Translation{0}; quat Rotation{identity}; vec3 Scale{1}` (each component's magnitude ≥ 1e-4); virtual `EulerAngles` (local, deg, rw), `WorldPosition` (rw), `WorldRotation` (rw), `WorldScale` (read-only), `RenderPosition`, `RenderRotation` (read-only, interpolated, §5.2) | required |
| `PrefabInstanceComponent` (`Prefab`) | `AssetRef<Prefab> Prefab; std::vector<PrefabOverride> Overrides` | hidden; instance root only |
| `PrefabLinkComponent` (`PrefabLink`) | `UUID PrefabEntityID; UUID InstanceRoot` | hidden; every instance member incl. root |
| `MeshRendererComponent` (`MeshRenderer`) | `AssetRef<Mesh> Mesh; std::vector<AssetRef<Material>> Materials` (per submesh; empty or null slot = mesh default); `bool CastShadows{true}; bool ReceiveShadows{true}; bool Visible{true}` | |
| `CameraComponent` (`Camera`) | `ProjectionType Projection{Perspective}` (Perspective, Orthographic); `float VerticalFov{60}` (deg); `float OrthographicSize{10}` (half height, m); `float NearClip{0.1}; float FarClip{1000}` (ortho range, culling); `bool Primary{false}; ClearMode Clear{Skybox}` (Skybox, Color); `color3 ClearColor{0.05,0.05,0.06}` | |
| `DirectionalLightComponent` (`DirectionalLight`) | `color3 Color{1}; float Intensity{3}; bool CastShadows{true}; float ShadowDistance{100}; uint32 CascadeCount{4}` (1–4); `float CascadeSplitLambda{0.75}; float LightAngle{1.0}` (angular diameter, deg; PCSS penumbra); `float DepthBias{1.0}; float NormalBias{1.0}` (texel-scaled) | |
| `PointLightComponent` (`PointLight`) | `color3 Color{1}; float Intensity{10}; float Range{10}; float SourceRadius{0.05}` | no shadows in v1 |
| `SpotLightComponent` (`SpotLight`) | `color3 Color{1}; float Intensity{10}; float Range{15}; float InnerConeAngle{20}; float OuterConeAngle{30}; bool CastShadows{false}; float SourceRadius{0.05}` | |
| `EnvironmentComponent` (`Environment`) | `AssetRef<Environment> Environment; float Intensity{1}` (≤ 65,504, the largest binary16 value); `float Rotation{0}` (deg about Y); `bool ShowSkybox{true}; float SkyboxBlur{0}` (0–1); `color3 FallbackColor{0.2,0.22,0.25}` (ambient when no map) | unique per scene |
| `PostProcessComponent` (`PostProcess`) | `float ExposureEV{0}` (multiplier 2^EV); `Tonemapper Tonemap{AgX}` (AgX, ACES, PbrNeutral, Linear); `bool SsaoEnabled{true}; float SsaoRadius{0.5}; float SsaoIntensity{1}; SsaoQuality SsaoQuality{Medium}` (Low, Medium, High); `bool BloomEnabled{true}; float BloomIntensity{0.04}; bool FxaaEnabled{true}` | unique per scene |
| `TextComponent` (`Text`) | `std::string Text; AssetRef<Font> Font` (null = default); `float Size{32}` (px at 1080p reference, ≤ 100,000; a World text's em is `Size / 100` m before the entity's scale); `color4 Color{1}; TextSpace Space{Screen}` (Screen, World); `vec2 Anchor{0.5,0.5}` (viewport-normalized, Screen); `vec2 Pivot{0.5,0.5}; vec2 Offset{0,0}` (px); `TextAlignment Alignment{Center}` (Left, Center, Right); `bool Billboard{false}` (World) | |
| `RigidBodyComponent` (`RigidBody`) | `BodyType Type{Dynamic}` (Static, Kinematic, Dynamic); `float Mass{1}` (kg, ≤ 1e9); `float Friction{0.5}; float Restitution{0}; float LinearDamping{0.05}; float AngularDamping{0.05}; float GravityFactor{1}; MotionQuality MotionQuality{Discrete}` (Discrete, LinearCast); `bool AllowSleeping{true}; bool3 LockTranslation{false}; bool3 LockRotation{false}; std::string Layer{"Default"}; float MaxLinearVelocity{500}; float MaxAngularVelocity{47.12}` (rad/s, Jolt default 0.25·π·60); `bool EnhancedInternalEdgeRemoval{false}` (Jolt `mEnhancedInternalEdgeRemoval`; set on rolling bodies, §9.2); `vec3 InitialLinearVelocity{0}; vec3 InitialAngularVelocity{0}` (Dynamic only) | requires Transform |
| `BoxColliderComponent` (`BoxCollider`) | `vec3 HalfExtents{0.5}` (each ≥ 0.001); `vec3 Offset{0}; quat Rotation{identity}; bool IsTrigger{false}` | requires Transform |
| `SphereColliderComponent` (`SphereCollider`) | `float Radius{0.5}` (≥ 0.001); `vec3 Offset{0}; bool IsTrigger{false}` | requires Transform |
| `CapsuleColliderComponent` (`CapsuleCollider`) | `float Radius{0.5}` (≥ 0.001); `float HalfHeight{0.5}` (cylinder half, Y axis, ≥ 0.001); `vec3 Offset{0}; quat Rotation{identity}; bool IsTrigger{false}` | requires Transform |
| `MeshColliderComponent` (`MeshCollider`) | `AssetRef<Mesh> Mesh` (null = MeshRenderer mesh); `bool Convex{false}; bool IsTrigger{false}` | non-convex requires Static or Kinematic |
| `CharacterControllerComponent` (`CharacterController`) | `float Height{1.8}; float Radius{0.3}` (Height > 2 × Radius; the entity's origin is the capsule's base); `float MaxSlopeAngle{45}; float StepHeight{0.3}; float Mass{70}` (≤ 1e9); `float GravityFactor{1}; std::string Layer{"Default"}` | excludes RigidBody |
| `AudioSourceComponent` (`AudioSource`) | `AssetRef<AudioClip> Clip; float Volume{1}; float Pitch{1}; bool Loop{false}; bool PlayOnStart{false}; bool Spatial{true}; float MinDistance{1}; float MaxDistance{50}; Attenuation Attenuation{Inverse}` (None, Inverse, Linear, Exponential; None pans without distance falloff); `float Rolloff{1}; float DopplerFactor{1}; AudioGroup Group{Sfx}` (Music, Sfx, Ui); both enums are `Audio/AudioTypes.h`'s | |
| `AudioListenerComponent` (`AudioListener`) | `bool Primary{true}` | fallback: primary camera; several active primaries → `AUDIO_MULTIPLE_PRIMARY_LISTENERS` (the first in canonical order wins) |
| `ScriptComponent` (`Script`) | `AssetRef<Script> Script` (must be a Behaviour script, §11.2); `Map<Variant> Fields` (overrides of declared fields, name → value validated against the script's field schema, §5.4); `int32 ExecutionOrder{0}` | one per entity; no shortcut property (§11.4) |

`Name` and `Tags` are *entity-level* components: they serialize as entity keys, scripts reach them as `Entity.Name` and the tag methods, and they have no shortcut property.

Runtime-only (never registered as serializable, invisible to scripts and automation): `WorldTransformComponent`, `PreviousWorldTransformComponent`, `InterpolationResetTag`, `HierarchyDisabledTag`, `PhysicsBodyRuntime`, `CharacterRuntime`, `AudioSourceRuntime`, `ScriptRuntime`, `PendingDestroyTag`, `PendingStartTag`. `PhysicsBodyRuntime` and `CharacterRuntime` are realized as `PhysicsSystem`'s private entries, keyed by body handle and owner UUID, not as EnTT components, and `AudioSourceRuntime` is private to `AudioSystem` (ADR 0014 decision 10, ADR 0015 decision 1).

Physics composition rules (validated, §9):
- **Solid colliders.** Several collider types on one entity, plus solid colliders on descendant entities that have no `RigidBody` of their own, form one `StaticCompoundShape` on the nearest ancestor `RigidBody`. A solid collider with no `RigidBody` on itself or an ancestor gets an **implicit static body**.
- **Shared static bodies for seamless tracks.** Jolt removes internal edges (ghost contacts) only between sub-shapes of **one** shape; adjacent pieces in separate bodies bump at every seam. Track geometry is therefore authored as a `Static` `RigidBody` on the level root, with every track piece a collider-only descendant, so the whole track becomes one static compound. Moving platforms keep their own `Kinematic` body. `PHYSICS_ADJACENT_STATIC_BODIES` (warning, auto-fixable by adding a Static `RigidBody` to the common parent) fires when two implicit static bodies have world AABBs within 1 mm of each other.
- **Trigger colliders** never join a solid compound. A trigger collider on an entity without its own `RigidBody` gets an **implicit sensor body**: Kinematic, following its entity's world pose through `MoveKinematic` each `PreStep`. Checkpoints under a level root, and a trigger attached to a moving ball, therefore just work. Colliders on an entity that has its own `RigidBody` must agree on `IsTrigger` (`PHYSICS_MIXED_TRIGGER` otherwise).
- Friction and restitution live on the body, because Jolt stores them per body. Contact data names the child collider entity that was hit (§9.4).
- **Gathering** (ADR 0014 decision 10). An implicit static body gathers the solid colliders of the collider-only descendants below its entity, as a `RigidBody` does, so a tree of collider-only entities is one static compound and `PHYSICS_ADJACENT_STATIC_BODIES` fires only between separate trees. An implicit sensor body holds only its own entity's trigger colliders. Implicit bodies take the layer of the nearest ancestor `RigidBody`, else layer 0. A sensor `RigidBody` of Type Static is created Kinematic (§9.2). A `RigidBody` without any collider makes no body, without a diagnostic. Solid colliders on a `CharacterController`'s entity and on its collider-only descendants form no body (the capsule is the character's shape); trigger colliders there still get implicit sensor bodies. Effectively disabled entities make no bodies.
- **Collision groups.** A body's group is the nearest entity at or above its owner with a `RigidBody` or a `CharacterController` (none for implicit static bodies). Bodies of one group never collide and never report each other, so a trigger attached to a moving ball or a character never reports its own carrier.
- **Order.** Bodies in canonical order of their owners (an owner's solid body before its sensor body); a body's colliders are the owner's own in `BuiltinComponents` order, then each gathered descendant's in canonical order. A collider's index in that order is its sub-shape user data (§9.2).

### 5.4 Reflection registry: the single source of truth
```cpp
enum class FieldType : uint8_t { Bool, Int32, UInt32, Float, Vec2, Vec3, Vec4, Quat, Color3, Color4, Bool3,
	String, EntityRef, AssetRef, Enum, Array, Struct, Map, Variant };
struct FieldMeta
{
	std::optional<double> Min, Max, Step;
	std::optional<double> MinMagnitude;      // per component |v| >= MinMagnitude (Transform.Scale)
	std::string_view Unit;                   // "kg", "m", "deg"
	AssetType AssetFilter = AssetType::None;
	bool ReadOnly = false, Hidden = false, Virtual = false, Scriptable = true, Serialized = true;
	RunModes Modes = RunModes::All;          // All | EditorOnly (see §11.4)
};
// Variant fields resolve their schema at run time from the owning object and the key or pointer:
using VariantSchemaResolver = Result<const FieldInfo*> (*)(const ResolveContext& context);

registry.Component<RigidBodyComponent>("RigidBody", "Simulates the entity with Jolt physics.")
	.Category("Physics").Version(1).Requires<TransformComponent>().Excludes<CharacterControllerComponent>()
	.Field("Type", &RigidBodyComponent::Type, "Static never moves; Kinematic is moved by script; Dynamic is simulated.")
	.Field("Mass", &RigidBodyComponent::Mass, "Mass in kilograms (Dynamic only).", { .Min = 0.001, .Unit = "kg" })
	.Field("Layer", &RigidBodyComponent::Layer, "Physics layer name from project settings.")
	/* ... */
	.Validate([](const RigidBodyComponent& component, ValidationContext& context)
	{
		if (component.Type == BodyType::Dynamic && component.Mass <= 0.0f)
			context.Error("Mass", "must be > 0 for Dynamic bodies");
	});
```
- **Every float is finite.** Readers, reflected setters, script proxies and automation reject NaN and ±Inf with a located error (`/components/Transform/Translation/1: must be finite`), so non-finite values can never reach Jolt, the renderer or a file. `Min`/`Max`/`MinMagnitude` are enforced on every write path, not only in the inspector.
- **`Map`** is a string-keyed map `std::map<std::string, T>` whose value type is any `FieldType`. Keys are written in sorted (canonical, byte-wise) order. The JSON Schema uses `additionalProperties` with the value schema. Patches use RFC 7386 semantics (`null` deletes a key; nested objects merge). The inspector shows an editable key/value table, and random generators produce 0–8 random keys.
- **`Variant`** holds any JSON-representable value whose schema is resolved at run time by the field's `VariantSchemaResolver`. Validation, drawers, undo, random generation and schema output go through the resolved `FieldInfo`. When resolution fails (unknown script field, missing prefab target), the value is preserved verbatim and a diagnostic is raised, so data is never silently dropped.
- The users of these two types are listed explicitly and tested by the registry suite: `ScriptComponent.Fields` (`Map<Variant>`, resolved against the assigned script's field schema per key), `PrefabOverride.Value` (`Variant`, resolved by the target component and field, or the whole component struct for `AddComponent` overrides) and `ProjectSettings.Input.Actions` (`Map<Struct InputAction>`). A new use requires a registry-test fixture.
- `TypeRegistry` holds `ComponentInfo` (name, mandatory description, category, version, flags `Serializable/ScriptVisible/EditorVisible/Removable/UniquePerScene/Required/Hidden/EntityLevel/NoShortcut`, `Requires`/`Excludes`, ordered fields, type-erased `Add/Remove/Has/Patch/ToJson/FromJson/Validate/Migrate(fromVersion, json&)`), `StructInfo` (the same without ECS operations: `MaterialData`, `ProjectSettings`, every `*ImportSettings`, every automation param/result struct) and `EnumInfo` (name↔value table; **enums serialize by name**).
- Descriptions are mandatory for every type and field (the builder signature requires them); `GenerateDocs.py --check` fails on an empty one.
- Consumers: serializer (§6), inspector (one drawer per `FieldType`; custom drawers only for `Script` fields and `Prefab` overrides), Luau component proxies (§11.4), automation validation and JSON Schemas (§13.4), `Engine.d.luau`, docs, undo (component JSON before and after), the coverage gates (§15.6), and `FuzzySuggest(name)` (Levenshtein over registered names) for "did you mean" hints.
- `using BuiltinComponents = TypeList<...>` is the one list of component types. Startup asserts every listed type is registered. A registry-parametrized doctest checks, for every component and every reflected struct: default → JSON → equal; 200 seeded random in-range round trips (including `Map` and `Variant` fields, with the resolver fed a fixture schema); validation rejects out-of-range and non-finite values; script get/set of every scriptable field; automation set/get; schema validates the serialized output. Adding a component gets these tests for free.

### 5.5 Prefabs
A prefab asset (`.prefab`) is a serialized entity subtree with one root and prefab-local UUIDs (§6.3).
- **Instantiation** (`Scene::InstantiatePrefab(handle, transform, parent, rootID)`, the single path used by the editor, automation and scripts) deep-copies the subtree and derives member IDs deterministically: `InstanceID = Hash64(rootID, PrefabEntityID)`. `EntityRef` fields and script `Entity` field values that point inside the prefab are remapped with the same function. The root gets `PrefabInstanceComponent`; every member gets `PrefabLinkComponent`. Nested prefab instances inside a prefab asset are flattened when the prefab is created.
- **Overrides.** When an edit commits on an instance member, the change tracker records field-level overrides `{PrefabEntityID, Component, Field, Value}` and component-level overrides `{PrefabEntityID, AddComponent|RemoveComponent, Component, Value?}` (`Value` is a `Variant`, §5.4). Root Name, Transform and Parent are always implicit overrides. Entities added under an instance (no `PrefabLinkComponent`) are user children and are preserved.
- **Update.** When a prefab asset changes, every instance in open scenes is rebuilt as *prefab + overrides* inside one undoable command. Derived IDs are stable, so external references into the instance survive. Prefab entities that no longer exist disappear; overrides that no longer match a field are dropped with a warning.
- `prefab.apply` writes an instance's overrides back into the asset; `prefab.revert` clears overrides; `prefab.unpack` removes the link components and keeps the entities.
- Scenes store instances fully expanded plus their override records, so a scene loads even if its prefab is missing (diagnostic `PREFAB_MISSING_ASSET`), and the Runtime never resolves prefabs while loading a scene.

### 5.6 Scene lifecycle: Edit, Play, Simulate
```
Edit ──Play/Simulate──▶ Running ⇄ Paused ──Step(n)──▶ Paused        Running/Paused ──Stop──▶ Edit
```
- **Play** creates a `PlaySession`: `SceneSerializer::ToJson(editScene)` → `SceneSerializer::FromJson(runtimeScene)` through an in-memory `ordered_json` document. This is the exact path the exported Runtime uses to load a scene, which removes "works in the editor, broken in export" bugs. Benchmark gate: 1,000 entities copy in under 30 ms (Release). A faster registry copy is deferred until a scene needs it.
- Session setup: physics bodies are created in canonical order, the script VM is created, script instances are created, `OnCreate` runs for every instance in (`ExecutionOrder`, canonical) order, then `OnStart` for all; `PlayOnStart` audio starts. The `PhysicsSystem` is created right after the scene loads, so queries and `physics.bodyInfo` work at tick 0. The `AudioSystem` starts its `PlayOnStart` sources in canonical order with their voices held paused until the first `AudioUpdate` phase, so the paused and lockstep state a host applies after creation is in force before a voice can reach a device, and their cursors are 0 at tick 0 (ADR 0014 decision 11, ADR 0015 decisions 5 and 13).
- **Simulate** is Play without the script engine and audio, using the editor camera; physics runs as in Play.
- **Pause / Step(n)** freezes the scheduler; Step runs exactly n ticks (each tick = fixed step + frame phase, as in lockstep).
- **Lockstep** (automation, §13.6): the wall clock is ignored and ticks advance only through `play.step`.
- **Stop** destroys the session (`OnDestroy` for all instances, bodies, voices, VM). The edit scene is never touched during play; selection is restored by UUID.
- **Scene.Load** from a script swaps the session's scene at the end of the frame: the current runtime scene is torn down (including its VM, so state never leaks), the new scene is loaded through the serializer, and the optional parameters table (JSON-serializable) is readable in the new scene through `Scene.GetLoadParameters()`.
- Edits made during play affect only the runtime scene, are not undoable, are visibly tinted in the UI, and are discarded on Stop.

### 5.7 Step order (`PlaySession`), documented and tested
Fixed step (repeated `StepCount` times per frame, one per tick):
0. Interpolation snapshot: `PreviousWorldTransform ← WorldTransform` for every entity; clear `InterpolationResetTag` (§5.2).
1. Apply injected input events stamped for this tick (and, during a replay, the replay's events); `InputState::LatchStep()`. A recording captures every event applied here (§13.6).
2. Start flush: `OnStart` for instances created since the last flush, canonical order.
3. Script `OnFixedUpdate(fixedDt)` for every enabled instance, (`ExecutionOrder`, canonical) order; the iteration list is snapshotted at phase start.
4. Task scheduler resumes `Task.Wait*` and `Task.Delay` threads due at this tick (simulation time), then the running test case thread, if any (§11.10).
5. `TransformSystem::Update`, then `PhysicsSystem::PreStep`: create, rebuild or remove bodies for changed components; teleport bodies whose world pose was changed by script or automation (including writes made in steps 3–4 of this tick, which the transform update has just propagated); `MoveKinematic` for kinematic and implicit sensor bodies.
6. `PhysicsWorld::Step(fixedDt, collisionSteps)` (§9.3).
7. `PhysicsSystem::PostStep`: write dynamic poses back (world → local); dispatch sorted collision and trigger events (§9.4).
8. Destroy and disable flush: `OnDestroy`, then synthesized `OnCollisionExit`/`OnTriggerExit` to surviving contact partners of destroyed or newly disabled entities (§9.4), then body/voice/instance removal, then entity destruction, children first. `PhysicsSystem::FlushDestroyed` (synthesized exits, then body and character removal) runs before `Scene::FlushPendingDestroys` and repeats until a pass finds nothing, since its callbacks may destroy or disable more; `AudioSystem` releases voices from its destroy signals.
9. `TransformSystem::Update`.

Frame phase (once per frame; once per tick in lockstep and with `ManualClock`):
1. `InputState::LatchFrame()`; start flush.
2. Script `OnUpdate(frameDt)`, then `OnLateUpdate(frameDt)`; destroy flush; `TransformSystem::Update`. Transforms written in this phase get `InterpolationResetTag`.
3. `AudioSystem::Update` (listener, source positions and velocities), then, while the session owns audio time (§10.1), one tick of frames for every tick run since the last pull. Play mode only.
4. Render extraction: interpolates `PreviousWorldTransform` → `WorldTransform` by `Alpha`, except for entities with `InterpolationResetTag` or a tagged ancestor, which render at `WorldTransform`.

**Creation and destruction semantics.** `Scene.CreateEntity` and `Scene.Instantiate` take effect immediately: the entity and components exist, script instances are created and `OnCreate` runs before the call returns (re-entrancy depth limit 16), the physics body is created at the next `PreStep`, and `OnStart` runs at the next start flush, before the instance's first update. Entities created during a phase are not visited by that phase. `Destroy` marks the entity with `PendingDestroyTag`: `IsValid()` returns false immediately, further script access raises `entity 'Ball' (5d1c…) was destroyed at tick 812`, and the flush points above perform the destruction. A session holds at most `Simulation.MaxEntities` entities (default 65,536). Creating or instantiating beyond that raises a script error (`entity limit 65536 reached`) or an automation `InvalidState` error, never a crash. Jolt's own body limit (§9.1) is checked with a diagnostic before it can be reached.

---

## 6. Serialization and file formats

All authored files are UTF-8 JSON, LF, tab-indented (per `.editorconfig`), written by the canonical `JsonWriter`:
- Header keys `"Format"` and `"Version"` first. Keys in registry order. Entities in canonical order. **PascalCase keys** in every authored file. (Automation envelopes use camelCase; component and asset data inside them keep PascalCase.)
- Floats: shortest round-trip representation of the `float` value (`std::to_chars` on `float`), `-0` written as `0`, NaN/Inf rejected. Default-valued fields are written: explicit beats implicit for agents reading files.
- **Load → save is byte-identical.** A test asserts it for every authored file under `Projects/` and `Tests/Data`. Line-oriented logs are exempt from the canonical writer and from this test: `.jsonl` files (batch scaffolds, `BuildLog.jsonl`) and everything under a project's `Automation/` folder. `Provenance.json` is still written canonically, but it is not engine-consumed.
- **Strict reader.** `JsonReader` errors carry JSON pointers. Unknown fields warn; unknown components warn and are **preserved verbatim** on save (forward compatibility). `--strict` (CI) turns both into errors. A file whose `Version` is newer than the reader supports fails with `UnsupportedVersion`, naming both versions.
- **Structural pre-validation.** Scene and prefab loaders validate the whole entity graph **before** creating any entity. Merge results and hand edits are expected inputs, so each case returns a located `Result` (JSON pointer), never an assert: duplicate entity IDs, an invalid or zero ID, a `Parent` that does not exist (dangling) or forms a cycle, a `PrefabLink` whose `InstanceRoot` is missing or not an instance root, a member whose root lacks `PrefabInstanceComponent`, and a duplicated unique-per-scene component. A child listed before its parent is accepted and normalized to canonical order, with the warning `SCENE_NONCANONICAL_ORDER`. Asserts (`CreateEntityWithID`) are reserved for in-memory API misuse. Strict loading (Play copy, Runtime, export) fails on any error. **Repair loading** (`scene.open {repair: true}`, `project.validate {fix}`) applies deterministic fixes and reports each one: the later duplicate gets a fresh ID, orphans are reparented to the root, a cycle is cut at its first back-edge in file order, inconsistent prefab members are unpacked, and extra unique components are dropped (their JSON goes into the report). Each case has a fixture under `Tests/Data/Scenes/Invalid/`, in addition to the mutation fuzz test.
- **Migrations** are pure `json → json` functions: file-level (`Version`) and per component (`ComponentVersions`). Every migration has a golden fixture in `Tests/Data/Formats/`, and fixtures for every old version must keep loading. `project.upgrade` (§13.5) rewrites a project's files in the current canonical form and records the rewrite in provenance.
- **Enum spelling.** Files always contain the registry's PascalCase enum names. Readers of authored files are strict (exact case). Automation parses enum values case-insensitively (`"tap"`, `"Tap"`) and responses echo the canonical spelling.
- Extensions: `.eproj`, `.scene`, `.prefab`, `.material`, `.sfx`, `.meta`, `.replay`, `.jsonl` (batch). Generated JSON Schemas go to `Docs/Reference/Schemas/`.

### 6.1 Project (`RollingBall.eproj`)
```json
{
	"Format": "Project", "Version": 1, "Name": "RollingBall",
	"StartScene": "Assets/Scenes/Level1.scene",
	"Window": { "Title": "Rolling Ball", "Width": 1600, "Height": 900, "VSync": true, "Fullscreen": false, "Resizable": true },
	"Simulation": { "FixedHz": 60, "MaxStepsPerFrame": 5, "Seed": 1337, "MaxEntities": 65536 },
	"Physics": {
		"Gravity": [0, -9.81, 0],
		"Layers": ["Default", "Ball", "Track", "Trigger"],
		"Collisions": [["Default", "Default"], ["Ball", "Track"], ["Ball", "Trigger"], ["Ball", "Default"]]
	},
	"Input": { "Actions": {
		"Jump": { "Type": "Button", "Bindings": ["Key.Space", "Gamepad.South"] },
		"MoveX": { "Type": "Axis", "Positive": ["Key.D", "Key.Right"], "Negative": ["Key.A", "Key.Left"], "Gamepad": "Gamepad.LeftX", "Invert": false },
		"MoveZ": { "Type": "Axis", "Positive": ["Key.W", "Key.Up"], "Negative": ["Key.S", "Key.Down"], "Gamepad": "Gamepad.LeftY", "Invert": false }
	} },
	"Rendering": { "ShadowMapSize": 2048, "SsaoHalfResolution": false },
	"Scripting": { "MemoryLimitMB": 256, "CallbackBudgetMs": 1000, "PauseOnError": true, "BlockPlayOnTypeErrors": false },
	"Export": {
		"BuildScenes": ["Assets/Scenes/Level1.scene", "Assets/Scenes/Level2.scene", "Assets/Scenes/Level3.scene"],
		"Exclude": ["Assets/Tests/**"], "Icon": "Assets/Icon.png", "Version": "1.0.0", "Company": "Yggdrasil Demos"
	},
	"Testing": {
		"Suites": [
			{ "Script": "Assets/Tests/RollingBall.test.luau", "Scene": "Assets/Scenes/Level1.scene" },
			{ "Script": "Assets/Tests/Level2.test.luau", "Scene": "Assets/Scenes/Level2.scene", "Parameters": { "Time": 61.25 } },
			{ "Script": "Assets/Tests/Autopilot/Level1.test.luau", "Scene": "Assets/Scenes/Level1.scene", "Modes": ["Editor"] }
		],
		"Replays": ["Assets/Tests/Replays/*.replay"],
		"CaseTimeoutTicks": 1800, "SuiteTickLimit": 36000
	}
}
```
`Input.Actions` is a `Map` (keys sorted on save). `Scripting.CallbackBudgetMs` (minimum 10) applies in every configuration. Dist builds raise an effective budget below 5,000 ms to 5,000 ms, so slow player machines hitch instead of disabling scripts; test runs apply per-suite overrides exactly (§11.10). `Export.Icon` (PNG, ≥ 256²), `Version` (`major.minor.patch`) and `Company` go into the exported executable's resources (§14.2). The `Testing` keys are defined in §11.10. `Physics.Layers` holds 1 to 16 names (§9.2; ADR 0014 decision 4, which amends ADR 0006 decision 11) and each `Physics.Gravity` component lies within ±1e12 m/s² (`MaxPhysicsGravity`, ADR 0014 decision 34).

### 6.2 Scene (`Level1.scene`)
```json
{
	"Format": "Scene", "Version": 1, "Name": "Level1", "Seed": 42,
	"ComponentVersions": { "Transform": 1, "MeshRenderer": 1, "RigidBody": 1, "SphereCollider": 1, "Script": 1 },
	"Entities": [
		{
			"ID": "5d1c9a7e33b04f12", "Name": "Ball", "Parent": null, "Active": true, "Tags": ["Player"],
			"Components": {
				"Transform": { "Translation": [0, 2, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] },
				"MeshRenderer": { "Mesh": "0000000000000102", "Materials": ["a41f0c2290b1d3e4"], "CastShadows": true, "ReceiveShadows": true, "Visible": true },
				"RigidBody": { "Type": "Dynamic", "Mass": 1, "Friction": 0.8, "MotionQuality": "LinearCast", "Layer": "Ball", "MaxAngularVelocity": 120 },
				"SphereCollider": { "Radius": 0.5, "Offset": [0, 0, 0], "IsTrigger": false },
				"Script": { "Script": "c0ffee0000000001", "ExecutionOrder": 0, "Fields": { "Torque": 30, "Goal": "77e1a0c4d2b95f01" } }
			}
		}
	]
}
```
(Field lists are abridged here; real files contain every field.) A prefab instance root adds `"Prefab": { "Prefab": "<handle>", "Overrides": [ { "PrefabEntityID": "<prefab-local id>", "Component": "Transform", "Field": "Scale", "Value": [1, 2, 1] } ] }` and members add `"PrefabLink": { "PrefabEntityID": "<prefab-local id>", "InstanceRoot": "<root id>" }`, both under `Components`.

### 6.3 Prefab (`Block.prefab`)
Identical entity schema with `"Format": "Prefab"`, a `"Root": "<local id>"` key, and no `Seed`.

### 6.4 Asset metadata (`Track.glb.meta`)
Every asset file, native or foreign, has a sidecar. The registry is rebuilt by scanning `.meta` files; there is no central registry file to conflict in merges.
```json
{
	"Format": "AssetMeta", "Version": 1,
	"Handle": "3c9f2e7a11d04b88", "Type": "Prefab", "Importer": "Gltf", "ImporterVersion": 1,
	"Settings": { "Scale": 1.0, "GenerateMissingTangents": true, "ImportMaterials": true, "MergeMeshes": false },
	"SubAssets": [
		{ "Key": "mesh:0:Straight", "Handle": "77e1a0c4d2b95f01", "Type": "Mesh" },
		{ "Key": "material:0:TrackPBR", "Handle": "77e1a0c4d2b95f02", "Type": "Material" },
		{ "Key": "texture:0", "Handle": "77e1a0c4d2b95f03", "Type": "Texture" }
	]
}
```
Sub-asset keys are stable glTF identities (kind, index, name), so reimporting a modified file keeps every handle.

Files that a glTF references (external `.bin` buffers, and images that have no meta of their own) get a **dependency meta** instead of an importer of their own: `{"Format": "AssetMeta", "Version": 1, "Handle": "…", "Type": "Dependency", "Owner": "<gltf handle>"}`. A dependency is never loaded on its own, never imported a second time by `TextureImporter`, and moves, is deleted and is trashed together with its owner. An image that already has a `Texture` meta is not converted: the glTF's material references that standalone texture handle instead of creating a `texture:` sub-asset, so the same texture is never imported twice.

### 6.5 Material (`Red.material`)
```json
{
	"Format": "Material", "Version": 1,
	"BaseColor": [0.9, 0.15, 0.15, 1], "BaseColorMap": null, "Metallic": 0, "Roughness": 0.45,
	"MetallicRoughnessMap": null, "NormalMap": null, "NormalScale": 1, "OcclusionMap": null, "OcclusionStrength": 1,
	"Emissive": [0, 0, 0], "EmissiveStrength": 1, "EmissiveMap": null,
	"AlphaMode": "Opaque", "AlphaCutoff": 0.5, "DoubleSided": false, "UVScale": [1, 1], "UVOffset": [0, 0]
}
```

### 6.6 Sound effect (`LineClear.sfx`)
A procedural audio source, so agents can create sound without external files (§10.3). It cooks to 48 kHz mono 16-bit PCM.
```json
{
	"Format": "SoundEffect", "Version": 1, "Seed": 7, "Volume": 0.8,
	"Layers": [
		{ "Wave": "Square", "DutyCycle": 0.5, "Notes": ["C5:0.06", "E5:0.06", "G5:0.06", "C6:0.18"],
		  "Envelope": { "Attack": 0.005, "Decay": 0.05, "Sustain": 0.6, "Release": 0.08 } },
		{ "Wave": "Noise", "Duration": 0.05, "StartFrequency": 4000, "EndFrequency": 800, "Volume": 0.3,
		  "Envelope": { "Attack": 0.0, "Decay": 0.05, "Sustain": 0.0, "Release": 0.0 } }
	]
}
```
A layer may also set `LowPass` (the one-pole low-pass of §10.3) and `Delay` (its start offset in seconds). A note is `"<Name><Octave>:<seconds>"` in equal temperament (A4 = 440 Hz) or a rest `"R:<seconds>"`, played back to back, with at most 7 digits before and 8 after the decimal point; a layer without notes is a tone of `Duration` sweeping exponentially from `StartFrequency` to `EndFrequency`, holding `EndFrequency` during its release. Each note has its own linear ADSR whose release follows the note's end (tails overlap and sum); Noise is a seeded sample-and-hold at the frequency that draws its first value at each note's start; the low-pass runs over the layer's span, from its delay to the end of its last release. Every duration is converted to whole frames once, F(s) = floor(double(s) × 48000 + 0.5), and lengths are sums of frames, so synthesis is bit-exact. Limits: 1 to 16 layers, 256 notes per layer, at least one frame per layer and at most 10 s (480,000 frames) per sound; range checks compare with the bounds rounded to float, as the type registry does. `Audio/SoundSynth.h` holds the full semantics (ADR 0015 decisions 11 and 23). The shipped `engine://Audio/LineClear` preset plays this example's square layer at `Volume` 0.6 and the sound at 0.7, because the overlapping release tails clip at the example's levels (ADR 0015 decision 24).

### 6.7 Input replay (`Opening.replay`) and automation batch (`Scaffold.jsonl`)
```json
{
	"Format": "Replay", "Version": 1,
	"Scene": { "Handle": "8a61c0d2e4f31b77", "Path": "Assets/Scenes/Level2.scene" },
	"Parameters": { "Time": 61.25 },
	"Seed": 7, "FixedHz": 60, "EngineVersion": "0.1.0+3f2a9c1", "Config": "Release",
	"Events": [
		{ "Tick": 0, "Type": "Action", "Name": "MoveZ", "Value": 1.0 },
		{ "Tick": 240, "Type": "Action", "Name": "MoveX", "Value": 0.35 },
		{ "Tick": 300, "Type": "Action", "Name": "MoveX", "Value": 0.0 },
		{ "Tick": 310, "Type": "Action", "Name": "Jump", "State": "Tap" },
		{ "Tick": 320, "Type": "Key", "Key": "Escape", "State": "Down" },
		{ "Tick": 321, "Type": "Key", "Key": "Escape", "State": "Up" }
	],
	"Expect": [ { "Tick": 900, "Luau": "return Scene.GetName() == 'Level3'" } ],
	"FinalTick": 900, "FinalStateHash": "3e0c91a2b4d5f607"
}
```
- **Header.** `Scene` names the start scene by handle (authoritative) and path (readable; a moved scene still resolves). `Parameters` are the `Scene.Load` parameters the scene starts with, so Level 2 and Level 3 replays begin exactly as they would after the previous level. `EngineVersion` and `Config` record where the replay was made. With cross-configuration determinism (§9.1), `Config` is informational, and a replay verifies in every configuration. `FinalStateHash` is the state hash at `FinalTick`. `input.replay {verify: true, strictHash: true}` compares it, and the determinism stage always does (§15.8).
- **Events** use exactly the event set of automation input (§13.6), with PascalCase keys and enum values in the file: `Action {Name, State | Value}`, `Key {Key, State}`, `MouseButton {Button, State, Position?}`, `MouseMove {Position}`, `MouseDelta {Delta}`, `Scroll {Delta}`, `GamepadButton {Gamepad, Button, State}`, `GamepadAxis {Gamepad, Axis, Value}`, `Text {Text}`. `State` is one of `Down`, `Up` and `Tap`. A recording writes the events that were actually applied at step 1 of each tick, whatever their source (§13.6).
- **Expect** entries are Luau expressions. The editor compiles them at record time and on every save, so a syntax error is a located diagnostic. When a replay is cooked (`ReplayImporter`, §7.4), each expression is precompiled to bytecode, so the Dist Runtime, which has no Luau compiler, can verify replays.

A batch file holds one `{"method": ..., "params": ...}` object per line; `{"$ref": "<line>.<jsonPath>"}` references earlier results. `Editor --headless --batch f.jsonl` stops at the first error with exit code 1.

### 6.8 Cooked binaries
Every cooked artifact (`Library/Cache`, pak entries) starts with a 32-byte little-endian header `{char Magic[4] = "ECKD"; uint16 FormatVersion; uint16 AssetType; uint32 ImporterVersion; uint32 Flags; uint64 PayloadSize; uint64 PayloadXXH64}` (`static_assert(std::endian::native == std::endian::little)`). They are read only through `BinaryReader`, which bounds-checks every read, returns `Result`, never trusts sizes or offsets, and is fuzzed (§15.2). Payloads:
- mesh: submeshes `{IndexOffset, IndexCount, MaterialSlot, AABB}`, slot names, vertices `{pos f32×3, normal f32×3, tangent f32×4, uv0 f32×2}`, `uint32` indices (CPU copy kept for mesh colliders and CPU picking);
- texture: format, width, height, mip count, mip data;
- environment (FormatVersion 1): four `uint32` sizes, the nine SH coefficients, the skybox cube texels (RGBA16F with mips), the specular cube texels (256² × 7 mips); sizes are checked before allocation (ADR 0013 decision 9);
- font: R8 SDF atlas and glyph metrics;
- script: kind (`Behaviour`, `Module`, `TestSuite`), bytecode, field schema, `require` list (resolved handles), source map;
- replay: header, events in binary form, and one bytecode chunk per `Expect`;
- audio clip (v1): `uint8 Encoding` (Pcm16, Wav, Flac, Mp3, Vorbis), `uint8 Stream`, `uint16 Reserved`, `uint32 SampleRate`, `uint32 ChannelCount`, `uint64 FrameCount`, `uint64 ByteCount`, then the original encoded bytes or the synthesized 16-bit PCM (ADR 0015 decision 9);
- scene, prefab, material, project: minified canonical JSON.

---

## 7. Asset system

### 7.1 Types, handles, references
`AssetType` is one of `Scene, Prefab, Mesh, Material, Texture, Environment, AudioClip, Script, Font, Replay`. `AssetHandle` is a UUID. Dependency metas (§6.4) have no `AssetType` of their own; they cannot be loaded. A glTF source has no type of its own: its `.meta` declares the prefab as the main asset and lists mesh, material and texture sub-assets with handles `Hash64(sourceHandle, key)`.

Asset references accepted by scripts and automation (`AssetRef` syntax): a 16-hex handle, a project path (`Assets/Materials/Red.material`), a sub-asset path (`Assets/Models/Track.glb#mesh:0:Straight`) or an engine path. Responses always expand references to `{id, path, type}`.

**Built-in assets** (procedural or shipped, fixed handles, `Resources/EngineAssets.json`):
- Meshes: `engine://Meshes/{Cube, Sphere, Plane, Quad, Cylinder, Capsule, Cone}` (deterministic generation, analytic tangents, unit size).
- Materials: `engine://Materials/{Default, Error}`. Textures: `engine://Textures/{White, Black, FlatNormal, Checker, Missing}`, and `engine://Textures/BlueNoise` (`0x0186`, the tonemap's dither, a Generated entry, §8.4).
- Fonts: `engine://Fonts/Default` (Inter Regular, SIL OFL). Environments: `engine://Environments/{Studio, Sky}` (Poly Haven CC0 `studio_small_09`, `kloofendal_48d_partly_cloudy_puresky`, 1k).
- Sound effects: `engine://Audio/{Click, Blip, Coin, Jump, Hit, Explosion, PowerUp, LineClear, Win, Lose}` (`.sfx` presets, handles `0x0241` to `0x024a`), and `engine://Audio/Silence` (`0x024b`, procedural: 0.1 s of 48 kHz mono silence, the AudioClip placeholder of §7.2; ADR 0015 decision 9).

The demos therefore need no external art or audio.

### 7.2 AssetManager
```cpp
class AssetManager   // interface in Asset; EditorAssetManager (AssetPipeline: source → cook → load) | RuntimeAssetManager (Asset: paks)
{
public:
	virtual Result<AssetRef<Asset>> Load(AssetHandle handle) = 0;                 // synchronous
	virtual JobHandle<Result<AssetRef<Asset>>> LoadAsync(AssetHandle handle) = 0;  // decode on jobs, publish on main thread
	virtual AssetState GetState(AssetHandle handle) const = 0;                    // Unloaded | Loading | Loaded | Failed
	virtual const AssetMetadata* GetMetadata(AssetHandle handle) const = 0;
	virtual std::optional<AssetHandle> Resolve(std::string_view reference) const = 0;
	template<typename T> AssetRef<T> GetOrPlaceholder(AssetHandle handle);          // logs once, records diagnostic
	uint64_t GetVersion(AssetHandle handle) const;                                 // bumps on hot reload
	std::span<const AssetDiagnostic> GetDiagnostics() const;
	void WaitIdle();                                                               // screenshots and tests
};
```
- Loaded assets are immutable `AssetRef<T>` (`std::shared_ptr<const T>`), safe to share across threads. Hot reload replaces the registry entry and bumps the version; old references stay valid until released. The renderer's `GpuResourceCache` keys GPU mirrors by `(handle, version)`.
- **One load path.** `EditorAssetManager` (in `AssetPipeline`, injected into `EngineContext` as `AssetManager&`, §3) imports a source into `Library/Cache`, then loads the **cooked bytes** with the same `IAssetLoader` the Runtime uses on pak entries. Every editor session exercises the shipping loaders.
- **Failure policy.** A missing or failed asset never yields null: callers get a typed placeholder (Missing checker texture, Error material, unit cube mesh, silent clip, default font), the error is logged once, and an `AssetDiagnostic` is recorded (visible in the editor, `_meta`, `project.validate`). Export refuses to run while any diagnostic has severity Error.

### 7.3 Registry and integrity
`AssetRegistry` is built at project open by scanning `Assets/**.meta`. A source without a `.meta` gets one; a moved pair (file + `.meta`) keeps its handle. Scan diagnostics, auto-fixable through the editor and `project.validate {fix}`: duplicate handles (copy-pasted `.meta`), orphan `.meta`, type/importer mismatch, case mismatch, and a dependency whose owner is missing. `project.refreshAssets` runs the scan synchronously, and every automation call that takes a path refreshes first, so a just-written file never races the watcher.

### 7.4 Importers (`AssetPipeline`, editor-only)
`IAssetImporter {GetId(); GetVersion(); CanImport(extension); Result<ImportResult> Import(const ImportContext&, const AssetMetadata&);}`. Importers are **pure functions of (source bytes, settings, importer version)**: no timestamps, no pointers, no unordered iteration in output. A test imports every fixture twice and compares hashes.

| Importer | Input | Behaviour |
|---|---|---|
| `TextureImporter` | PNG, JPEG, TGA, BMP (stb_image from memory) | sRGB or linear by usage setting; CPU mips with `stb_image_resize2` (sRGB-correct; normal maps renormalized per mip); RGBA8/RGBA8_SRGB; > 16384 px or decode failure is an error |
| `GltfImporter` | `.gltf`, `.glb` (cgltf with VFS file callbacks; `cgltf_validate`) | one submesh per triangle primitive (strips/fans converted; points/lines skipped with a warning); positions, normals (generated if missing), UV0, tangents (from file; else MikkTSpace once approved, else Lengyel per-vertex tangents with `ASSET_TANGENTS_APPROXIMATED`, Appendix C); uint32 indices; materials (metallic-roughness, `KHR_materials_emissive_strength`, `KHR_texture_transform` scale/offset); embedded, data-URI and relative external buffers and images (dependency closure, §6.4); a prefab mirroring the node hierarchy; required unsupported extensions (Draco, meshopt, BasisU) rejected with a precise message; URIs containing `..`, absolute paths, drive letters or a scheme other than `data:` rejected with `ImportFailed` and the offending URI; textures bound to `TEXCOORD_1` (e.g. occlusion) are ignored with `ASSET_UNSUPPORTED_UV_SET`, and `COLOR_0` is ignored with `ASSET_VERTEX_COLORS_IGNORED` (the vertex format has only UV0 and no colour); validation of index bounds, NaN/Inf, degenerate triangles |
| `MaterialImporter` | `.material` | validate + migrate; cooked canonical JSON |
| `SceneImporter`, `PrefabImporter` (two importers) | `.scene`, `.prefab` | strict load (structural pre-validation, §6) + migrate; cooked canonical JSON |
| `EnvironmentImporter` | Radiance `.hdr` (`stbi_loadf`), at most 8,192 texels wide | GPU bake through `IEnvironmentBaker` (§8.6), settings `EnvironmentImportSettings {ClampLuminance, MaxLuminance}`; `.exr` rejected with "download the .hdr variant from Poly Haven". Without a GPU (`--renderer none`) the `EditorAssetManager` serves the import from any cooked artifact with the same cache key (§7.5): under the asset's own handle, under another handle of the project cache, then in the engine cooked cache; otherwise the import is `Unsupported` with the hint "start the editor with a GPU once to bake this environment" (ADR 0013 decision 9) |
| `AudioImporter` | WAV, FLAC, MP3, Ogg Vorbis (`.ogg` through stb_vorbis; Ogg Opus refused with a conversion hint) | the format is detected from the bytes; every frame is probed and validated through `Audio/AudioDecoder` (non-finite samples, more than two channels and sample rates outside 8 to 192 kHz are refused); cooked as original bytes; setting `Stream: AudioStreamMode` (`Auto` streams clips above 10 s, `Stream`, `Decode`), since the `.meta` is written before the import knows the length (ADR 0015 decisions 2, 10 and 28) |
| `SoundEffectImporter` | `.sfx` | deterministic synthesis (`Audio/SoundSynth`, §6.6) → 48 kHz mono 16-bit PCM; synthesis and read errors are `ImportFailed` |
| `FontImporter` | TTF, OTF | SDF atlas (ASCII + Latin-1, 48 px, spread 8) via `imstb_truetype.h` from the vendored Dear ImGui (stb_truetype v1.26, compiled `STBTT_STATIC` in this TU only) |
| `ScriptImporter` | `.luau` | `luau_compile` (error decoded from the bytecode with line and column), classification (`Behaviour`, `Module`, `TestSuite`) and field-schema extraction in a throwaway load-time VM with a shared time budget (§11.2), `require` edges recorded in `AssetDependencyGraph`, type diagnostics from the registered `IScriptDiagnosticsProvider` (the EditorCore type checker; none in tools without it) |
| `ReplayImporter` | `.replay` | strict load; the scene is resolved by handle; every `Expect` compiled to bytecode; cooked `Replay` payload (§6.8) |

### 7.5 Cache, cooking, hot reload
- Cache key: `XXH64(source bytes ‖ importer id ‖ importer version ‖ canonical settings JSON ‖ engine cook version)`; artifacts at `Library/Cache/<handle>/<key>.bin`. An entry whose header or hash fails validation is discarded and rebuilt, never trusted.
- **Engine cooked cache.** Engine resources (built-in HDRI bakes, the blue-noise texture, the SDF font atlas, the ten sound-effect presets) are cooked with the same keys into `bin/EngineCache/<handle>/<key>.bin` (shared by all configurations; bakes do not depend on the build configuration), mounted as `enginecache://`. It is populated by `Editor --headless --bake-engine-assets`, which `CI.py` runs after the build stage on a GPU, and by any GPU-enabled editor on first use. `--renderer none` editors and exports reuse it and never need a GPU, as long as the bakes exist. An export without a GPU whose engine cache holds no bake of a built-in environment leaves that environment out of `Engine.pak` with a warning, and fails only when an included asset references it (a reference issue with the GPU hint; ADR 0013 decision 9). `CI.py`'s bake stage runs the editor with `--renderer vulkan`; without a device it fails, and with `--gpu-optional` it reruns with `--renderer none` and ends as a warning (ADR 0013 decision 21).
- Dependencies (material → textures, prefab → meshes and materials, glTF → dependency files, script → required scripts) are recorded in `AssetDependencyGraph`; reimporting a dependency invalidates its dependents, so changing `Board.luau` re-extracts and recompiles `Game.luau`.
- **Hot reload** uses a **polling** watcher (every 500 ms on a job: size + mtime, confirmed by content hash, 200 ms debounce). Polling replaces three native APIs with one deterministic implementation that tests drive through `MemoryMount`. A change reimports on a job and swaps on the main thread at the start of the next frame; dependents are notified; a failed reimport keeps the last good version and raises a diagnostic. Shaders reload the same way in dev builds (§8.12); scripts per §11.8.
- **Race rules** (each with a test):
	1. *No echo.* Every editor write under the project (`script.write`, `scene.save`, `asset.setProperties`, imports, `project.upgrade`) goes through `AssetWriter` (autosave writes under the unwatched `Library/`). `AssetWriter` writes atomically, updates the watcher's known `(size, mtime, hash)` for that path and schedules the reimport itself, so the watcher never sees the write as an external change.
	   Its optional main-thread mutation guard runs before any VFS access in Write, Remove, Move and CreateDirectories, including backups and parent creation. A rejected operation returns the guard error without watcher or listener effects. EditorContext binds this guard to its command-origin mutation policy, covering implicit metadata generation as well as explicit writes. Human/passive work and disposable dry-run overlays remain allowed; the writer's dry-run flag alone never bypasses the guard. The borrowed editor callback is cleared after the asset manager drains and before editor member destruction (ADR 0018).
	2. *Newest wins.* Import jobs carry `(handle, contentHash, generation)`. The main thread drops a completion whose generation is older than the newest one requested for that handle, so a slow job can never overwrite a newer result.
	3. *Open documents are never reloaded silently.* If the open scene, or a prefab used by it, changes on disk, the editor raises `SceneChangedOnDisk {path, dirty}` (event, `_meta` flag, banner) and leaves the in-memory scene alone. `scene.open {path, reload: true}` (plus `discardChanges` when dirty) adopts the disk version.
	4. *Deterministic sessions are frozen.* While lockstep, a replay, a recording or a test run is active, script and asset reloads are queued and applied when the session ends. A reload during ordinary Play marks the session `modified` (`play.state`), which invalidates any recording in progress.

### 7.6 Packaging for export
Export cooks **every registered asset under `Assets/`** except `Export.Exclude` globs (default `Assets/Tests/**`), plus the engine resources referenced by built-in handles. A **testing export** (`project.export {testing: true}`, `Editor --export … --testing`) writes `"Testing": true` into the manifest and always includes `Assets/Tests/**`, whatever `Export.Exclude` says, together with every suite script, every scene referenced by `Testing.Suites`, and every replay matched by `Testing.Replays`, which are cooked to `Replay` assets with precompiled expectations. Including everything rules out "missing in export" bugs from string-path lookups (`Assets.Load("…")`). Paks are deterministic (same inputs → byte-identical files, tested). Reachability pruning is deferred.

`Engine.pak` contains the SPIR-V of the **target** configuration (`bin/<TargetConfig>-<sys>-<arch>/Shaders`; Dist SPIR-V is compiled without `-g`), and the File and Generated built-ins taken from the engine cooked cache: the Default font, the blue-noise texture, the ten sound-effect presets and the built-in environments (§7.5 for a missing bake). Procedural built-ins (meshes, materials, the White to Missing textures, the silent clip) are generated by the Runtime's asset manager and are not in the pak. Building the target configuration's Runtime also builds its shaders (§2.2), so the two always exist together.

---

## 8. Renderer

### 8.1 Device and swapchain (`Graphics`)
- **Loader.** Exactly one TU (`Graphics/VulkanDispatch.cpp`) contains `VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE`. Init order (from `Vendor/NVRHI/VENDOR.md`), split across the two init levels of §4.1. Process level: `vk::detail::DynamicLoader` (kept alive until after the last `vkDestroyInstance`) → `VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr)` → `glfwInitVulkanLoader(vkGetInstanceProcAddr)` → `glfwInit()`. Per context: instance → `init(instance)` → device → `init(device)` → `nvrhi::vulkan::createDevice`. `DeviceDesc::vulkanLibraryName` is ignored in static mode and is not used. No loader → exit 3 with a readable message (and a message box when windowed).
- **Instance.** `apiVersion` = 1.4 when `vk::enumerateInstanceVersion()` reports it, otherwise 1.3; below 1.3 is an `InitFailed` error. `--vulkan-api=1.3` caps it, and the GPU suite runs under both caps so both paths are exercised. macOS adds `VK_KHR_portability_enumeration` with `eEnumeratePortabilityKHR`. Validation (`VK_LAYER_KHRONOS_validation` + `VK_EXT_debug_utils`) per the configuration table (§2.2). Headless creates no surface extensions.
- **Vulkan 1.4 policy.** The engine requires only the 1.3 feature set (NVRHI's Vulkan backend needs `dynamicRendering`, `synchronization2` and `timelineSemaphore`) and requests 1.4 where available, passing the enabled extension lists to NVRHI so it can use what it detects. Each 1.4 capability the engine uses is an optional, feature-gated path with a 1.3 fallback, and both paths are tested under both API caps (`--vulkan-api=1.3` forces the fallback). The paths in v1:
	1. **Host image copy for immutable texture uploads** (`VkPhysicalDeviceVulkan14Features::hostImageCopy`, with `VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT` checked per format). `GpuResourceCache` creates asset textures and baked environment cubes as native `VkImage`s with `VK_IMAGE_USAGE_HOST_TRANSFER_BIT`, in device-local memory from a small engine block allocator. It writes the mips with `vkCopyMemoryToImage` and sets the final layout with `vkTransitionImageLayout`, with no staging buffer and no command list, then wraps the image with `createHandleForNativeTexture` as a `keepInitialState` texture whose initial state is `ShaderResource` (NVRHI refuses to copy out of a texture in a permanent state, which readback and the path test need). NVRHI does not own such an image, so the engine destroys the `VkImage` and its memory through a deferred-release queue keyed by the last submission ID, which mirrors NVRHI's own deferred destruction; `GpuResourceTracker` counts these images too. The fallback is NVRHI's `writeTexture` through a command list. The test "Texture upload: host-copy and staging paths read back identically" runs for every texture format the engine uses; it reports a skip with the reason when the device lacks the feature.
	2. Nothing else in v1. NVRHI does not use push descriptors (`vulkan-backend.h`) and exposes no hook for line-rasterization state, so those 1.4 features would need NVRHI changes. Adopting another 1.4 feature follows the same gated-path-plus-dual-cap-test rule.
- **Device selection** is a pure function `ScoreDevice(const DeviceCandidate&) -> std::optional<int>` over a plain struct, unit-tested without a GPU. Required: API ≥ 1.3, `dynamicRendering`, `synchronization2`, `timelineSemaphore`, `samplerAnisotropy`, `imageCubeArray`, `shaderStorageImageExtendedFormats`, and `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` (from `vkGetPhysicalDeviceFormatProperties`) for every storage format the passes write: `R16_SFLOAT`, `R8_UNORM`, `RG16_SFLOAT`, `RGBA16_SFLOAT`, `RGBA8_UNORM`, `R32_UINT`; plus a graphics queue (with present support when windowed). `B10G11R11_UFLOAT` storage is optional: without it, Bloom uses `RGBA16_SFLOAT` (a pipeline permutation, logged once). Optional: `depthClamp` (shadow pancaking; fallback extends the light near plane), `fillModeNonSolid` (wireframe debug view), `hostImageCopy` (above), `VK_EXT_device_fault` (crash reports), `VK_KHR_portability_subset` (enabled when exposed). Ranking: discrete > integrated > CPU. `--gpu=<index|substring>` or `ENGINE_GPU` overrides.
- **NVRHI.** `nvrhi::vulkan::createDevice(DeviceDesc{...})`, wrapped with `nvrhi::validation::createValidationLayer` when validation is on. `DeviceDesc::errorCB` and the debug messenger route to the `Engine` logger and increment `GpuDiagnostics`' error and warning counts, which every GPU test fixture requires to be zero. The loader's general messages ("Loader Message": a third-party implicit layer, a missing driver) describe the installation and are logged as warnings without counting.
- **Swapchain.** Our own code: `glfwCreateWindowSurface`, `VkSwapchainKHR`, images wrapped with `createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, …)`. Format `B8G8R8A8_UNORM`, else `R8G8B8A8_UNORM` (UNORM holding sRGB-encoded values, §8.9). Present mode FIFO with VSync, else MAILBOX, else IMMEDIATE. **Synchronization: one acquire semaphore per frame in flight and one render-complete (present) semaphore per swapchain image**, attached through `queueWaitForSemaphore` and `queueSignalSemaphore` to the frame's last `executeCommandLists`, then present. **All swapchain calls use the C entry points through the dispatcher** (`VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR`, `vkQueuePresentKHR`, …) and switch on the returned `VkResult`. vulkan.hpp's enhanced-mode wrappers would throw `vk::OutOfDateKHRError`, which the state machine treats as an ordinary state: `VK_SUBOPTIMAL_KHR` and `VK_ERROR_OUT_OF_DATE_KHR` lead to recreation, `VK_ERROR_SURFACE_LOST_KHR` to surface and swapchain recreation, and `VK_ERROR_DEVICE_LOST` to `FatalError(DeviceLost)`. Recreation on those results or on resize is a tested state machine: every resize and every suboptimal or out-of-date result is coalesced into one recreation at the next acquire, which skips that frame; a 0×0 or iconified (minimized) window skips rendering, releases the swapchain until it is restored and throttles the loop (§4.2); viewport render targets (the editor viewport, M10) are debounced for 2 frames.
- **Frame pacing never waits unboundedly.** `nvrhi::IDevice::waitEventQuery` waits with an infinite timeout and `assert`s the result, and `NDEBUG` is set only in Dist, so the engine never calls it. Each frame in flight records the submission ID returned by `executeCommandLists`. `BeginFrame` first tries `pollEventQuery`, then waits on the queue's timeline semaphore (`getQueueSemaphore`) with `vkWaitSemaphores` in 100 ms slices. `VK_TIMEOUT` retries until a 10 s budget is spent, which then becomes `FatalError(GpuHang)`, and `VK_ERROR_DEVICE_LOST` becomes `FatalError(DeviceLost)`. `waitForIdle` (shutdown, resize) checks its boolean result the same way.
- **Device loss surfaces three ways, and all are handled.** It can come as a `VkResult` from first-party calls (above); as NVRHI's message callback reporting "Device Removed!" (NVRHI catches `vk::DeviceLostError` inside `Queue::submit` itself), which sets `GpuDiagnostics::DeviceLost`, checked after every submit; or as a `vk::SystemError` thrown out of other NVRHI internals, caught at the frame boundary (§4.6). Each path ends in `FatalError`, with the autosave written first.
- **Headless.** No surface, no swapchain; the final image lands in an `OffscreenTarget`, read by `Readback::ReadTexture(texture) -> Result<Image>` (staging texture with `CpuAccessMode::Read`, `copyTexture`, `mapStagingTexture`).

`GraphicsSpecification::DisableDepthClamp` restricts both the effective capability and enabled Vulkan feature to `hardwareSupport && !DisableDepthClamp`. It defaults to false, applies in every configuration and has no CLI option. This permits deterministic testing of the extended-near shadow fallback on capable hardware (§8.7, §15.3; ADR 0017).

### 8.2 Frame structure and the render snapshot
`Scene/RenderExtraction` builds a plain `RenderSnapshot` per view; the renderer never sees the ECS, so it is tested with synthetic snapshots.
```cpp
struct RenderSnapshot
{
	bool HasCamera;                          // false: no camera (clear to the default colour, screen texts only)
	CameraData Camera;                       // view, projection (reverse-Z), position, viewport, exposure
	std::vector<MeshDrawItem> Meshes;         // world matrix, mesh handle+version, material per submesh, flags, PickID
	std::vector<LightData> Lights;            // directional, point, spot (≤ 256 visible after CPU culling)
	EnvironmentData Environment;              // handle, intensity, rotation, skybox flags, fallback
	PostProcessSettings Post;
	std::vector<TextItem> Texts;
	DebugDrawList DebugDraw;
	std::vector<UUID> PickTable;              // PickID - 1 → entity UUID (snapshot-local; M9)
	RenderViewFlags Flags;                    // EditorOverlays, Grid, Selection, Colliders (M9)
	RenderDebugView DebugView;                // Lit, except a viewport.screenshot with debugView
};
class SceneRenderer
{
public:
	SceneRenderer(GraphicsDevice& device, ShaderLibrary& shaders, const SceneRendererSpecification& specification);
	void Resize(uint32_t width, uint32_t height);
	void Render(nvrhi::ICommandList& commandList, const RenderSnapshot& snapshot, RenderTarget& target);
	nvrhi::ITexture* GetFinalTexture() const;   // RGBA8_UNORM, display-encoded
};
```
`DebugView` is a member of its own rather than a view flag, because a screenshot sets it independently of the view's flags; the pick table and the view flags are defined by M9 (ADR 0017). `HasCamera` is false for a view without a camera (a game view without a primary camera), which clears to the default colour and draws only screen texts. `TextItem` carries a text's fields as extracted (space, anchor, pivot, offset, alignment, billboard, font, colour, size, the rendered world matrix and the entity); a World text's em is `Size / WorldTextPixelsPerMetre` (100) metres before the entity's scale. `DebugDraw` is empty after extraction and its producers append to it (§8.10; ADR 0013 decisions 5 and 6).

A frame: `BeginFrame` (wait on the event query of frame N-2, acquire or pick the offscreen target) → record the scene command list → record ImGui or blit → execute → present → `runGarbageCollection()`. The pass list is fixed (no render graph): NVRHI's automatic state tracking places barriers, and immutable resources (IBL cubes, LUTs, material textures) are `keepInitialState` textures in `ShaderResource`, so every command list starts and ends with them there while copies out of them (readback in tests) stay legal. Pass targets come from `RenderTargetPool` keyed by `TextureDesc`. Each pass is wrapped in `beginMarker` and a pooled timer query (`GpuProfiler`).

### 8.3 Pass list
Depth is **reverse-Z** everywhere: `D32_FLOAT`, clear 0, compare `GreaterOrEqual`, infinite far plane for perspective (`m[0][0] = f/aspect; m[1][1] = f; m[2][3] = -1; m[3][2] = zNear`). Orthographic cameras use a finite reverse-Z range, `d = (zFar + z_view) / (zFar − zNear)` (1 at the near plane, 0 at the far plane, linear in view depth). Every pass that reconstructs view-space data takes the projection type from `ViewConstants` (`ProjectionKind`, `OrthoHalfExtents`, `Near`, `Far`), never assuming perspective. Linear depth is `zNear / d` for perspective and `zFar − d · (zFar − zNear)` for orthographic. The view ray is `normalize(ndc.xy · tanHalfFov · aspect, −1)` for perspective, while orthographic uses the constant direction (0, 0, −1) and the origin `(ndc.xy · OrthoHalfExtents, 0)`. CPU tests cover both mappings and both reconstructions. Projections never flip Y: NVRHI's Vulkan backend gives every viewport a negative height (`VKViewportWithDXCoords`), so clip space has +Y up as in D3D, and pipelines use `frontCounterClockwise = true` to keep glTF's counter-clockwise front faces; a GPU test renders a CCW triangle with back-face culling and checks that it is visible and upright.

| # | Pass | Type | Output (format) | Notes |
|---|---|---|---|---|
| 1 | Prepare | CPU | constant/structured buffer uploads | frustum culling against submesh AABBs; opaque sorted by pipeline → material → mesh; transparent back-to-front, ties by UUID; light list ≤ 256 |
| 2 | Directional CSM | graphics | `ShadowCascades` D32_FLOAT 2048² × 4 array (`Rendering.ShadowMapSize`) | stable cascades (bounding-sphere fit + texel snapping), practical split λ, depth clamp pancaking, slope-scaled bias × cascade texel size |
| 3 | Spot shadow atlas | graphics | `ShadowAtlas` D32_FLOAT 4096², ≤ 8 tiles of 1024² | shadowed spot lights by screen importance; over budget → unshadowed + one-time warning |
| 4 | Depth/normal prepass | graphics | `SceneDepth` D32_FLOAT; `SceneNormals` RG16_FLOAT (octahedral, view space, normal-mapped); `EntityId` R32_UINT (editor views only) | alpha-mask materials discard here |
| 5 | Depth pyramid | compute | `ViewDepth` R16_FLOAT, 5 mips (linear view depth, projection-aware) | GTAO input |
| 6 | GTAO + denoise | compute | `AmbientOcclusion` R8_UNORM (full res; half on Low or `SsaoHalfResolution`) | §8.8 |
| 7 | Forward opaque | graphics | `SceneColor` RGBA16_FLOAT | depth `GreaterOrEqual`, writes off; PBR + IBL + shadows + AO |
| 8 | Skybox | graphics | `SceneColor` | pixels at depth 0; skybox cube at `SkyboxBlur` LOD |
| 9 | Forward transparent | graphics | `SceneColor` | sorted, alpha blend |
| 10 | Bloom | compute | `Bloom` R11G11B10_FLOAT (RGBA16_FLOAT when unsupported as storage, §8.1), 6-mip chain from half res | 13-tap downsample, Karis average on the first mip, 3×3 tent upsample, `lerp(scene, bloom, BloomIntensity)` |
| 11 | Tonemap + encode | compute | `LdrColor` RGBA8_UNORM | exposure 2^EV, tonemapper, sRGB OETF in shader, blue-noise triangular dither (±1 LSB) |
| 12 | FXAA 3.11 | compute | `LdrColor` (ping-pong) | optional |
| 13 | Overlays | graphics | `LdrColor` (+ `SceneDepth`, tested without writes) | editor grid, icons, debug lines (depth-tested and on-top pipelines), selection outline from `SelectionMask` R8_UNORM |
| 14 | Text | graphics | `LdrColor` | SDF text: world (depth-tested) then screen |
| 15 | ImGui or blit | graphics | swapchain BGRA8_UNORM or `OffscreenTarget` | editor UI; Runtime non-Dist F1 overlay |

There are no temporal effects (no TAA, no temporal noise): frame N of a deterministic scene is exactly reproducible on a given device, which golden tests rely on.

The overlay passes (13, 14) attach `SceneDepth` as an ordinary depth attachment with depth writes disabled, not as a read-only one: NVRHI stores a read-only depth attachment with `VK_ATTACHMENT_STORE_OP_STORE`, which synchronization validation counts as a write, and keeping `SceneDepth` in `DepthWrite` from the prepass to the overlays needs no transition (ADR 0013 decision 20). The forward passes test against the prepass depth without writing it, which needs bit-identical clip positions across the pipelines. Slang 2026.8 cannot decorate the position `Invariant`, so `Scene.slang` emits the vertex shader's two matrix products as `OpVectorTimesMatrix` decorated `NoContraction`, which no driver may fuse differently per pipeline (ADR 0013 decision 27).

### 8.4 Binding model and shared structs
Shaders use HLSL register syntax compiled by Slang with `-fvk-t-shift 0 all -fvk-s-shift 128 all -fvk-b-shift 256 all -fvk-u-shift 384 all`, matching NVRHI's default `VulkanBindingOffsets` (SRV 0, sampler 128, CBV 256, UAV 384). Every `nvrhi::BindingLayoutDesc` sets `registerSpace = N` with `registerSpaceIsDescriptorSet = true`, so `spaceN` is descriptor set N.

| Space (set) | Frequency | Contents |
|---|---|---|
| 0 | per view | `b0 ViewConstants` (incl. projection kind and ortho extents, §8.3), `b1 ShadowConstants` (cascade matrices, splits, texel world sizes, penumbra scales), `b2 EnvironmentConstants` (SH9, intensity, rotation); `t0 Lights` (StructuredBuffer), `t1 ShadowCascades`, `t2 ShadowAtlas`, `t3 EnvSpecular` (cube), `t4 EnvSkybox` (cube), `t5 BRDFLut`, `t6 AmbientOcclusion`, `t7 BlueNoise`; `s0 LinearClamp`, `s1 ShadowCompare` (`GreaterOrEqual`), `s2 AnisoWrap`, `s3 PointClamp`; plus the layout's `PushConstants` item (80 B) for the per-draw constants below |
| 1 | per material | `b0 MaterialConstants`; `t0 BaseColorMap`, `t1 MetallicRoughnessMap`, `t2 NormalMap`, `t3 OcclusionMap`, `t4 EmissiveMap`; one cached `BindingSet` per (material, version); empty slots bind White/FlatNormal/Black |
| push | per draw | `DrawConstants { float4x4 World; uint EntityId; uint Flags; uint2 Padding; }` (80 B; normal matrix from the cofactor of `World` in the shader). NVRHI requires push constants to be declared as a `BindingLayoutItem::PushConstants` in a binding layout; it lives in the set-0 layout of every graphics pipeline that draws meshes (and the matching set-0 binding sets), and Slang declares it `[[vk::push_constant]] ConstantBuffer<DrawConstants> Draw;`. Values are set per draw with `ICommandList::setPushConstants` |

Structs shared between C++ and Slang live in `Resources/Shaders/Shared/*.h` (the common subset, `ENGINE_SHADER` guards), included by both sides (`Resources/Shaders` is an include path of Engine). C++ adds `static_assert(sizeof(...))`; the test `Shaders.SharedStructsMatchReflection` compares every member offset in slangc's `-reflection-json` with `offsetof`. A GPU test checks the matrix convention (column-major glm, `-matrix-layout-column-major`, `mul(M, v)`) by transforming known vectors. Slang names: types and functions PascalCase, resources PascalCase without prefix, constant buffers end in `Constants`, locals camelCase.

Every `RWTexture*` declares its storage format explicitly with `[vk::image_format("r11f_g11f_b10f" | "r16f" | "r8" | "rg16f" | "rgba16f" | "rgba8" | "r32ui")]`, so the SPIR-V never relies on `shaderStorageImageWriteWithoutFormat`. `Shaders.LayoutsMatchReflection` also checks the reflected image format against the texture format the C++ pass creates.

**Blue noise** (`t7`, 64² R8) is generated in code: a seeded void-and-cluster implementation in `Renderer/BlueNoise.cpp`, run by `--bake-engine-assets` into the engine cooked cache (§7.5) and shipped in `Engine.pak`. A CPU test compares the hash of the output with a committed value, so no third-party texture or license is involved.

**Shared layouts and per-view binding sets** (ADR 0013 decisions 7, 20, 21 and 27). NVRHI accepts a binding set only for the pipeline's own layout objects, so `PipelineFactory::CreateBindingLayouts` creates each set's layout once and `GraphicsPipelineSpecification::SharedBindingLayouts` hands it to every pipeline that binds that set (the prepass shares set 1 with the forward pass for its Mask variants). The passes every view of a device shares (skybox, bloom, tonemap, FXAA, debug lines, text) keep no binding sets of their own: each record takes the view's `PassBindingCache`, which finds a set by its layout and exact `BindingSetDesc` and holds it; a `SceneRenderer` releases at the end of every render what that render did not bind, so views never evict each other's sets and no pass keeps a resized or destroyed view's targets alive. Material binding sets are cached per view by (handle, generation) the same way. M8 binds this part of set 0: `b0`, `b2`, `t0`, `t3` to `t5`, `t7`, `s0`, `s2`, `s3` and the push constants; M9 adds `b1`, `t1`, `t2`, `t6` and `s1`. `ViewConstants` ends with `LightCount` (400 bytes); `t0 Lights` is a structured buffer of `Shared/ShaderLight.h` (std430); `DrawConstants::Flags` holds `DrawFlagMirrored`; `MaterialConstants` holds the §8.5 factors and `Flags` (`MaterialFlagEmissiveMap`); the debug view is the specialization constant `SceneDebugViewConstantId`; the post and bake passes take push constants (`TonemapConstants`, `BloomConstants`, `EnvironmentBakeConstants`). `SharedStructsMatchReflection` covers every constant-buffer and push-constant struct and the element structs of structured buffers (`ShaderLight`).

### 8.5 PBR material model
glTF 2.0 metallic-roughness: base colour (sRGB texture × factor), metallic-roughness (G = roughness, B = metallic), tangent-space normal map with scale, occlusion (R) with strength, emissive (sRGB) × `EmissiveStrength`, alpha modes Opaque/Mask/Blend, double-sided, UV scale and offset. BRDF: GGX NDF, height-correlated Smith visibility, Schlick Fresnel, Lambert diffuse, multi-scatter energy compensation from the DFG LUT, perceptual roughness clamped to ≥ 0.045, geometric specular anti-aliasing from screen-space normal variance. Lights use artist units: unitless intensity multiplied by colour, windowed inverse-square falloff up to `Range` (point/spot), smoothstep cone attenuation, `SourceRadius` widening roughness (representative point) and driving PCSS. Exposure is manual (2^`ExposureEV`); with artist units HDR values stay well inside FP16, so no pre-exposure is needed. The mesh pipelines created at startup are the prepass and the forward opaque pass for {Opaque, Mask} × {CullBack, CullFront, CullNone} and the transparent pass (Blend) for the three cull modes: a mirroring world matrix (negative determinant) draws with CullFront and `DrawFlagMirrored`, which flips normals and `SV_IsFrontFace`, and double-sided materials draw with CullNone. With the passes' own pipelines that makes 26 at startup (`SceneRendererPipelines::StartupPipelineCount`). The debug view (M8: Lit, Albedo, Normals, Roughness, Metallic, Emissive; M9 adds AO, ShadowCascades, Overdraw) is a Vulkan specialization constant through `IDevice::createShaderSpecialization`; a non-Lit view's nine forward variants are created the first time a snapshot asks for it (`SceneRendererPipelines::EnsureDebugView`) and kept, so a debugging path does not cost every device, fixture and exported game 71 pipelines (an exception to §8.12's startup rule). A debug view's post chain is fixed: no skybox, bloom, FXAA or dither, exposure 1, Linear, the OETF only for Albedo and Emissive, so a data view stores v as round(255 v). Both pipeline counts are logged and asserted by a test (ADR 0013 decisions 7 and 12). Mask materials drop fragments by writing `SV_Coverage = 0` (Slang compiles `discard` to `OpDemoteToHelperInvocation`, whose feature `GraphicsDevice` does not enable); `MaterialFlagEmissiveMap` tells the shader whether an emissive map multiplies the factor; a light of radiance C·I delivers π·C·I illuminance to a surface facing it, so a white Lambertian surface lit head-on by a directional light of intensity 1 has radiance 1; specular anti-aliasing follows Tokuyoshi and Kaplanyan with Filament's variance and threshold, diffuse occlusion Jimenez's multi-bounce fit and specular occlusion Lagarde's, with horizon occlusion for normal maps (ADR 0013 decision 21).

### 8.6 IBL pre-filtering (Poly Haven HDRIs)
`EnvironmentBaker` (Renderer, implements `IEnvironmentBaker`) runs at **import time** in the editor (GPU compute); results are cooked into the `.envmap` payload, so neither the editor at load nor the runtime ever re-bakes.
1. `stbi_loadf` → RGB32F equirect, at most 8,192 texels wide (keeping the upload at most 512 MiB); non-finite texels rejected; optional `ClampLuminance` import setting (default off).
2. Equirect → `EnvSkybox` RGBA16F cube, face `min(1024, width/4)`, full mip chain by compute downsampling (skybox and `SkyboxBlur`).
3. Specular prefilter → `EnvSpecular` RGBA16F 256² × 7 mips, mip *i* = perceptual roughness *i*/6, GGX importance sampling with **filtered importance sampling** (source mip chosen by PDF, Křivánek & Colbert), Hammersley sequence, 64–512 samples per mip. This tames Poly Haven sun texels (> 50,000) without ad-hoc clamping.
4. Diffuse irradiance → **SH9** (L2) by a compute reduction with exact texel solid angles in a fixed order, over the first skybox level of at most 64², then normalized, convolved with the cosine lobe over π and windowed (**Hanning**, w = 4) against ringing from small bright sources; negative results clamped in the shader. Baked texels are clamped to 65,504, the largest finite binary16 value.
5. DFG/BRDF LUT RG16_FLOAT 128², Filament's multi-scatter form `(∫ Fc·Gv, ∫ Gv)` over (NdotV, perceptual roughness), generated once at device startup by compute (< 1 ms), state permanent. It depends only on the BRDF and belongs with it (`Renderer/BrdfLut`), not with the bake.

Shading: diffuse IBL `SH(N) · albedo · (1 − F_ms) · multiBounce(AO)`; specular IBL split sum with multi-scatter and specular occlusion (Lagarde, from AO, NdotV and roughness) plus horizon occlusion for normal-mapped surfaces. `Rotation` rotates the lookup vector; `Intensity` scales both terms. More HDRIs: `Scripts/FetchAssets.py hdri <id> --res 2k --dest <Project>/Assets/Environments` then `asset.import`.

**Conventions** (`Asset/EnvironmentData.h`, shared by the shaders and the CPU references): right-handed, +Y up; the equirectangular centre is −Z and u = 0.75 is +X; cube faces and texel directions follow Vulkan's face selection; `IrradianceSH9` holds the coefficients of irradiance / π, so a constant environment evaluates to its radiance; `Rotation` turns the environment by +Rotation about +Y. The skybox face is `max(1, min(1024, width / 4))` or an explicit power of two up to 1024, and specular mip 0 is the skybox resampled to the specular size; `EnvironmentBaker::BakeWithOptions` bakes the oracle tests' small cubes. `Intensity` is at most 65,504 and the renderer clamps it, zeroing any scaled SH coefficient that is not finite (ADR 0013 decisions 7, 9, 21 and 27).

### 8.7 Soft shadows
- **Directional CSM + PCSS.** Up to 4 cascades over `ShadowDistance`, practical split (`CascadeSplitLambda`), bounding-sphere fit of the cascade's **generic frustum-slice corners**, which are computed from the inverse view-projection for both projection kinds, so an orthographic camera's box-shaped frustum fits the same way. The projection is snapped to the texel grid (a sub-texel camera move leaves the light matrix unchanged), depth range extended toward the light for off-screen casters, depth-clamp pancaking.
  One directional light per view owns the four-layer target: the first eligible visible shadow-casting directional light in canonical order; other directionals still illuminate without shadows. Stabilization quantizes the sphere radius and light-space centre, including Z; the unchanged-matrix guarantee applies to movement within the same snap cell, not a movement across its boundary (ADR 0017).
- **PCSS:** blocker search with 16 Vogel-disk taps; penumbra width in world units `(dReceiver − dBlocker) · tan(LightAngle / 2)` **converted to UV per cascade**, so softness is consistent across cascade boundaries; filter with 32 Vogel taps through the comparison sampler (`SampleCmpLevelZero`), radius clamped to [1.5 texels, cascade maximum]; kernel rotated per pixel by a fixed interleaved-gradient noise pattern (no temporal variation). Cascades blend over the last 10% of each range; shadows fade out at `ShadowDistance`.
- **Bias:** rasterizer slope-scaled bias scaled by cascade texel size (`DepthBias`) plus receiver normal offset (`NormalBias`). Both directional and spot PCSS correct each tap's comparison depth to the geometric receiver plane. Blocker taps use their actual texel centres; bilinear comparison taps use the nearest receiver depth over only their four-texel footprint, with a bounded floating-point roundoff allowance. The triangle normal is derived before light/cascade divergence, so interpolated or normal-mapped shading normals cannot create false blockers on a planar receiver (ADR 0017).
- **Spot lights:** the same filter on their atlas tile, penumbra from `SourceRadius`.
- Pure-function tests: split distances, sphere fit (perspective and orthographic corner sets), texel snapping, atlas allocation. GPU test: a 0.37-texel camera translation changes the shadowed image by less than the stability threshold. Device tests force `DisableDepthClamp` and verify the extended-near fallback's caster depth and receiver shadow under both Vulkan caps. Goldens `ShadowsNear`, `ShadowsFar` and `ShadowsOrtho` (the Tetris-style orthographic camera).

### 8.8 SSAO: GTAO
Ground-truth ambient occlusion (Jimenez et al. 2016), implemented in-house in Slang from the paper:
- Main pass on the `ViewDepth` pyramid with the prepass's normal-mapped normals: Low 1 slice, Medium 2, High 3, each with 3 steps per side, cosine-weighted horizon integration with a thickness heuristic, per-pixel rotation from a fixed interleaved-gradient pattern.
- Two edge-aware (depth and normal) bilateral denoise passes.
- Application is physically motivated: AO affects **indirect light only** (direct occlusion is the shadow maps' job), diffuse uses the Jimenez multi-bounce fit with albedo, specular occlusion derives from AO, NdotV and roughness, and the final factor is `min(GTAO, materialAO)`.
- `SsaoRadius` (world units), `SsaoIntensity` (power) and `SsaoQuality` come from `PostProcessComponent`.
- **Projection-aware.** View-space positions come from the projection-aware reconstruction of §8.3. The screen-space radius is `SsaoRadius · 0.5 · ViewportHeight · |P[1][1]| / linearDepth` for perspective and `SsaoRadius · 0.5 · ViewportHeight / OrthoHalfExtents.y`, constant across the screen, for orthographic. The view vector is per pixel for perspective and constant for orthographic. Goldens `GtaoOn`, `GtaoOff` and `GtaoOrtho`.

### 8.9 HDR, tonemapping, output encoding
`SceneColor` is RGBA16F linear. Exposure multiplier `2^ExposureEV`. Tonemappers: **AgX** (default, Base look), ACES (Hill RRT+ODT fit), Khronos PBR Neutral, Linear (clamp; debug). The tonemap pass applies the sRGB OETF in the shader and blue-noise triangular dither before 8-bit quantization (no sky banding). Swapchain and viewport textures are UNORM holding sRGB-encoded values, so ImGui blends in gamma space as it expects and `ImGui::Image(viewport)` shows exact colours. CPU reference implementations of every tonemapper are compared with the GPU at 1,024 sample points.

**Order and guards** (ADR 0013 decisions 8 and 21): the bloom lerp in linear radiance; NaN replaced by 0 and exposed radiance clamped to 65,504; exposure from `ViewConstants::Exposure` (`2^ExposureEV`, or 1 for debug views); the tonemapper; the sRGB OETF (unless `EncodeSrgb` is off); the blue-noise triangular dither of ±1 LSB tiled by pixel coordinate, which fades out within the first and last 8-bit level so exact black and white stay exact. One pipeline: the tonemapper and the flags are constants. The tonemapper formulas are frozen in `Renderer/TonemapPass.h` (AgX's inset and outset matrices, log2 range [-12.47393, 4.026069], 6th-order sigmoid and 2.2 power, whose white point 0.9965 still encodes to 255; ACES as Hill's input matrix, RRT+ODT fit and output matrix without an exposure pre-scale; Khronos PBR Neutral's reference constants; Linear's clamp), because the shader and the CPU reference implement them independently; the `TonemapCurves` test program evaluates the shader functions in 32-bit float for the comparison. Bloom has no threshold: the upsample adds each level into the next larger one and the last step divides by the mip count, so a uniform scene blooms to itself; its first downsample replaces NaN with 0 and clamps samples to 65,024 / 6, so the sums stay finite in both storage formats.

### 8.10 Debug draw, picking, selection, text
- `DebugDrawList` (CPU, in the snapshot): `Line`, `Ray`, `Box`, `Sphere`, `Capsule`, `Arrow`, `Frustum`, `Text3D`, each with colour, duration (simulation seconds) and `DepthMode {Tested, OnTop}`. Filled by scripts (`Debug.*`), collider visualization (built from components, so it works in edit mode), light/camera gizmos and the editor. Drawn after tonemapping as 1 px `LineList` (wide lines are unavailable on MoltenVK) from one geometrically growing dynamic vertex buffer. A box is a centre, half extents and a rotation; a capsule the segment between its hemisphere centres and a radius; a frustum eight explicit corners (a perspective camera's far plane is at infinity). `Advance(delta)` removes the commands whose time is spent, then ages the rest; the list's owner calls it once per simulation step before that step's producers add, so a 0-duration command is extracted once and a timed one stays at least its duration. A list holds at most 65,536 commands (more are dropped and counted); `BuildDebugLineVertices` tessellates at most 2^20 vertices per record and skips, counting them, the commands that do not fit, that are not finite or whose vertices overflow, and the renderer warns once (ADR 0013 decisions 6, 10 and 27). **Collider visualization** (`Scene/ColliderDebugDraw`, edit and play) builds renderer-agnostic records from the components and the composition rules (§5.3), in world space and coloured by category: Static, Kinematic, Dynamic, Sleeping (play only), Trigger, Character and Invalid (a body the composition accepts but the session refuses or did not create); `AppendColliderDebugDraw` maps them one to one onto the list (Box, Sphere, Capsule, and a mesh collider's triangle edges as Lines), with duration 0 and depth-tested (ADR 0014 decisions 16 and 34). The views that show colliders (the screenshot annotation, the viewport option) append them to their snapshot and build them at the snapshot's alpha (`ColliderDebugDrawOptions::Alpha`), so in a running play view each record sits at its entity's rendered pose, on its mesh, and not up to a step ahead of it (ADR 0016 decision 3).
- **Picking.** The prepass writes `EntityId` = snapshot-local index + 1, mapped back through `PickTable`, so IDs never go stale when EnTT recycles entities. Editor clicks read 1×1 through a pooled staging texture two frames later (no stall), suppressed while ImGuizmo is hovered or active. Automation never uses GPU picking: `viewport.pick` and `scene.raycast` run a deterministic CPU ray test (AABB, then triangles from the CPU mesh copy).
  Each asynchronous request owns its UUID mapping and echoes view generation, revision and click sequence; stale results are discarded and UUIDs re-resolved. Cancellation retains GPU resources until their submission retires. CPU pixel rays use the current scene/game image's actual extent (640×360 headless default), top-left integer coordinates and pixel centres; hidden/minimized images are unavailable even when the camera retains its last aspect. Raycasts are geometric, two-sided and do not sample material alpha. The optional mask uses project physics layers: nearest enabled ancestor-or-self rigid-body layer, otherwise the entity's character-controller layer, otherwise Default; unknown names also use Default. No collider or physics world is required (ADRs 0017–0018).
- **Selection outline.** Selected meshes render into `SelectionMask` (1.0 visible, 0.5 occluded by `SceneDepth`); a separable dilation and edge composite draws a solid outline for visible and a dimmer one for occluded parts. No stencil needed.
- **Text.** `TextRenderer` batches SDF glyph quads (smoothstep over `fwidth`). Screen text is laid out from `Anchor`, `Pivot`, `Offset` and `Size` scaled by viewport height / 1080; world text is depth-tested, optionally billboarded. Font atlases are mirrored by the `TextRenderer` itself (a pass-owned GPU object keyed by (handle, version), uploaded through `HostImageUpload` and released by `CollectStale`). Given the snapshot camera's view (`TextRenderInputs::CameraView`), a label on or behind the camera plane is neither recorded nor counted. A record holds at most 2^20 text vertices (an item that does not fit is left out whole, with one warning per renderer), and an item with a vertex beyond float's range is skipped (ADR 0013 decisions 11, 20, 21 and 27).

### 8.11 ImGui through NVRHI
`Engine/ImGui/ImGuiRenderer` is our backend for Dear ImGui 1.92.9b (docking): sets `ImGuiBackendFlags_RendererHasTextures` and `RendererHasVtxOffset` (16-bit indices), services `ImDrawData::Textures` (`WantCreate`/`WantUpdates`/`WantDestroy`, `SetTexID`, `SetStatus`) and destroys remaining textures in `GetPlatformIO().Textures` at shutdown; one pipeline per target format, straight-alpha blending, per-command scissor, projection in push constants; dynamic vertex/index buffers per frame slot growing geometrically. `ImTextureID` (`ImU64`) is a key into a slot map `{nvrhi::TextureHandle, mip, slice, cached BindingSet}` that holds the handle until the frame using it retires. The platform side is the vendored `imgui_impl_glfw` (`ImGui_ImplGlfw_InitForVulkan`), compiled in its own NoPCH TU, on Win32, Cocoa and X11. It cannot initialize on the GLFW null platform (it needs the native window handle), so in headless mode `ImGuiLayer` is the platform side itself: display size and framebuffer scale from the `Window`, the frame clock's delta, GLFW's in-memory clipboard and no input. Docking on, multi-viewport off. `io.IniFilename` points at `user://Editor/imgui.ini` (resolved to a native path) while no project is open (the launcher), and at `<Project>/Library/Editor/imgui.ini` once a project is open; ImGui is told to reload the file on the switch. ImGuizmo's known clip-rect issue is mitigated by calling `Manipulate` last in the viewport window and popping any surplus clip rects afterwards.

### 8.12 Shader pipeline
**Slang → SPIR-V with slangc** from the Vulkan SDK: HLSL-compatible syntax with modules (`import Lighting;`), generics, first-class SPIR-V and `-reflection-json`/`-depfile` support; it ships on all three OSes, and offline compilation keeps compilers out of shipped games.
- `Resources/Shaders/Shaders.json` lists programs, entry points, stages and permutations: `{ "Programs": [ { "Name": "Forward", "File": "Passes/Forward.slang", "Entries": [ { "Name": "VSMain", "Stage": "vertex" }, { "Name": "PSMain", "Stage": "fragment" } ], "Permutations": { "ALPHA_MASK": ["0", "1"] } } ] }`.
- `CompileShaders.py` (the only place flags exist) runs per entry and permutation: `slangc <file> -target spirv -profile spirv_1_6 -entry <E> -stage <S> -fvk-use-entrypoint-name -matrix-layout-column-major -fvk-t-shift 0 all -fvk-s-shift 128 all -fvk-b-shift 256 all -fvk-u-shift 384 all -I Resources/Shaders -warnings-as-errors all -O2 [-g in Debug/Release] -D<PERM>=<V> -reflection-json <out>.refl.json -depfile <out>.d -o <out>.spv`, then `spirv-val` on every output. Incremental by depfile, flags hash and the slangc version string (checked against the version pinned in `Scripts/Lib/Toolchain.json`, currently 2026.8 from SDK 1.4.350). It is invoked by the `Shaders` utility project (for ninja, by the same rule on `Engine`; §2.2), whose declared inputs make IDE builds recompile changed shaders, and writes the stamp file last.
- `ShaderLibrary::Get(program, entry, permutation) -> nvrhi::ShaderHandle` loads `.spv` from `engine://Shaders` (dev) or `Engine.pak`. `PipelineFactory` checks every C++ `BindingLayoutDesc` against the reflection JSON (set, binding, resource type, array size, constant-buffer size); a mismatch is a load error. A `BindingLayoutDesc` carries no byte sizes, so each pass's `PipelineLayoutDescription` also gives the byte size it binds to each constant buffer (`ConstantBuffers`, `sizeof` the C++ struct) and the format it creates for each storage image (`StorageImages`), which the check compares with the reflected struct size and `[vk::image_format]` (ADR 0009 decision 13). **`Shaders.LayoutsMatchReflection` runs this check for every pipeline without a GPU**, so binding bugs surface in CPU-only tests.
- Hot reload (Editor dev builds): a `.slang` change runs `CompileShaders.py --program X` as a subprocess (a compiler crash cannot take the editor down) and rebuilds dependent pipelines; a failed compile keeps the old pipeline and logs located diagnostics.
- The passes' pipelines are created at startup; no runtime compile hitches. The exception is the forward passes' debug-view variants, created on first use (§8.5). The ImGui renderer creates its pipeline per target format on the first UI frame that renders into that format (the swapchain's, then RGBA8 for screenshots; ADR 0009 decision 25); a failure there is `FatalError(OutOfMemory)` like any pipeline failure (§8.14 item 7).

### 8.13 Headless rendering and screenshots
The headless device (no surface) plus `OffscreenTarget` + `Readback` is the **same path** used by editor screenshots, `viewport.screenshot`, `editor.screenshot` (the full editor UI rendered through ImGui on the GLFW null platform), golden tests, thumbnails and `Runtime --screenshot-at`. Screenshots are **re-rendered** at the requested size, camera and debug view after `AssetManager::WaitIdle()`; they never grab the last presented image. Annotations (entity labels with short IDs, collider outlines, AABBs, axes) are drawn into the overlay pass for that capture only. PNG encoding uses stb_image_write.

### 8.14 GPU resource lifetime
1. Every GPU object is held by an NVRHI handle owned by exactly one engine object: `GpuResourceCache` (meshes, textures, materials, environments by `(handle, version)`), `RenderTargetPool`, a pass, or the ImGui renderer.
2. Releasing means dropping the handle; NVRHI defers destruction until command lists referencing it complete; `runGarbageCollection()` runs every frame.
3. Binding sets are cached by (layout, resource generation); a hot reload bumps the generation.
4. Shutdown: `waitForIdle` → SceneRenderer → GpuResourceCache → ImGuiRenderer → RenderTargetPool → Swapchain → NVRHI device → `VkDevice` → debug messenger → `VkInstance` → loader.
5. `GpuResourceTracker` (non-Dist) counts creations and destructions per type. GPU tests assert live counts return to baseline after a scene unloads and are zero at device destruction; validation-layer leak reports also fail tests. `RenderStats` tracks the allocation count against `maxMemoryAllocationCount` (warning above 2,000).
6. Device loss, GPU out-of-memory and GPU hangs are fatal (§8.1 lists the three ways device loss surfaces): autosave (editor; CPU only), crash report with `VK_EXT_device_fault` details when available, exit 4. Recovery is out of scope for v1.
7. **Creation failures are values.** NVRHI `create*` calls return a null handle on failure, for example on device out-of-memory. The engine never calls them directly outside `Graphics`: `GraphicsDevice` wraps each one (`Result<nvrhi::TextureHandle> CreateTexture(const nvrhi::TextureDesc&)`, likewise for buffers, samplers, shaders, pipelines and binding sets) and turns a null result into `ErrorCode::Gpu` with the descriptor's debug name. `GpuResourceCache` reacts to an asset upload failure with the asset's placeholder plus an `AssetDiagnostic`. Render targets and pipelines are created at startup or resize (ImGui's on first use, §8.12), and their failure is `FatalError(OutOfMemory)`.
8. **Fault injection.** In non-Dist builds, `GpuDiagnostics` accepts `--gpu-inject-fault=device-lost|oom-texture|hang`. It forces the corresponding path: the device-lost flag after the next submit, a null `CreateTexture`, or a timeline wait that always reports `VK_TIMEOUT`. Tests check exit code 4, the crash report and a written autosave for device loss and hang, and a placeholder plus diagnostic for the texture OOM. Until the autosave (M10) and the asset placeholder (`GpuResourceCache`, M6) exist, the tests check the editor's fatal-error hook line in the crash report and the logged texture diagnostic with a run that keeps going (ADR 0009 decision 8).
9. **Stale mirrors.** Every host that renders calls `GpuResourceCache::CollectStale()` and `SceneRendererPipelines::CollectStale(assets)` once per frame after its renders were executed, so replaced versions (environments are about 50 MB each, materials, font atlases) do not stay mirrored; both pass `releaseUnused = true` exactly once, at the first collection after a frame rendered a newly shown scene (opened, closed or swapped, play mode included). `Renderer/StaleMirrorSchedule` decides that moment, fed by the editor's `EditorCore/ShownSceneTracker` (the shown edit and play scenes compared with the previous frame's, plus the `SceneOpened` and `PlayStateChanged` events since, a dropped event counting as a change) and by the Runtime's start scene. `GraphicsDevice::Shutdown` sweeps the resource tracker before its host-image leak check, since a released binding set may still reference textures through it (ADR 0013 decisions 7, 22 and 27).

---

## 9. Physics (Jolt 5.6.0)

### 9.1 Integration
- `PhysicsEngine::Initialize()` (process level, from `ProcessContext`, §4.1): `JPH::RegisterDefaultAllocator()`, `JPH::Trace` and `JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = …)` routed to the logger and assert handler, `JPH::VerifyJoltVersionID()` (false is fatal), `Factory::sInstance = new JPH::Factory()`, `RegisterTypes()`, and one `JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, threads)` (threads from `ProcessContextSpecification::Physics`; `SetWorkerThreadCount` changes them between steps, which no result depends on). Shutdown reverses it and asserts that no world and no body is alive.
- `PhysicsWorld` (ECS-agnostic, pimpl, one per play session): `PhysicsSystem::Init(maxBodies 16384, 0, maxBodyPairs 65536, maxContactConstraints 16384, …)`, a `TempAllocatorImplWithMallocFallback` (16 MB, falling back to the heap where `TempAllocatorImpl` would abort), gravity from the project. Its API is expressed in engine types: `CreateBody(const BodyDescription&) -> Result<BodyHandle>`, `DestroyBody`, `SetPose(body, pose, activate)`, `GetPose`, `MoveKinematic`, `AddForce`, `AddForceAtPosition`, `AddTorque`, `AddImpulse`, `AddAngularImpulse`, `Get/SetLinearVelocity`, `Get/SetAngularVelocity`, `IsSleeping`, `WakeUp`, `IsBodyValid`, `GetUserData`, `GetMotionType`, `IsSensor`, `GetLayer`, `GetShape`, `SetShape` (a compound rebuilt in place), `Raycast`, `RaycastAll`, `ShapeCast`, `Overlap`, `GetBodyBounds`, `GetSubShapeBounds`, `Step(dt, collisionSteps)`, `SetGravity`, `DrainContactEvents()` (unsorted), `GetStats`, `GetLayers`, `GetLimits`, `GetContactBuffer`. The void mutators assert on a dead handle or a wrong motion type and ignore the call, because their callers validate external input first. Approaching a limit (bodies, at most Jolt's 8,388,608; body pairs; contact constraints) raises `PHYSICS_LIMIT_EXCEEDED`: a warning logged once when a count reaches 90 % of a limit, an error at it. Jolt asserts inside `Update` whenever it returns an `EPhysicsUpdateError` (full caches); the engine's assert hook lets exactly that assert pass and the world reports the error as `PHYSICS_LIMIT_EXCEEDED`. Bodies beyond the body limit are not created, and the entity gets a diagnostic plus, at run time, a script error.
- **Nothing invalid reaches Jolt.** Every value entering `PhysicsWorld` is finite (§5.4), so script `0/0` and automation typos are rejected at the binding or protocol layer with a located error. Shapes are built only through `ShapeSettings::Create()`, and a `ShapeResult` error becomes `PHYSICS_INVALID_SHAPE` with Jolt's message; the body is then not created, which is never an assert. Scales are checked with `Shape::IsValidScale` before use. Dynamic bodies with all six DOFs locked (`PHYSICS_ALL_DOFS_LOCKED`) and Dynamic bodies with trigger colliders (`PHYSICS_DYNAMIC_TRIGGER`) are validation errors, and such bodies are not created. Together with the minimums on scale, extents and radii (§5.3), this keeps `JPH_ENABLE_ASSERTS` (on in Debug and Release, routed to the crash handler) and MSVC Debug floating-point exceptions unreachable from content. A play-session fuzz test (§15.2) proves it.
- **What the world refuses, and what it makes safe** (ADR 0014 decisions 3, 27, 28 and 34). The world itself refuses what Jolt must not see, so a caller that forgets a check still cannot reach a Jolt assert: a Dynamic sensor, a Dynamic body without a degree of freedom or whose shape holds a mesh, non-finite values, a full world, colliders smaller than `PhysicsShape::MinColliderSize` (2 mm after scaling) or larger than `MaxColliderSize` (100 km across), flat convex hulls, anything placed beyond `MaxPhysicsCoordinate` (1e9 m; one placement rule, `IsPlaceablePhysicsPose`, serves the world, the character and the system), a mass above `MaxPhysicsMass` (1e9 kg) or an inertia above 1e18 kg m² (`PhysicsShape::CheckDynamicBody`), and a gravity component beyond `MaxPhysicsGravity` (1e12 m/s²). Settings Jolt asserts on are made safe instead, because they are valid content: Kinematic bodies get every degree of freedom and fixed mass properties; a Dynamic body gets the shape's mass properties scaled to `Mass`, made symmetric, with isotropic inertia added to an ill-conditioned shape; colliders are built at 1000 × 2⁻⁶⁴ kg/m³, so Jolt's mass sums cannot overflow while every result scaled to a body's mass is bit for bit the same; starting velocities are clamped just below the maxima; `LinearCast` falls back to `Discrete` for a shape without an inner radius; `GravityFactor` is bounded to ±1000 and the maximum velocities to 1e6; the forces accumulated on a body are scaled so a step changes its velocities by at most 1e9, and impulses are clamped velocity changes; mesh-versus-mesh pairs are dropped before the narrow phase. `CollisionGroup` (64 bits, 0 = none) keeps the bodies of one group from colliding or reporting each other (§5.3). The world wakes the bodies around a destroyed body, a body whose shape changed and a new or teleported Static body, since Jolt wakes no neighbour itself.
- In MSVC Debug Jolt enables floating-point exceptions during `Update`; contact listener code must not produce NaN/Inf. Resting contacts penetrate by `mPenetrationSlop` (0.02 m); tests use tolerances.
- **Determinism across configurations.** `JPH_CROSS_PLATFORM_DETERMINISTIC` is defined for Jolt and every consumer, and Jolt and first-party code compile with the precise floating-point model (§2.2). Jolt documents that it is then deterministic regardless of compiler, configuration (Debug/Release/Distribution), OS and architecture, provided its inputs are identical. First-party simulation code keeps that guarantee by using precise FP, canonical order, DetMath instead of CRT transcendental functions (§4.12), total orders for every sort on the simulation path (ties broken by UUID), so the unspecified order of equal elements in `std::sort` never matters, and the same API call order in every configuration. The guarantee is: same source, same toolchain, same platform, so identical state hashes in Debug, Release and Dist. The `determinism` CI stage proves it (§15.8). Cross-OS bit-exactness is not claimed (§1.2).

### 9.2 Components to bodies (`Scene/PhysicsSystem`)
- Shapes: Box, Sphere, Capsule, ConvexHull (`MeshCollider.Convex`) or Mesh (`MeshShape`, Static/Kinematic only). Scale is baked into the shape; non-uniform scale on spheres and capsules uses the largest axis and raises `PHYSICS_NONUNIFORM_SCALE`. `Offset`/`Rotation` use `RotatedTranslatedShape`. Several colliders (§5.3) form a `StaticCompoundShape`. Mesh shapes are cached per (mesh handle, version, scale). A body's shape is described as a `BodyShapeDescription` of `ColliderShapeDescription`s (geometry, `Scale` in the collider's own axes, then `Rotation` and `Position` within the body, and `UserData`, the collider index); a cached mesh is placed as `SharedShapeGeometry`, an already built one-collider shape, so compound rebuilds never rebuild a mesh BVH (ADR 0014 decision 5).
- **Collider identity in compounds.** Each sub-shape of a compound gets its collider index as sub-shape user data (`CompoundShapeSettings::AddShape(…, userData)`). A per-body table maps the index back to the collider entity's UUID. Contacts and queries resolve a `SubShapeID` by its first level to the compound's sub-shape user data (`GetSubShapeIndexFromID`, then `GetCompoundUserData`; one rule, `Detail::ResolveColliderIndex`), never through `GetSubShapeUserData`, which returns a shared leaf's own data (ADR 0014 decisions 5 and 24). That names the exact child collider, so contacts report both the body owner and the collider entity that was hit (§9.4), and `Physics.Raycast` hits report the collider entity (with `Body` as the owner).
- Body settings: `Type` → `EMotionType`; `MotionQuality::LinearCast` → CCD; lock flags → `EAllowedDOFs`; `EnhancedInternalEdgeRemoval` → `BodyCreationSettings::mEnhancedInternalEdgeRemoval`; mass, friction, restitution, damping, gravity factor, max velocities. `UserData` holds the entity UUID; a `BodyID → UUID` table serves events whose callbacks only get IDs.
- **Seams.** Jolt removes ghost contacts on internal edges only within one shape, so separate bodies bump at shared edges. Tracks use the shared-static-body pattern (§5.3), and rolling bodies set `EnhancedInternalEdgeRemoval`, which makes Jolt collide such a body against a compound with its `InternalEdgeRemovingCollector`. Test: a 0.5 m sphere rolled at 6 m/s across 20 adjacent 1 m boxes of one static compound keeps `|v.y| < 0.05 m/s` throughout. A control case with 20 separate bodies documents the bump the pattern avoids. Measured with the integrated world, Jolt's own collector is not enough: with its default convex radius the boxes' rounded edges form a groove at every seam, and with flush faces its 1-degree face test still keeps the ball's contact with the next box's leading edge. Boxes therefore have no convex radius, and every world collides body pairs through `Detail::CollideBodiesWithEdgeRemoval`: Jolt's default collision for pairs without enhanced internal edge removal, and for the others the same algorithm with the face test at 0.1 degree and a total order on equally deep delayed contacts (ADR 0014 decision 26).
- **Triggers** (`IsTrigger`) are Jolt sensors (`mIsSensor`), created **Kinematic** and kept active, so they also detect sleeping bodies (Jolt docs: static sensors only see active bodies). They also set `mCollideKinematicVsNonDynamic`, so they detect kinematic platforms and character inner bodies (§9.6). Implicit sensor bodies (§5.3) follow their entity with `MoveKinematic`. A sensor `RigidBody` of Type Static is created as a Kinematic sensor for the same reason and follows its entity the same way.
- Bodies are (re)built at `PreStep` when RigidBody, a collider or `Active` changes (EnTT signals mark them dirty), so component order does not matter. Bodies are created in canonical order. A body is also rebuilt when a gathered collider moved relative to its body, the hierarchy changed above or below a collider, the world scale of an owner or gathered collider changed (scale is baked in), the `MeshRenderer.Mesh` a mesh collider falls back to changed, or a collider's mesh got a new version. A rebuild that keeps the motion type and sensor flag uses `PhysicsWorld::SetShape` (handle, pose, velocities and contacts kept); any other destroys and creates the body with its current velocities (ADR 0014 decision 11).
- **Layers.** Up to 16 named project layers plus a symmetric collision matrix. Object layer = `(layerIndex << 2) | kind` with kind ∈ {Static, Moving, Sensor}; broad-phase layers `NonMoving`, `Moving`, `Sensor`. The pair filter is `matrix[a >> 2][b >> 2] && !(both Static) && !(Sensor and Static) && !(both Sensor)`, so sensors never pay for static geometry or each other, as Jolt recommends. Three small engine classes implement Jolt's filter interfaces. Masks are 32-bit (`PhysicsLayerMask`, the width of Luau's `bit32`; bits at or above the layer count select nothing; ADR 0014 decision 4). Queries take a layer mask (`Physics.LayerMask("Track", "Default")`).

### 9.3 Fixed step and sync
- `Step(fixedDt, collisionSteps)` with `collisionSteps = max(1, ceil(60 / FixedHz))`, computed in integers: `1.0 / 60 × 60` need not round to exactly 1, and an extra collision step would change results (ADR 0014 decision 3).
- **PreStep** runs right after a `TransformSystem::Update` (§5.7 step 5), so world matrices include every Transform write made earlier in the same tick. Kinematic bodies call `MoveKinematic(target, fixedDt)` (correct velocities, so moving platforms carry the ball). Teleports are detected exactly: a Dynamic body or a character whose owner's local `Translation` and `Rotation` differ bit for bit from the values PostStep wrote (a pose decomposed from the world matrix would differ in the last bits and wake every body every tick), and a Static body whose world matrix differs from the one it was created or last teleported at, is teleported in the same tick with `SetPositionAndRotation` and woken, keeping its velocity unless the script also set it. A character is moved with `CharacterController::SetPose` before its update; a Kinematic body whose entity carries `InterpolationResetTag` at PreStep is placed with `SetPose` instead of swept, so it does not fling what rests on it. A Transform written beyond `MaxPhysicsCoordinate` is not applied: the entity's local values go back to the body's (ADR 0014 decisions 11 and 34). Test: "Transform write in OnFixedUpdate teleports the body in the same tick".
- **PostStep:** every Dynamic body that is active or has a parent writes its world pose back as local transforms (through the parent inverse), so the entity always shows where the body is. Bodies are visited in canonical order, never in `GetActiveBodies` order. Render interpolation does not depend on PostStep; it uses the per-step snapshot of every entity (§5.2). A dynamic body under a moving parent raises `PHYSICS_DYNAMIC_UNDER_MOVING_PARENT`. It is simulated in world space, not carried by the parent.
- **Body control** (ADR 0014 decisions 13 and 34). Every body function takes the owner's UUID and returns a `Result`: NotFound without a created body (or for an owner pending destruction or disabled), InvalidArgument for non-finite or out-of-range values, InvalidState for the wrong motion type. `MoveKinematic(entity, position, rotation)` sets the entity's world pose and the next PreStep sweeps the body there, so the Transform stays the single source of a kinematic body's pose. `Teleport` sets the entity's and the body's world pose at once, tags `InterpolationResetTag` and keeps the velocity. Forces and impulses need a Dynamic body, and so do `SetLinearVelocity` and `SetAngularVelocity`, since every PreStep sets a Kinematic body's velocities through `MoveKinematic`; Static bodies take no velocity.

### 9.4 Collision and trigger events
`ContactListener` callbacks run on Jolt worker threads during `Update`. `OnContactAdded` appends `{Added, BodyA, BodyB, SubShapePair, Point, Normal, RelativeNormalSpeed}` and `OnContactRemoved` appends `{Removed, SubShapePair}` to a mutex-guarded buffer; nothing else happens on workers. After `Update` the main thread:
1. maps body IDs to UUIDs and collapses sub-shape contacts into body-pair begin/end events with a per-pair reference count;
2. **sorts events by (UUID a, UUID b, type)** (Jolt documents callback order as non-deterministic);
3. drops events whose entities were destroyed;
4. dispatches to scripts on **both** entities: `OnCollisionEnter(other, contact)`, `OnCollisionExit(other)`, and for sensor pairs `OnTriggerEnter(other)`, `OnTriggerExit(other)`. `other` is the body owner. `contact = {Point, Normal, RelativeSpeed, Collider, OtherCollider}`, where `Collider` and `OtherCollider` are the collider entities of the first sub-shape pair that touched, resolved through the sub-shape user data (§9.2). They differ from the owner for compounds, for example the track piece under a level root.

Events are per body pair: a ball rolling from one piece of a compound track to the next stays in contact with the same body and gets no new `OnCollisionEnter`.

**Exit on destroy, disable and character contacts.** Jolt reports a destroyed body's `OnContactRemoved` only at the next step, with the stale handle, so the session keeps the set of active pairs and closes a destroyed body's contacts itself (ADR 0014 decision 25). At each destroy/disable flush (§5.7 step 8 and the frame-phase flush), every active pair involving a destroyed or newly disabled entity is closed: the surviving partner receives `OnCollisionExit(other)` or `OnTriggerExit(other)`, with `other:IsValid() == false` for a destroyed entity and `other:IsActive() == false` for a disabled one. Teleporting out of a trigger is reported by Jolt's next step as a normal exit. A checkpoint trigger therefore never keeps a stale "inside" state after a respawn.

Scripts never run on physics threads. Destroying an entity inside a callback is deferred (§5.7).

**Dispatch details** (ADR 0014 decisions 11, 12 and 25). The system delivers one `PhysicsEvent` per entity of a pair event (`{Type, Self, Other, Contact, Synthesized, Tick}`, the normal from `Self` towards `Other`) to an `IPhysicsEventListener`, which M13's dispatcher implements; pairs are sorted by (lower UUID, higher UUID, type) and each is dispatched first to the lower UUID. Events of a pair with an entity already pending destruction or disabled when the dispatch starts are not dispatched (an enter is dropped and the pair never becomes active; the exit is the flush's). A callback whose `Self` an earlier callback of the same dispatch destroyed or disabled is dropped, while its partner's is still delivered; a side that got the enter gets exactly one exit unless its own entity goes first. Pairs are keyed by owner UUIDs and kind, so they survive rebuilds: a rebuilt body's pairs become unconfirmed, and at PostStep a pair that no contact re-established ends with a synthesized exit. Jolt also reports `OnContactRemoved` when two touching bodies fall asleep and `OnContactAdded` when they wake; such contacts stay **dormant** (still counted for their pair), so sleeping never ends a pair, and are dropped only after a step that began with one of their bodies awake and did not report them again. `FlushDestroyed` repeats its close, dispatch and remove pass until a pass finds nothing. Run-time diagnostics are logged once and passed to `IPhysicsEventListener::OnPhysicsDiagnostic`, which M13 turns into the script error for a refused body (§9.1).

### 9.5 Queries
`Raycast`, `RaycastAll`, `SphereCast` (`CastShape`), `OverlapSphere`, `OverlapBox` through `NarrowPhaseQuery` on the main thread, filtered by layer mask, results sorted by distance then UUID. Hits report the collider entity and the owning body entity. Bounds: `GetBodyBounds(entity)` returns the world AABB of the body the entity owns, and `GetColliderBounds(entity)` the world AABB of that entity's own collider sub-shapes, whether they form a standalone body or part of an ancestor's compound. Both are nil when there is none. Queries include sensors, so a game can ask whether a point lies inside a checkpoint (ADR 0014 decision 7). `Raycast` casts all hits and returns the first after sorting by distance, then collider UUID, then body UUID; `SphereCast` keeps the world's single closest hit, ties broken by body handle (handles follow the canonical creation order); a world-level overlap returns one entry per (body, collider). Scene-level queries validate their input (radii and half extents within [`MinColliderSize` / 2, `MaxColliderSize` / 2], points within `MaxPhysicsCoordinate`; InvalidArgument otherwise) and leave out entities pending destruction or disabled; world-level ones assert on invalid input (ADR 0014 decisions 30 and 34).

### 9.6 Character and ball control
- `CharacterControllerComponent` wraps `JPH::CharacterVirtual` with `ExtendedUpdate` (stair stepping, floor sticking), updated in PreStep from the desired velocity set by `CharacterController:Move(velocity)`. `IsGrounded()`, `GetGroundNormal()`, `GetVelocity()`. A `CharacterVirtual` is not in the broad phase, so the character gets an inner body: an ordinary Kinematic capsule of the controller's size created by `PhysicsWorld::CreateBody` (not Jolt's `mInnerBodyShape`, which the world's records, limits and filters would miss), on the controller's layer and in its collision group, with the owner's UUID as user data, never sleeping, and the world's next body in the canonical creation order, so its ID is deterministic. The controller places it at the padded base after every update and teleport (ADR 0014 decisions 8 and 23). Raycasts, overlaps and sensors therefore see the character through its kinematic inner body. A `CharacterContactListener` records character-vs-body contacts into the same buffer, so the character's script and the other body receive `OnCollisionEnter`/`OnCollisionExit` with the same sorting and exit rules (§9.4).
- The rolling ball is a Dynamic sphere with `LinearCast`, `EnhancedInternalEdgeRemoval`, friction ≈ 0.8 and raised `MaxAngularVelocity` (Jolt's default 47 rad/s caps a 0.5 m ball at ≈ 23 m/s). The script applies camera-relative `AddTorque(cross(up, moveDir) * torque)` plus a small `AddForce` for air control. Each level root carries a `Static` `RigidBody`, so all track pieces form one static compound without seams (§5.3). Moving platforms are kinematic bodies driven by `RigidBody:MoveKinematic`. Checkpoints, goal and kill zones are triggers (implicit sensor bodies).
- **Character conventions** (ADR 0014 decisions 8, 23 and 34). The entity's origin is the capsule's base, and `Height > 2 × Radius` (else `PHYSICS_INVALID_SHAPE`). `Move(velocity)` is consumed by one PreStep: a script moves its character by calling it in every `OnFixedUpdate`, and a jump is one `Move` with an upward part. A step's velocity is the desired velocity's horizontal part plus, while grounded (Jolt's `OnGround` while the character does not move away from the ground faster than 0.1 m/s), the ground's vertical velocity and the desired vertical part, and in the air the previous vertical velocity plus gravity × `GravityFactor` × dt; it is clamped to `MaxCharacterSpeed` (500 m/s), and `MoveCharacter` refuses a desired velocity with a larger component. `StepHeight` sets both the stair step-up and the floor step-down. A gravity whose squared length underflows counts as none; the weight a character pushes onto its ground is capped at 1e10 N s; an update that would carry the character beyond `MaxPhysicsCoordinate` leaves it where it was with no velocity and warns once. The ground's horizontal velocity does not carry the character yet (open for M13's bindings).

### 9.7 Determinism tests
A 200-body pile stepped 600 times gives the same `ComputeStateHash()` across repeated runs and with Jolt thread counts 0, 1 and 8. Contact-event order is identical across runs. The same pile's final hash is a committed constant checked in the Debug and Release unit runs, and the `determinism` stage compares a 600-tick physics-and-script replay across Editor Debug, Editor Release, exported Release and exported Dist (§15.8). Other tests: free fall vs analytic, rest height (0.48 with slop), restitution bounce, trigger enter/exit exactly once (including a sleeping body), layer matrix (incl. no Sensor–Static/Sensor–Sensor pairs), teleport honoured in the same tick, kinematic platform carry, CCD prevents tunnelling at 200 m/s, destroy inside a callback is safe, seam test (§9.2), compound contacts name the child collider, synthesized exits on destroy and disable, trigger detects a character (inner body), invalid shapes, all-DOF-locked and Dynamic-trigger bodies are rejected with diagnostics and never assert.

---

## 10. Audio (miniaudio 0.11.25)

### 10.1 Engine
- `AudioEngine` (ECS-agnostic, one per application) wraps `ma_engine` whose resource manager uses a **custom `ma_vfs` backed by the engine VFS**, so loose files in the editor and `Game.pak` entries play through identical code. Groups (`ma_sound_group`): Master → Music, Sfx, Ui.
- **The engine never owns the device.** `ma_engine` is **always** created with `noDevice = MA_TRUE`, 2 channels at 48 kHz, in every mode. Windowed runs add a separate engine-owned `ma_device` (playback, f32 stereo, 48 kHz; miniaudio converts to the native format). Its data callback only calls `ma_engine_read_pcm_frames(&engine, out, frameCount, nullptr)`, the pattern miniaudio documents for caller-owned devices. Headless runs and tests have no device and pull frames themselves. Because the node graph never references a device, device loss or a reroute re-initializes **only the `ma_device`**: every `ma_sound`, group and voice keeps its state and playback position.
- **Device notifications.** The device config's `notificationCallback` only queues the notification; `AudioEngine::Update` handles the queue on the main thread, on the frame clock's time (never the wall clock). `rerouted` (default device changed, e.g. headphones unplugged) is logged. An unexpected `stopped` uninitializes the `ma_device` and re-creates it on the current default device after 1 s, up to 3 attempts, then gives up with one warning and keeps running with no device. If no device can be created at startup, the engine runs device-less with a warning. A `Rerouted` or `Started` after a `Stopped` cancels it (WASAPI stops, reroutes and restarts the device on a default-device change); a `Stopped` still pending a delay later re-creates the device only if it has not started again (ADR 0015 decisions 4 and 28).
- **Time ownership.** Three time sources, one reader at a time: `Device` (the callback reads in real time), `Simulation` and `Host` (no device: `Update` pulls the frames of each frame's unscaled delta with a fractional accumulator). While simulation time is owned the device callback outputs silence and does not read the engine, and the engine pulls one tick of frames per `AdvanceSimulationTick` into a discard or capture buffer: after n ticks at F Hz exactly round(n × 48000 / F) frames, half up and in integers (800 per tick at 60 Hz). The play session owns simulation time while it is in lockstep (§13.6) and, for its whole life, when it is a test run (`PlaySessionSpecification::OwnsAudioTime`, which M13's FeatureTest runner sets for every suite, `ScriptedClock` ones included); its `AudioUpdate` phase pulls one tick for every tick run since the last pull, never per frame, and ticks run before it took time are never pulled. A paused session that is not in lockstep does not take time: its voices are paused. Voices pause before time goes back to the device and resume only after the session has taken it (ADR 0015 decisions 5 and 13). Voices therefore advance with simulation time, and `play.step {ticks: 600}` no longer plays a burst out of sync. Normal play reads in real time through the device.
- **Deterministic decoding.** In test runs and headless processes, the resource manager is created with `MA_RESOURCE_MANAGER_FLAG_NO_THREADING` (which implies non-blocking) and `jobThreadCount = 0`. Before each pull, the `AudioEngine` processes pending resource-manager jobs inline (`ma_resource_manager_process_next_job` until empty), so streamed and async-loaded clips decode at the same simulation point in every run. Ordinary windowed Runtime keeps miniaudio's job thread. Windowed Editors construct the deterministic decoder from startup so in-process test runs do not replace a resource manager underneath loaded clips; playback devices remain available (ADR 0019 decision 6). A pull longer than 480 frames is read in chunks of at most 480, with the pending jobs processed before each, and `Update` processes them too, so a stream's next page is decoded before the mixer needs it (ADR 0015 decision 23). Test entry points select deterministic decoding before AudioEngine construction. `Test.CaptureAudio(n)` measures the exact sample interval of n ticks starting with its current step; with grouped fixed steps it waits for the containing AudioUpdate and resumes at the next phase 4, trimming earlier/later samples without an extra pull (ADR 0019 decision 6).
- **Clips** (ADR 0015 decision 6). A clip is named by its handle's 16 hex digits and registered per version under `"<16 hex digits>@<version>"` (miniaudio never replaces the data of a registered name), reference-counted (each `RegisterClip` balanced by `UnregisterClip`); a hot-reloaded version is a new registration while voices of the old one play on. An encoded clip that does not stream (at most `MaxDecodedClipSeconds`, 10 s, by default) is decoded once at registration (`Audio/AudioDecoder`'s `DecodeEncodedAudio`, interleaved f32) and registered with `ma_resource_manager_register_decoded_data`, as synthesized PCM is; `register_encoded_data` is not used, since every voice on encoded data would run its own decoder. A streamed clip (`MA_SOUND_FLAG_STREAM`) is decoded by the resource manager to the same f32, its bytes served by the `AudioVfs` as a memory file under the resource name (every other name resolves through the engine VFS), so streamed and decoded playback are sample-identical. The bytes are shared, never copied, and kept until the registration is removed and its last voice has ended.
- **Voices** (ADR 0015 decisions 7, 23, 24 and 28). `HandlePool<Voice>` of 64 `ma_sound`s, each started with `ma_sound_init_from_file` on its registration's resource name, which shares the data buffer node (what `ma_sound_init_copy` does inside). When full, the voice with the lowest priority is stolen, then the farthest from the listener (non-spatial voices count as distance 0), then the oldest, but only when its priority is not above the new voice's; otherwise `PlayVoice` fails with InvalidState. Priorities, fixed when a voice starts: 0 for effects (Sfx and Ui sources, every one-shot), 1 for music, 2 for the editor's preview, so effects never steal the soundtrack. A voice at the end of a non-looping clip is released at the end of the pull that played its last frame. The engine plays a voice at a pitch of at most 16 and reduces its Doppler factor so the Doppler pitch stays within [1/3, 3], and replaces every non-finite output sample with silence. `AudioVoiceInfo::CursorFrames` counts the frames the engine has mixed of the voice: exact while the engine pulls its own frames, miniaudio's cursor while a device reads. Captures (`StartCapture`/`StopCapture`, `Test.CaptureAudio`) record only the frames the engine pulls itself, up to 10 minutes.

### 10.2 Components and spatialization (`Scene/AudioSystem`)
`AudioSystem::Update` runs after transforms in the frame phase: it creates/destroys voices for `AudioSourceComponent`s (EnTT signals), applies fields (volume, pitch, looping, attenuation model, min/max distance, rolloff, Doppler factor, group), sets position, direction and velocity (from the delta of the rendered pose, below) for spatial sources, and feeds `ma_engine_listener_set_position/direction/velocity` from the primary `AudioListenerComponent` (the first in canonical order when several are primary, `AUDIO_MULTIPLE_PRIMARY_LISTENERS`), falling back to the primary camera (validation `AUDIO_NO_LISTENER` when neither exists and sources are spatial). Non-spatial sources use `MA_SOUND_FLAG_NO_SPATIALIZATION`. `Audio.PlayOneShot(clip, position?, volume?, group?)` is non-spatial when `position` is nil, spatial at `position` otherwise, and uses the `Sfx` group unless one is given. Play-mode pause pauses all session voices; Stop releases them. Edit mode is silent except asset-browser previews. `Attenuation::None` keeps a sound positioned (panned and Doppler-shifted) without distance falloff (miniaudio's inverse model with a rolloff of 0, since its own "none" turns spatialization off).

**Lifecycle** (ADR 0015 decisions 12, 13 and 28). One `AudioSystem` per Play session (none in Simulate and none without an engine), owned by `PlaySession` (`GetAudioSystem`). `Start` plays the `PlayOnStart` sources in canonical order; a source created later with `PlayOnStart` starts at the first Update after it exists; a source whose entity becomes effectively disabled is released, and a `PlayOnStart` one starts again when re-enabled; removing the component or destroying the entity releases its voice at the destroy flush; a changed `Clip` restarts a playing voice and a null `Clip` plays nothing; values a script writes out of range are clamped. A spatial source and the listener are heard where they are drawn: at their rendered pose (§5.2, `ComputeRenderedWorldMatrix` at the frame's alpha), whose position delta over the frame's delta is their velocity (0 for a zero delta). Physics and `OnFixedUpdate` scripts move entities only inside the fixed steps, of which a frame runs 0 to `MaxStepsPerFrame`, so the world pose of a body at a constant speed stands still on some frames and jumps on others; the rendered pose moves at that speed on every frame, so it has no frame-rate Doppler warble and a follow camera placed from `Transform.RenderPosition` hears its ball without a shift. No velocity is reported on the frame a pose stops interpolating (the entity or an ancestor gains `InterpolationResetTag`: a teleport, a respawn, a creation, an enable, or a first write outside the fixed steps; a pose written outside them on every frame, such as a camera in `OnLateUpdate`, keeps its velocity), nor for a move at or above the speed of sound in one frame (a camera cut) (ADR 0016 decision 8). The listener is the first active primary `AudioListener` in canonical order, else the primary camera, else the origin. Clips load through `GetOrPlaceholder` (a missing clip records its diagnostic once and plays the silent clip). `SetPaused` pauses every voice of the system; destroying it is Stop. `InitializeMix` records the engine's Music, Sfx and Ui volumes and sets them to 1 before startup callbacks. `Start` invokes it idempotently for standalone callers, then starts voices without overwriting script-authored gains. Destroying the system restores the original mix even when no voices started, so a game's mix (`Audio.SetGroupVolume`, §11.5) never leaks into Edit mode, the preview or later sessions; the Master volume stays the host's. Session replacement first performs fallible `PlaySession::Prepare` without callbacks or shared audio ownership, retires the old session only after preparation succeeds, and calls `Activate` once on the installed replacement (ADR 0019 decision 10). Audio is not simulated state and never reaches the state hash.

### 10.3 Procedural sound effects
`Audio/SoundSynth` turns a `.sfx` description (§6.6) into PCM deterministically: layers of Sine/Square/Triangle/Saw/Noise oscillators, note sequences (`"C5:0.06"`) or frequency sweeps, ADSR envelopes, optional low-pass, per-layer volume and a seeded noise generator. It runs at import (cooked PCM), so runtime cost is zero. Agents create sounds with `asset.create {type: "SoundEffect", values: {...}}`; ten presets ship as `engine://Audio/*`. The exact semantics (note grammar, whole-frame timing, the sweep's release, the first noise draw, the low-pass span, float-rounded range checks) are those of §6.6 and `SoundSynth.h`; synthesis uses Core/DetMath in double precision, so the presets' committed hashes hold in every configuration and on every platform (ADR 0015 decisions 11 and 23).

### 10.4 Tests
On a device-less engine: a playing clip has non-zero RMS; a source panned hard right has right RMS > left; volume 0 is silent; attenuation is monotonic with distance; streamed and decoded playback of the same clip are sample-identical (no-threading resource manager); captured PCM of a fixed scene is bit-identical across runs; a missing clip yields a diagnostic and silence; synthesis is bit-identical across runs. Device handling uses miniaudio's Null backend (`ma_backend_null`) as the real device plus a fake notification source: a `stopped` notification re-creates only the device, and a voice keeps playing with an unchanged cursor; three failures leave the engine device-less with one warning; in lockstep the device callback reads nothing and voices advance by exactly 800 frames per tick. The fake notification source is `AudioEngine::InjectDeviceNotification` (thread-safe; an injected `Stopped` counts as unexpected), with `InjectDeviceCreationFailures` for re-creations and `AudioEngineSpecification::InjectedDeviceCreationFailures` for the startup creation; the re-creation delays run on `Update`'s clock, so tests pass chosen times. Applications never use them (ADR 0015 decision 4).

---

## 11. Scripting (Luau 0.741)

### 11.1 VM ownership and sandbox

Native conversion of Luau tables into JSON or reflected values has a separate bounded allowance: `min(4 MiB, VM soft-limit bytes)`, charging 256 bytes per expanded value and entry and four bytes per string/key byte. Repeated aliases consume the allowance on every expansion, and traversal polls the inherited watchdog deadline. Exceeding this allowance rejects the input without advancing the VM allocator's breach count (ADR 0019 decision 10).
- **One `lua_State` per play session (and per loaded scene)**, created on Play and closed on Stop or `Scene.Load`, so no state leaks between sessions. Created with `lua_newstate(TrackingAllocator, …)`.
- **Memory limit (soft, with headroom).** `Scripting.MemoryLimitMB` (default 256) is a *soft* limit. The allocator keeps a reserved headroom of 16 MB above it and fails allocations only at the hard limit (soft + headroom), so the error handler, traceback capture and other instances can still allocate after a breach. Crossing the soft limit sets a flag; the next interrupt that is not a GC step (see the watchdog below) raises `script exceeded memory limit 256 MB in Board.OnFixedUpdate` in the current callback. The faulting instance is disabled and a full GC runs. A **second** breach in the same session, or reaching the hard limit at all, stops the session with a structured `memory` error (`play.state` reports it). The VM is never left in an `LUA_ERRMEM`/`LUA_ERRERR` cascade, and the process is never out of memory.
- Libraries: `luaL_openlibs`, then `os` is reduced to nothing (no `os.time`, `os.clock`, `os.date`; `Time.GetRealTime()` is the explicit non-deterministic escape hatch), `math.random`/`math.randomseed` are rebound to the session stream, the transcendental `math` functions are rebound to DetMath (§4.12), the engine API tables are registered, `require` is installed (`luaopen_require` with a `luarequire_Configuration` over the VFS, resolving relative paths inside `Assets/` only, cached per VM, cycle-detected), then `luaL_sandbox(L)` makes globals and libraries read-only. Each script module runs in its own thread (`lua_newthread` + `luaL_sandboxthread`) with private globals.
- **Watchdog.** `lua_callbacks(L)->interrupt` checks a per-callback deadline measured with `lua_clock()`. The budget is `Scripting.CallbackBudgetMs` (default 1,000, minimum 10), applied in every configuration; Dist raises a smaller effective budget to 5,000 ms except in test runs, where per-suite overrides apply exactly (§6.1, §11.10). It is never disabled. **The interrupt returns immediately when `gc >= 0`**, because raising an error from a GC-step interrupt is unsafe; otherwise past the deadline it raises `script exceeded 1000 ms in Board.OnFixedUpdate (possible infinite loop)`. Nested callbacks, such as `OnCreate` running inside `Scene.Instantiate` called from `OnFixedUpdate`, inherit the outer callback's deadline instead of starting a new one, so nesting cannot extend the budget. Task and test-case resumes each get a fresh deadline. An agent-written `while true do end` cannot hang the editor or a shipped game.
- **Values from scripts are validated.** `Lua::Check<T>` rejects non-finite numbers, vectors, quaternions and colours with a located error (`Transform.Translation: value contains NaN`), and component writes enforce `FieldMeta` ranges (§5.4). Nothing a script computes can reach Jolt or the renderer as NaN, Inf, zero scale or a zero radius.
- **Bytecode trust.** Only bytecode compiled by the engine is loaded with `luau_load`; Luau does not verify bytecode, so pak entries are hash-verified before loading (§14.1).
- One OS thread uses the VM (the main thread).

### 11.2 Script shape and fields
```lua
--!strict
export type Ball = ScriptInstance & {
	-- fields (declared below, resolved per instance)
	Torque: number, Goal: Entity?, GoalSound: AssetRef?, Checkpoints: { Entity },
	-- state
	Spawn: vector, ReachedGoal: boolean,
}

local Ball = {}   -- a plain table literal: unsealed in strict mode, so members can be added below

Ball.Fields = {
	Torque = Field.Number(30, { Min = 0, Max = 200, Tooltip = "Roll torque (N*m)" }),
	Goal = Field.Entity(),
	GoalSound = Field.Asset("AudioClip"),
	Checkpoints = Field.Array(Field.Entity()),
}

function Ball.OnStart(self: Ball)
	self.Spawn = self.Entity.Transform.WorldPosition
	self.ReachedGoal = false
end

function Ball.OnFixedUpdate(self: Ball, dt: number)
	local camera = Scene.GetPrimaryCamera()
	if camera == nil then return end
	local forward = camera.Transform:Forward()
	local right = camera.Transform:Right()
	local move = right * Input.GetAxis("MoveX") + forward * Input.GetAxis("MoveZ")
	local body = self.Entity.RigidBody :: RigidBody
	body:AddTorque(vector.cross(vector.create(0, 1, 0), move) * self.Torque)
	if self.Entity.Transform.WorldPosition.y < -20 then
		body:Teleport(self.Spawn)
		body:SetLinearVelocity(vector.zero)
	end
end

function Ball.OnTriggerEnter(self: Ball, other: Entity)
	if other == self.Goal and not self.ReachedGoal then
		self.ReachedGoal = true
		if self.GoalSound then
			Audio.PlayOneShot(self.GoalSound, self.Entity.Transform.WorldPosition)
		end
	end
end

return Script.Define("Ball", Ball)
```
- **The class pattern.** A behaviour builds its class as a local table literal and returns `Script.Define(name, class)`, declared `Define: <T>(name: string, class: T) -> T`. `Script.Define` registers the name and returns the same table, so the module's type is exactly the class literal's inferred type. This avoids adding members to a sealed table returned from a function, which the new solver rejects. The instance type is an exported alias (`ScriptInstance & {fields…, state…}`) used for `self`. The Behaviour, Module and Test templates follow this pattern. TypeChecker fixtures cover each template, a behaviour with every `Field` kind, and the cast idiom below, and CI type-checks every documentation example (§11.9).
- **Cross-script access.** `Entity:GetScript()` returns `any`, because the instance type depends on which script is attached. The idiom requires the other script's module for its type and casts: `local BallScript = require("./Ball")` then `local ball = other:GetScript() :: BallScript.Ball?`. Requiring a behaviour returns its class table, the same one the instances use.
- Field kinds: `Number`, `Integer`, `Bool`, `String`, `Vector`, `Color`, `Quat`, `Entity`, `Asset(type)`, `Enum({...})`, `Array(kind)`. Field kinds map onto reflection `FieldType`s, and `ScriptComponent.Fields` is a `Map<Variant>` resolved against the script's schema (§5.4), so the inspector, serialization and automation (`script.fields`) handle them generically.
- **Script kinds.** The importer classifies each script by what its module returns. A **Behaviour** returns the result of `Script.Define`. A **TestSuite** returns the result of `Test.Suite` (§11.10). Anything else is a **Module**: a plain library such as `Board.luau`, needing no `Script.Define`, loadable only through `require`. Only a Behaviour may be assigned to `ScriptComponent.Script`; anything else raises `SCRIPT_NOT_A_BEHAVIOUR` (validation error, also returned by `entity.update`).
- **Load-time environment.** The `ScriptImporter` classifies the script and extracts the field schema by running the module's top level in a throwaway sandboxed VM. That VM contains every **pure** standard library (`math` with DetMath and a fixed-seed `random`, `string`, `table`, `bit32`, `utf8`, `buffer`, `vector`, `coroutine`), the pure engine modules `Script`, `Field`, `Color`, `Quat` and `Math`, the constructor `Test.Suite`, and a **load-time `require`**. Other engine APIs raise `Input is not available at load time`. The load-time `require` uses the same VFS resolver as the runtime one (relative to the script, inside `Assets/`, cycle-detected), runs each required module once in the same restricted environment, caches it for the duration of this import, and records the edge in `AssetDependencyGraph`. One 250 ms budget covers the whole require graph. A module whose top level calls engine APIs fails import with a located message. Module tops must be free of engine side effects; building local tables (`table.freeze`, string formatting, kick tables) is fine. The `luau-gameplay` skill documents this.
- Overrides in `ScriptComponent.Fields` with an unknown name or wrong type produce a validation diagnostic (`SCRIPT_UNKNOWN_FIELD_OVERRIDE`, `SCRIPT_FIELD_TYPE_MISMATCH`), fall back to the default at run time, and are preserved in the file until fixed.
- An instance is a table `{Entity = <Entity>, <resolved fields>}` whose metatable is the class table (`__index = class`).

### 11.3 Lifecycle callbacks and order
All optional: `OnCreate()`, `OnStart()`, `OnFixedUpdate(dt)`, `OnUpdate(dt)`, `OnLateUpdate(dt)`, `OnDestroy()`, `OnEnable()`, `OnDisable()`, `OnCollisionEnter(other, contact)`, `OnCollisionExit(other)`, `OnTriggerEnter(other)`, `OnTriggerExit(other)`, `OnHotReload()` (EditorOnly: hot reload exists only in the editor, §11.4). Order and timing are defined in §5.7: callbacks run by (`ExecutionOrder`, canonical entity order). Gameplay belongs in `OnFixedUpdate` (deterministic); `OnUpdate`/`OnLateUpdate` are for presentation (cameras, smoothing). `Time.GetDeltaTime()` and input edges are phase-aware (§4.3).

Tasks: `Task.Wait(seconds)`, `Task.WaitTicks(n)`, `Task.Spawn(fn)`, `Task.Delay(seconds, fn) -> TaskHandle`, `Task.Cancel(handle)`, built on `lua_resume`/`lua_yield`, timed in simulation time, resumed at step 4 of §5.7, owned by the instance and cancelled when it is destroyed. `Task.Spawn(fn)` creates a thread (`lua_newthread` + `luaL_sandboxthread`) and resumes it immediately until its first yield or its end. `Task.Delay` creates a thread that first runs when its time is due.

**Yield rule.** Luau raises "attempt to yield across metamethod/C-call boundary" when a coroutine yields with C calls on its stack beyond the resume point (`ldo.cpp`, `nCcalls > baseCcalls`). Callbacks are entered with `lua_pcall` from C++, so a callback body is not yieldable. The engine makes this explicit instead of letting it surface as an obscure VM message. `Task.Wait`, `Task.WaitTicks`, `Test.WaitTicks` and `Test.WaitUntil` check `lua_isyieldable(L)` first and, when it returns false, raise `Task.Wait can only be used inside Task.Spawn, Task.Delay or a Test.Case body` at the caller's line. Bodies of `Task.Spawn`, `Task.Delay` and `Test.Case` always run as resumed threads, where waiting works. A callback that wants a sequence writes `Task.Spawn(function() … Task.Wait(1) … end)`. Unit tests: "Tasks: Task.Wait in OnStart raises the documented error", "Tasks: Task.Wait inside Task.Spawn resumes at the expected tick", "Tasks: yielding inside a metamethod raises the VM error, caught as a script error". The `luau-gameplay` skill states the rule.

### 11.4 Binding layer
- Every API member is registered once, and each registration produces the binding, the `Engine.d.luau` declaration, the docs entry (mandatory description) and a **call counter** (active when test mode is on, in every configuration):
	- module functions: `api.Module("Physics").Function("Raycast", &ScriptBindings::Physics_Raycast, "(origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?", "Casts a ray and returns the closest hit or nil.")`;
	- userdata types: `api.Type("Entity")`, `api.Type("Quat")`, `api.Type("Color")` and one type per component proxy with methods (`RigidBody`, `CharacterController`, `AudioSource`, `Camera`, `MeshRenderer`, `Transform`), each with `.Method(name, fn, signature, description)`, `.Property(name, getter, setter?, type, description)`, `.Operator("__mul" | "__eq" | "__add" | …, fn, signature, description)` and `.Constructor(name, fn, signature, description)` (e.g. `Quat.New`, `Color.FromHex`);
	- component proxy fields come from the type registry (§5.4) and are counted by gate 4;
	- **string-enum parameters** (e.g. `space: Space` of `Transform:Translate`) declare their enum, and each value gets its own counter when the enum has at most 16 values. Larger name tables (`Key`, `GamepadButton`) are covered by table-driven unit tests.
- **Run modes.** Every callback, function, method and property has `RunModes`: `All` (default) or `EditorOnly`. The EditorOnly members are `OnHotReload` and `Test.ReloadScript`, plus any future member that needs the editor. Coverage gates are evaluated per run mode (§15.6). Members whose *behaviour* differs between modes, but which work everywhere (`Application.IsEditor`, `Debug.Break`, `Application.Quit`), are `All`, and their per-mode behaviour is documented and tested in each mode.
- `Bindings/Script.cpp` binds `Script.Define` and every `Field.*` constructor (also present in the load-time VM).
- Typed helpers `Lua::Check<T>(L, index)`/`Lua::Push<T>` give uniform errors: `Entity:AddComponent: argument #2 expected string, got number`; numeric checks also reject NaN and ±Inf (`argument #1 contains NaN`).
- Userdata are tagged (`lua_newuserdatatagged`, one tag per type, metatables via `lua_setuserdatametatable`, destructors via `lua_setuserdatadtor`); method dispatch uses `__namecall` with atoms (`useratom` callback).
- **No pointers cross into Luau.** `Entity` userdata holds `{UUID, SceneGeneration}` and re-resolves on every access; a component proxy holds `{UUID, ComponentTypeIndex}`. Proxies implement `__index`/`__newindex` from the registry (fields validated against `FieldMeta`; writes go through the reflected setter, which patches and notifies systems). Each proxy field read and write increments a `(component, field)` coverage counter in test mode. Transform has hand-written fast paths.
- Value types: 3D vectors are Luau's native **`vector`** (no allocation; library `vector.create/magnitude/normalize/cross/dot/angle/floor/ceil/abs/sign/clamp/max/min/lerp`; fields `.x .y .z`). 2D values (screen positions, `vec2` fields) are `vector` with z = 0. `Quat` and `Color` are tagged userdata with lowercase fields (`.x .y .z .w`, `.r .g .b .a`) to match `vector`. All other names are PascalCase.
- **Enums are strings**, typed in `Engine.d.luau` as singleton unions (`type BodyType = "Static" | "Kinematic" | "Dynamic"`), so the type checker catches typos. This covers `Key`, `MouseButton`, `GamepadButton`, `GamepadAxis`, `CursorMode`, `Space`, `AudioGroup` and every reflected enum.
- **Shortcut properties.** `entity.<ComponentName>` returns the component's proxy, or nil when the component is absent, for every component that is ScriptVisible and not `Hidden`, `EntityLevel` or `NoShortcut`. `ID` and `Relationship` are hidden. `Name` and `Tags` are entity-level, so `.Name` is the entity's name string and tags use the tag methods. `Script` is `NoShortcut`: `GetScript()` returns the instance, and `GetComponent("Script")` returns the component proxy. **Entity members take precedence.** Registration asserts at startup that no shortcut collides with an `Entity` method or property, so a future component cannot shadow one silently.

### 11.5 Full API surface
| Module / type | Members |
|---|---|
| `Script`, `Field` | `Script.Define<T>(name, class: T) -> T` (§11.2); `Field.Number(default?, options?)`, `Field.Integer`, `Field.Bool`, `Field.String`, `Field.Vector`, `Field.Color`, `Field.Quat`, `Field.Entity()`, `Field.Asset(type)`, `Field.Enum(values, default?)`, `Field.Array(fieldKind)`; options `{Min, Max, Step, Tooltip}` |
| `Entity` (userdata) | `.ID` (hex string), `.Name` (rw); `:IsValid()`, `:IsActive()` (effective), `:IsActiveSelf()`, `:SetActive(active)`, `:HasTag(tag)`, `:AddTag(tag)`, `:RemoveTag(tag)`, `:GetTags()`, `:HasComponent(name)`, `:GetComponent(name)` (overloaded per component name in d.luau), `:AddComponent(name, values?)`, `:RemoveComponent(name)`, `:GetScript()` (instance table or nil, typed `any`, §11.2), `:GetParent()`, `:SetParent(parent?, keepWorld?)`, `:GetChildren()`, `:FindChild(name, recursive?)`, `:GetWorldBounds() -> (vector?, vector?)` (render AABB min/max of this entity's `MeshRenderer`, nil without one), `:Destroy()`; shortcut properties `.Transform`, `.RigidBody`, … per the rule in §11.4 (nil when absent); `==` compares UUIDs |
| Component proxies | every scriptable registry field of every component, read/write; plus methods below |
| `Transform` | fields `Translation`, `Rotation`, `Scale`, `EulerAngles`, `WorldPosition` (rw), `WorldRotation` (rw), `WorldScale` (r), `RenderPosition` (r), `RenderRotation` (r) (interpolated, §5.2); `:Teleport(position?, rotation?)` (world pose without interpolation), `:Forward()`, `:Right()`, `:Up()`, `:LookAt(target, up?)`, `:Translate(delta, space?)`, `:Rotate(eulerDegrees, space?)`, `:TransformPoint(p)`, `:InverseTransformPoint(p)`, `:TransformDirection(d)` |
| `RigidBody` | `:AddForce(f)`, `:AddForceAtPosition(f, p)`, `:AddTorque(t)`, `:AddImpulse(i)`, `:AddAngularImpulse(i)`, `:GetLinearVelocity()`, `:SetLinearVelocity(v)`, `:GetAngularVelocity()`, `:SetAngularVelocity(v)`, `:MoveKinematic(position, rotation)`, `:Teleport(position, rotation?)`, `:IsSleeping()`, `:WakeUp()` (velocity setters, forces and impulses need a Dynamic body, §9.3) |
| `CharacterController` | `:Move(velocity)`, `:IsGrounded()`, `:GetGroundNormal()`, `:GetVelocity()` |
| `AudioSource` | `:Play()`, `:Stop()`, `:Pause()`, `:Resume()`, `:IsPlaying()` |
| `Camera` | `:ScreenToWorldRay(x, y) -> (origin, direction)`, `:WorldToScreen(p) -> vector` |
| `MeshRenderer` | `:GetMaterial(index)`, `:SetMaterial(index, asset)` |
| `Scene` | `CreateEntity(name?, parent?)`, `Instantiate(prefab, position?, rotation?, parent?)`, `Destroy(entity)`, `FindByID(id)`, `FindByName(name)`, `FindAllByName(name)`, `FindByPath(path)`, `FindByTag(tag)`, `FindAllByTag(tag)`, `FindAllWithComponent(name)`, `GetPrimaryCamera()`, `GetName()`, `GetEntityCount()`, `Load(scene, parameters?)`, `GetLoadParameters()` |
| `Input` | `IsKeyDown(key)`, `IsKeyPressed(key)`, `IsKeyReleased(key)`, `IsMouseButtonDown(button)`, `IsMouseButtonPressed`, `IsMouseButtonReleased`, `GetMousePosition()`, `GetMouseDelta()`, `GetScrollDelta()`, `IsActionDown(name)`, `IsActionPressed(name)`, `IsActionReleased(name)`, `GetAxis(name)`, `IsGamepadConnected(index)`, `IsGamepadButtonDown(index, button)`, `GetGamepadAxis(index, axis)`, `SetCursorMode(mode)`, `GetCursorMode()` |
| `Time` | `GetDeltaTime()` (phase-aware), `GetFixedDeltaTime()`, `GetTime()` (simulation seconds), `GetTick()`, `GetFrameCount()`, `GetTimeScale()`, `SetTimeScale(scale)`, `GetInterpolationAlpha()` (this frame's render Alpha; 1 inside the fixed phase), `GetRealTime()` (non-deterministic; documented) |
| `Physics` | `Raycast(origin, direction, maxDistance, layerMask?) -> RaycastHit?` (`{Entity, Point, Normal, Distance}`), `RaycastAll(...) -> {RaycastHit}`, `SphereCast(origin, radius, direction, maxDistance, layerMask?)`, `OverlapSphere(center, radius, layerMask?) -> {Entity}`, `OverlapBox(center, halfExtents, rotation?, layerMask?)`, `GetBodyBounds(entity) -> (vector?, vector?)`, `GetColliderBounds(entity) -> (vector?, vector?)` (§9.5), `GetGravity()`, `SetGravity(g)`, `LayerMask(...names) -> number`; hits are `{Entity, Body, Point, Normal, Distance}` (`Entity` = collider entity, `Body` = owner) |
| `Audio` | `PlayOneShot(clip, position?, volume?, group?)` (non-spatial when `position` is nil; group `Sfx` by default), `SetGroupVolume(group, volume)`, `GetGroupVolume(group)` (the session's mix through `AudioSystem::SetGroupVolume`: reset at the session's start and restored at its end, §10.2) |
| `Assets` | `Load(reference) -> AssetRef?`, `IsValid(ref)`, `GetPath(ref)`, `GetType(ref)` |
| `Task` | `Wait(seconds)`, `WaitTicks(n)`, `Spawn(fn)`, `Delay(seconds, fn) -> TaskHandle`, `Cancel(handle)` |
| `Random` | `Seed(n)`, `Integer(min, max)`, `Number(min?, max?)`, `Bool(probability?)`, `Choice(array)`, `Shuffle(array)`, `UnitVector()`, `New(seed) -> RandomGenerator` (same methods) |
| `Math` | `Clamp`, `Clamp01`, `Lerp`, `InverseLerp`, `Remap`, `SmoothStep`, `SmoothDamp(current, target, velocity, smoothTime, dt) -> (value, velocity)`, `MoveTowards`, `Approximately`, `DeltaAngle`, `LerpAngle`, `Sign`, `Repeat`, `PingPong`, `Deg2Rad`, `Rad2Deg`, `Pi` |
| `Quat` | `New(x, y, z, w)`, `Identity()`, `FromEuler(x, y, z)` (deg), `AngleAxis(degrees, axis)`, `LookRotation(forward, up?)`, `Slerp(a, b, t)`; `:Inverse()`, `:ToEuler()`, `:Normalized()`, `:Rotate(v)`; `*` (quat × quat, quat × vector); fields `.x .y .z .w` |
| `Color` | `New(r, g, b, a?)`, `FromHex("#RRGGBB[AA]")`, `Lerp(a, b, t)`; fields `.r .g .b .a` |
| `Debug` | `DrawLine(a, b, color?, duration?)`, `DrawRay(origin, direction, color?, duration?)`, `DrawBox(center, halfExtents, rotation?, color?, duration?)`, `DrawSphere(center, radius, color?, duration?)`, `DrawArrow(from, to, color?, duration?)`, `DrawText(position, text, color?, duration?)`, `Break()` (pauses play in the editor; no-op in the Runtime; in test runs a counted no-op in every mode, reported in the results) |
| `Log` | `Trace(...)`, `Info(...)`, `Warn(...)`, `Error(...)`; `print` → `Log.Info`; entries carry script, line and entity |
| `Application` | `Quit(exitCode?)` (stops play in the editor; exits the Runtime; in test runs ends the current suite with a recorded `quit(exitCode)`, §11.10), `IsEditor()`, `IsHeadless()`, `IsFocused()`, `GetPlatform()`, `GetVersion()`, `GetWindowSize() -> vector` |
| `Test` (test runs only; `Test.Suite` also at load time) | `Suite(name, body, options?) -> TestSuite`, `Case(name, fn, options?)` (only inside a suite body; options `{TimeoutTicks}`), `Expect(condition, message?)`, `ExpectEqual(a, b, message?)`, `ExpectNear(a, b, epsilon?, message?)`, `Fail(message)`, `Skip(reason)`, `WaitTicks(n)`, `WaitUntil(predicate, timeoutTicks)`, `InjectAction(name, state, value?)`, `InjectKey(key, state)`, `InjectMouse(button, state, position?)`, `InjectGamepad(index, buttonOrAxis, stateOrValue)`, `Screenshot(name)`, `GetScriptErrors(since?) -> {ScriptError}`, `ExpectScriptError(pattern, withinTicks)`, `CaptureAudio(ticks) -> {RmsLeft, RmsRight, Peak}`, `ReloadScript(asset)` (EditorOnly), `GetStateHash() -> string`, `GetLastExtraction() -> {Alpha, Frame}`, `GetExtractedPosition(entity) -> vector?` |

### 11.6 Creating, destroying, spawning
Immediate creation and deferred destruction as defined in §5.7. `Scene.Instantiate` uses the shared prefab path (§5.5) with a root ID from the session's seeded generator. `AddComponent(name, values)` validates values against the registry before adding.

### 11.7 Error reporting
Every callback runs under `Lua::ProtectedCall` with an error handler that captures `lua_debugtrace` and maps chunk names to project paths. Every task and test-case resume goes through `Lua::ProtectedResume`, which takes the traceback from the dead thread's stack after `lua_resume` returns an error status; `lua_resume` takes no error handler, and the errored thread keeps its frames. Both produce a structured `ScriptError`:
```json
{ "id": 17, "kind": "runtime", "script": "Assets/Scripts/Board.luau", "line": 88, "column": 0,
  "message": "attempt to index nil with 'Transform'", "callback": "OnFixedUpdate",
  "entity": { "id": "8f3a2c1d9e4b7a60", "name": "Game" }, "tick": 412, "count": 1,
  "traceback": [ { "script": "Assets/Scripts/Board.luau", "line": 88, "function": "LockPiece" } ] }
```
`kind` ∈ {`compile`, `type`, `runtime`, `timeout`, `memory`}. Errors are deduplicated by (script, line, message) with a `count`, logged to the `Script` logger, appended to the script error stream (`script.errors`, `_meta`, console, `EventLog`, `Test.GetScriptErrors`), and the faulting **instance is disabled** (no further callbacks), so one bad script cannot spam at 60 Hz while others keep running. With `Scripting.PauseOnError` (default true) the editor pauses play at the first runtime error so an agent lands on the frozen state. The Runtime logs, disables the instance and continues. In test runs an error in a behaviour is recorded for the suite and fails the running case unless that case expects it (`Test.ExpectScriptError`); an error inside a case body fails that case at the error's location.

### 11.8 Hot reload
Edit mode: a changed script is recompiled and re-checked (and every script that requires it is re-extracted, §7.5); the inspector refreshes the field schema. Play mode (editor only; lockstep, replay, recording and test sessions defer reloads, §7.5): if compilation fails the old code keeps running and a `compile` error is raised. On success:
1. **Behaviours** re-execute and **the class table's contents are replaced in place**. Instances' metatable is the class table, so every instance gets the new methods while its state survives; new fields get their defaults; `OnHotReload(self)` runs if defined.
2. **Modules** (plain `require`d scripts) re-execute. If the old and new results are both tables, the old table's contents are replaced in place with the new ones, so every holder of the table sees the new functions. Then **every dependent** (transitively, in dependency order: modules first, then behaviours) re-executes the same way. Locals that a dependent captured from the module (`local new = Board.new`) are therefore re-bound. If a module returns a non-table value, its dependents are re-executed to pick it up. The VM's require cache is updated to the patched table.
3. The reload is one step: a failure anywhere in the chain (compile or top-level error) rolls back to the previous code for the whole chain and raises one error naming the failing script.

The editor queues published script handles and processes them at its safe point after UI, RPC and batch dispatch have returned. It finishes dependent imports before visiting the dependency order, deduplicates overlapping chains and reports failed imports without replacing the running code. Ending a deferred session drains the released import chain before a subsequent Start can read its assets (ADR 0019 decision 10).

`Test.ReloadScript(asset)` (EditorOnly) triggers the same path synchronously from a test, so `Callbacks.scene` asserts `OnHotReload` in the editor run. Tests: "HotReload: behaviour state survives", "HotReload: editing a required module updates its dependents' behaviour", "HotReload: a failing dependent rolls back the chain", "HotReload: deferred while lockstep owns time".

### 11.9 Static type checking
`ScriptTypeChecker` (EditorCore and Tests only; the Runtime links only the VM, compiler and require) wraps `Luau::Frontend` with `SolverMode::New`, a `FileResolver` over the project VFS (the same resolution as runtime `require`), a `ConfigResolver` reading `.luaurc` (default `--!strict`), `registerBuiltinGlobals`, `loadDefinitionFile` with `Resources/Scripting/Engine.d.luau`, then `freeze`. Calls are wrapped in `try`/`catch (const Luau::InternalCompilerError&)`. Diagnostics are `{file, line, column, endLine, endColumn, code, severity, message}` (1-based). It runs on save, in `script.check`, in `project.validate` and in CI (`Editor --headless --check-scripts`). Type errors always block export and test runs; they block Play only when `Scripting.BlockPlayOnTypeErrors` is true (default false, so agents can iterate on partial scripts while seeing diagnostics in `_meta`). All Analysis API use is confined to one `.cpp`, pinned to the vendored release.

`Engine.d.luau` is generated from the registries using Luau 0.741's current definition syntax (`declare class` is being removed upstream):
```lua
declare extern type Entity with
	ID: string
	Name: string
	Transform: Transform
	RigidBody: RigidBody?
	function IsValid(self): boolean
	function GetScript(self): any
	function GetComponent(self, name: "RigidBody"): RigidBody?
	function GetComponent(self, name: "Transform"): Transform?
	function GetWorldBounds(self): (vector?, vector?)
end
declare Script: {
	Define: <T>(name: string, class: T) -> T,
}
declare Scene: {
	FindByName: (name: string) -> Entity?,
	Instantiate: (prefab: AssetRef, position: vector?, rotation: Quat?, parent: Entity?) -> Entity,
}
```
`GenerateDocs.py --check` fails CI when the committed file is stale, and every Luau example in `Docs/Reference/ScriptAPI.md` and in this document's §11 is extracted and type-checked in CI (the project modules an example requires, such as `Board`, are provided as stubs under `Tests/Data/TypeChecker/DocExamples/`). TypeChecker fixtures (`Tests/Data/TypeChecker/`) pin the expected diagnostics for the three templates, a behaviour declaring every `Field` kind, the `GetScript` cast idiom, a test suite, and seeded mistakes (an optional passed to a non-optional parameter, a member added to a sealed table).

### 11.10 Test scripts and the test runner
**Shape.** A test script (`*.test.luau`, created from the `Test` template) is a module that returns one suite:
```lua
--!strict
local Board = require("../Scripts/Board")
local GameScript = require("../Scripts/Game")

return Test.Suite("Tetris", function()
	Test.Case("four lines clear for 800 points", function()
		local board = Board.New()
		board:FillRows(0, 3, 4)   -- rows 0-3 full except column 4
		board:Place("I", 4, 0, 1)
		Test.ExpectEqual(board:ClearLines(), 4)
		Test.ExpectEqual(Board.Score(4, 1), 800)
	end)

	Test.Case("hard drop locks the piece", function()
		local game = Scene.FindByName("Game")
		Test.Expect(game ~= nil, "Main.scene has a Game entity")
		local state = (game :: Entity):GetScript() :: GameScript.Game
		local locked = state.LockedCount
		Test.InjectAction("HardDrop", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(state.LockedCount, locked + 1)
	end, { TimeoutTicks = 120 })
end)
```
- `Test.Suite(name, body, options?)` is a pure constructor (available in the load-time VM, §11.2) returning `{Name, Body, Options}`, so the module top stays side-effect free. The importer classifies the script as a `TestSuite`. A TestSuite cannot be attached to an entity (`SCRIPT_NOT_A_BEHAVIOUR`).
- Suite options accept `CaseTimeoutTicks` (positive integer) as the default for cases without their own `TimeoutTicks`; otherwise the project's `Testing.CaseTimeoutTicks` applies. Unknown options are errors. `Test.ExpectScriptError(pattern, withinTicks)` claims one nonfatal occurrence observed during the current case, preserving logs/counts; unmatched faults fail the case, and setup/teardown, fatal and case-body faults cannot be claimed. Its pattern uses Luau string-pattern rules. These host and option details are recorded in ADR 0019.
- `Test.Case(name, fn, options?)` only registers a case. It is valid only while the runner executes a suite body, and raises `Test.Case outside Test.Suite` anywhere else. The `Test` module exists only in test runs; in normal play every member except `Suite` raises `Test is only available in test runs`.

**Binding to scenes.** The project's `Testing` settings list suites and replays:
- `Suites: [{Script, Scene?, Parameters?, Isolation?, Clock?, Overrides?, ExpectQuit?, Modes?}]`. `Scene` is the scene the suite runs in. If it is omitted, the suite runs in an **empty scene** (no entities), which is right for pure-logic suites such as Board tests. `Parameters` become `Scene.GetLoadParameters()`. `Isolation` is `"Suite"` (default: cases share one scene instance, in order) or `"Case"` (the scene is reloaded before each case). `Clock` is a list of frame deltas that selects `ScriptedClock` (§4.2) instead of lockstep `ManualClock`. `Overrides` holds `{CallbackBudgetMs?, MemoryLimitMB?, PauseOnError?}`, applied exactly (no Dist minimum) for that suite. `ExpectQuit` is the exit code the suite must end with through `Application.Quit`. `Modes` is a subset of `["Editor", "Release", "Dist"]` (default: all).
- `Replays: [globs]` (`.replay` files, §6.7). `CaseTimeoutTicks` (default 600) and `SuiteTickLimit` (default 36,000) bound every case and suite.

A scripted clock also uses the current case's effective `TimeoutTicks` as a limit on consecutive completed frames with no fixed step. Frame callbacks run before the check, allowing a finite pause to recover; any stepped frame resets the counter. Reaching the limit times out the case at its source location, cancels its thread and capture, and closes the suite. Tick counts and tick budgets still measure actual simulation ticks. An all-zero authored clock is rejected at setup (ADR 0019 decision 10).

**Execution** (`Engine/Testing/FeatureTestRunner`, shared by every entry point). For each suite in list order:
1. Start a fresh `PlaySession` of the suite's scene, in test mode with the project seed and lockstep `ManualClock` (or the suite's `ScriptedClock`), and apply its overrides. Run session setup (`OnCreate`, `OnStart`).
2. Call the suite body in the play VM to collect cases, in declaration order.
3. Run the cases **sequentially**. Each case body runs as its own thread (`lua_newthread`, resumed at step 4 of every tick, §5.7), so `Test.WaitTicks`/`WaitUntil` work. A case ends when its thread returns (passed, unless an `Expect*` failed), raises (error, located at the raising line), calls `Test.Fail` (failed) or `Test.Skip` (skipped), or exceeds its tick timeout (timeout, located at the line where it was waiting). `Expect*` failures are recorded with the caller's file and line, and the case continues so one run reports every failed expectation.
4. With `Isolation: "Case"`, the scene restarts before the next case. After the last case the session is destroyed. **The scene is always restarted between suites.** A case requesting Scene.Load must return before frame-end VM replacement; a still-suspended case becomes a located error. Suite isolation re-collects the same ordered case identities/options in the new VM and continues at the next case. Changed collection is an error; no coroutine survives a destroyed VM (ADR 0019 decision 6).
5. `Application.Quit(code)` during a suite ends that suite immediately with the event `quit(code)`. It passes only if `ExpectQuit` equals `code`; otherwise the running case fails. A quit first raised by final teardown is also reported and compared with `ExpectQuit`; the first request is classified exactly once. `Debug.Break` is a counted no-op in test runs.

Replays then run one by one, each in a fresh session started from its header (§6.7): events are applied by tick, every `Expect` is evaluated at its tick (bytecode from the cooked replay in exported runs), and the final state hash is recorded.

Replay cancellation drops future authored events before releasing the session's input/time lease. Already-applied input remains at the paused boundary: an applied Tap retains its next-tick release, and held buttons/axes remain until subsequent input or `input.inject releaseAll`. Independent input is preserved. The player validates all events before starting and queues them only at their execution tick (ADR 0019 decision 10).

**One result schema** for `FeatureTestRunner`, `test.run`, `Editor --run-tests` and exported `--feature-test`, written to `bin/TestResults/<run>.json` and JUnit:
`{suite, case, status: passed|failed|error|timeout|skipped|quit, message, file, line, ticks}` per case (replays use `suite = "replay:<path>"`, `case = "Expect@<tick>"`), plus per-suite `{suite, scene, mode, ticks, breaks, scriptErrors, finalStateHash}` and the run's coverage counters (§15.6). **`filter`** (in `test.run`, `--run-tests --filter`) is a case-insensitive substring matched against `"<suite>/<case>"`, so `"Tetris"` selects a suite and `"Tetris/hard drop"` one case. Replays match on their path.

The runner is part of the `Testing` module, which stays in Dist, so exported Release and Dist runtimes execute exactly the same suites and replays (§14.4). The test template, a documentation example and a TypeChecker fixture keep this shape type-checked.

---

## 12. Editor

### 12.1 Structure
- **`EditorCore`** (static library, no ImGui, linked by `Editor` and `Tests`): `EditorContext` (project, **one** open scene (v1 edits a single scene at a time; opening another requires saving or discarding, §13.5), `SelectionSet`, `CommandHistory`, `PlaySession`, diagnostics, editor camera state), `ProjectManager` (create from template, open, recent list in `user://Editor.json`), commands (§12.3), `AutomationServer` and all editor method handlers (`EditorCore/Automation/*Methods.cpp`), `Exporter`, `ScriptTypeChecker`, `ProjectValidator` (§13.7), `Autosave`.
- **`Editor`** (executable): `EditorApp`, `EditorLayer`, panels, gizmo, drawers. Panels read `EditorContext` every frame and never treat ImGui state as truth; every mutation goes through `EditorContext::Execute(Scope<Command>)`, the same entry point automation uses.
  M10 keeps CPU camera, gizmo-preview, reflected-edit, viewport and UI-state logic in EditorCore for tests without ImGui; the panels, ImGuizmo adapter and drawers remain in Editor (ADR 0018). A gizmo gesture changes an owned scene preview, then commits once on release; a conflicting edit cancels it before any live write. Preview state is excluded from simulation and autosave.
- CLI: `Editor [--project <path.eproj>] [--read-only] [--headless] [--renderer vulkan|none] [--automation[=port]] [--batch f.jsonl] [--run-tests [--filter F] [--junit out.xml]] [--check-scripts] [--validate] [--upgrade] [--export <dir> --config Dist|Release [--testing] [--smoke-test]] [--bake-engine-assets] [--dump-reference] [--gpu-validation] [--vulkan-api=1.3] [--timeout <s>]`. Without `--project`, the windowed editor shows the `ProjectLauncher`, and `--automation` starts in the **launcher state**: only `session.*`, `rpc.discover`, `docs.get`, `project.create` and `project.open` are available, and every other method returns `InvalidState` ("no project open").

### 12.2 Panels
| Panel | Function |
|---|---|
| `SceneHierarchyPanel` | tree with drag reparent/reorder, multi-select, search, active toggles, prefab instance badges |
| `InspectorPanel` | registry-driven drawers; Add Component menu by category; prefab override highlighting with per-field revert; material and import-settings editing for the selected asset |
| `SceneViewportPanel` | `EditorCamera` (orbit Alt+LMB, pan MMB, zoom wheel, fly RMB+WASD), ImGuizmo translate/rotate/scale (W/E/R) in local/world space with snapping (Ctrl: 0.5 m, 15°, 0.1), GPU picking, outline, grid, icons, collider view, debug-view dropdown, asset drag-drop (mesh → entity, prefab → instance, HDR → Environment). Viewport icons (camera, lights, audio, triggers) and content-browser type icons are first-party vector glyphs drawn with `DebugDrawList`/`ImDrawList` primitives (`Editor/Icons.cpp`), so no icon image set or license is needed |
| `GameViewportPanel` | primary camera at fixed or free resolution; receives game input when focused; "Agent controls time" banner in lockstep |
| `ContentBrowserPanel` | folder tree + grid, offscreen-rendered cached thumbnails, create (folder, scene, material, script, prefab, sound effect), rename/move (handles survive), delete to `Library/Trash/` (undoable), OS file drop import, import-error badges |
| `ConsolePanel` | ring-buffer log + script errors, level/logger filters, click opens file:line in the external editor and selects the entity |
| `DiagnosticsPanel` | asset, validation and type-check diagnostics with auto-fix buttons |
| `ProjectSettingsPanel` | window, simulation, physics layers and collision matrix, input actions, export, scripting (reflected `ProjectSettings`) |
| `StatsPanel` | frame time, per-pass GPU time, draw counts, memory, bodies, voices, Luau heap |
| `AutomationPanel` | connected clients, live request log with timings, pause-agent toggle, deny-mutations switch, "Allow AI automation" preference (lets agents attach to this editor, §13.8) |
| `UndoHistoryPanel` | command labels, `[agent]` prefix for automation-originated commands |
| `ProjectLauncher` | recent projects, New from template (Empty, Basic3D), Open; ImGui folder picker (no per-OS dialog code, automatable) |

### 12.3 Commands and undo
```cpp
class Command
{
public:
	virtual ~Command() = default;
	virtual Status Execute(EditorContext& context) = 0;   // atomic: fully applies or leaves state unchanged
	virtual void Undo(EditorContext& context) = 0;        // restores the exact prior state
	virtual std::string_view GetLabel() const = 0;
	virtual bool MergeWith(const Command& next) { return false; }   // gizmo drags, slider scrubs
	CommandOrigin Origin = CommandOrigin::User;           // User | Agent
};
```
Almost every operation uses one generic implementation, **`SceneEditCommand`** (snapshot undo): `SceneEdit edit(context, "Reparent")` begins tracking, the operation mutates the scene through the normal `Scene`/`Entity` APIs, and `edit.Commit()` builds the command from the scene's change tracker: for each touched entity, the canonical JSON before and after (absent = created or destroyed) plus parent and sibling index. Undo and redo restore through the heavily tested serializer. Debug builds verify after each commit that the state hash of every untouched entity is unchanged. Other command types: `AssetEditCommand` (file bytes before/after, for materials, sound effects, scripts written through automation and import settings), `ProjectSettingsCommand` (settings JSON before/after), `AssetMoveCommand`, `AssetDeleteCommand` (moves file + `.meta` to `Library/Trash/`; never a permanent delete) and `CompositeCommand` (transactions and `edit.batch`; atomic: if a child fails, executed children are undone in reverse).

Continuous edits begin on `IsItemActivated` or gizmo mouse-down and commit on release (one undo step). `CommandHistory` is bounded at 1,000 entries or 256 MB, keeps a save point for the dirty flag, and records `Scene::GetRevision()` before and after each command. Property tests: for random sequences of 10,000 operations over random scenes, undo-all restores byte-identical JSON and redo-all reproduces the final state; per command type, Execute → Undo equals the original and Execute → Undo → Redo equals Execute.

### 12.4 Play mode, assets, projects
- Toolbar: Play, Simulate, Pause, Step, Stop; lockstep indicator. Behaviour per §5.6. Play is blocked only by Error-severity asset diagnostics (and type errors when configured).
- Ordinary Play consumes `Application.Quit` at the editor safe point through the normal Stop path. Test, replay, recording and lockstep drivers retain control of their sessions. Script environment queries use the live borrowed window, including its headless state; cursor changes use that window and restore the editor baseline on Stop or replacement. Focus loss releases capture. Native input and viewport focus-loss input changes respect exclusive test/replay ownership, while authored test input remains effective (ADR 0019 decision 10).
- Autosave and recovery per §4.13.
- New-project templates write the folder skeleton `Assets/{Scenes,Scripts,Prefabs,Materials,Models,Textures,Audio,Fonts,Tests}`, `.luaurc`, `.gitignore` (`Library/`), an `AGENTS.md` stub and, for Basic3D, a scene with camera, sun, environment, post-process and ground. Script templates: `Behaviour` (the class pattern of §11.2), `Module` (a plain table module), `Test` (a suite, §11.10); each is type-checked by a fixture.
- `SceneChangedOnDisk` (§7.5) shows a banner offering reload; it never reloads silently.

### 12.5 Editor design quality

The editor's functional gates and its product design are separate acceptance obligations (ADR 0020). Editor changes must be reviewed in actual rendered, populated screens and exercised through the corresponding editing workflow. Screenshot comparisons guard a reviewed design against regressions; an unchanged screenshot alone is not a design approval.

- Use the committed Inter font, shared `Editor/Ui` style and a restrained dark palette. Size controls from font/style metrics, keep keyboard focus visible and apply display scaling to both text and spacing without compounding it across monitor changes. Account for framebuffer scaling so Retina displays do not apply the monitor scale twice. Game input ownership suspends UI keyboard navigation until focus returns to the editor.
- Distinguish the menu, primary toolbar, panel tools and status. The project, current scene, unsaved state and play mode must be understandable without opening a diagnostic panel. Friendly panel titles are presentation names; automation names remain stable, and existing saved layouts are migrated when their ImGui IDs change.
- Inspector labels precede their controls in aligned rows. Ordinary transforms expose position, Euler rotation and scale; raw quaternion and derived values remain available under advanced disclosure. Show readable entity and asset names with paths/identifiers on demand. Removal and other occasional actions belong in component menus rather than competing with every editable field.
- Browsing scenes, importing assets and opening projects must have discoverable selection flows. Import and project-opening pickers also accept typed paths. Empty, busy, failed, read-only and agent-controlled states explain what happened and what the user can do next.
- Review populated and empty screens at 1280×720 and 1600×900, and at a larger display scale. Required controls must remain reachable at narrow widths through wrapping or scrolling. Review selection, property editing and undo, asset browsing, scene opening and Play/Stop with the production command paths.

---

## 13. AI automation

### 13.1 Topology
```
Claude Code ──MCP (stdio)──▶ Tools/MCP bridge (Python, official mcp SDK) ──JSON-RPC 2.0 / TCP 127.0.0.1──▶ Editor AutomationServer
                              launch / attach / supervise / relaunch            I/O thread: framing, auth
                              catalog from the method registry                  main thread: Pump() → MethodRegistry
                              crash reports, output capping, transcript         → Commands / PlaySession / Renderer
```
The engine owns a stable, semver'd JSON-RPC protocol; MCP lives only in the thin Python bridge, so MCP revisions are absorbed by the pinned official SDK, the bridge survives editor crashes, and the same protocol serves Python tests, batch files and the Runtime.

### 13.2 Protocol, transport, security
- **Framing:** LSP-style `Content-Length: N\r\n\r\n<UTF-8 JSON>`; frames over 64 MB, invalid UTF-8 or nesting depth over 128 close the connection. A first line that is not a `Content-Length` header (e.g. `GET / HTTP/1.1`) closes the socket immediately, which defeats browser-based localhost attacks and DNS rebinding.
- **Transport:** TCP bound to `127.0.0.1` only, port 0 (OS-assigned) or `--automation=<port>`. The editor writes `<UserData>/<Product>/Automation/Sessions/<pid>.json` `{pid, port, token, protocolVersion: "1.0", engineVersion, projectPath, headless, startedAt}` with user-only permissions (0600 on POSIX; per-user `%LOCALAPPDATA%` ACL on Windows); deleted on clean exit; stale files (dead pid) are ignored.
- **Handshake:** the first request must be `session.hello {token, protocolVersion, client: {name, version}}`. The 256-bit token (OS CSPRNG) is compared in constant time; 3 failures close the connection; a major-version mismatch is rejected listing both versions.
- **Availability:** the server runs only with `--automation`, `--headless` or the editor preference "Allow AI automation". It does not exist in Dist builds.
- **Threading:** the I/O thread decodes frames into a queue; `AutomationServer::Pump` executes requests on the main thread between input and update, so handlers see a consistent frame and need no locks. Up to 4 clients; requests from one client run in order; mutations from all clients serialize through the single `CommandHistory`.
- **Disconnect.** When a client's connection closes, cleanly or not, the server cancels that client's pending operations (each resolves internally as `Cancelled`). If the client owned lockstep, it releases lockstep and pauses play, so the session never stays locked in "Agent controls time". It stops any recording that client started, marking the file invalid and not writing it, and logs one `AutomationClientDisconnected` event. Python tests cover a kill during `play.step`, during `input.replay` and while recording.
- **Pending operations:** handlers return `MethodResult = std::variant<nlohmann::json, Error, Scope<PendingOperation>>`; `PendingOperation::Poll(MethodContext&) -> std::optional<Result<nlohmann::json>>` runs once per frame until it resolves (`play.step`, `play.waitFor`, `asset.import`, `project.export`, `test.run`, `input.replay`), with optional `$/progress` notifications.
- **Watchdog:** if the main thread has not pumped for 5 s, the I/O thread itself answers new requests with `Busy` (−32007) including the main thread's current phase from an atomic phase marker (`"Script:Board.OnFixedUpdate"`, `"AssetImport:Track.glb"`). An agent never hangs on a frozen editor.
- **Paths:** every path argument resolves through `VfsPath` inside `project://`. Runtime recording is a narrow exception because its project mount is a read-only pak: relative `input.record` output paths are confined below `user://Replays/`, and Runtime `input.replay` also accepts the returned canonical path there. Absolute paths, traversal and other schemes remain rejected (ADR 0019 decision 7). `asset.import` may read an absolute source path (read-only). It copies the importable file and, for a `.gltf`, its **dependency closure**: every buffer and image URI, which must be relative and inside the source's directory tree (no `..`, no absolute paths, no schemes other than `data:`). Relative paths are preserved under `destDir`, and each copied dependency gets a dependency meta (§6.4). Nothing else is copied. `project.export` writes only under `<Project>/Build/` or `--export-root` given on the command line. No method executes processes, shell commands or native code; `script.eval` runs in the Luau sandbox.

### 13.3 Error codes
JSON-RPC standard codes plus: −32000 `Internal`, −32001 `NotFound`, −32002 `InvalidState`, −32003 `ValidationFailed`, −32004 `Conflict`, −32005 `Timeout`, −32006 `ScriptError`, −32007 `Busy`, −32008 `Unauthorized`, −32009 `Unsupported`, −32010 `Cancelled`. `error.data` carries `errorCode` (the engine `ErrorCode` name) and `issues[]`, each `{pointer, message, hint?, suggestions?}`:
```json
{"jsonrpc":"2.0","id":8,"error":{"code":-32602,"message":"Invalid params","data":{"errorCode":"Validation",
 "issues":[{"pointer":"/components/RigidBody/Mas","message":"unknown field 'Mas' on 'RigidBody'","hint":"did you mean 'Mass'?"},
           {"pointer":"/components/RigidBody/Friction","message":"must be >= 0 (got -0.2)"}]}}}
```

### 13.4 Conventions every method follows
- Names `domain.verb`; params and results are JSON objects with **camelCase** keys; embedded component/asset data keeps registry PascalCase. Enum values are parsed case-insensitively and echoed in canonical PascalCase (§6). Each method's params and result are **reflected C++ structs**, so parsing, defaults, validation and the JSON Schemas (2020-12, with `$defs` per component) are generic; there is no hand-written JSON plumbing. Every method has a description, `exposeAsTool`, `mutates`, `supportsDryRun`, `availableInRuntime`, `availableInLauncher` and `timeoutSeconds` metadata.
- **`EntityRef`** accepts a 16-hex UUID, a unique UUID prefix (≥ 6 hex digits) or a path (`/Game/Board`, `/Track/Piece[3]`); ambiguous paths fail and list candidates. **`AssetRef`** per §7.1. Responses always include canonical ids plus readable names/paths.
- **Target.** `target: "edit" | "play"`. Reads default to the play scene while playing. Mutations of the play scene must say `target: "play"`; they are transient (no undo) and the response says so.
- **Undo.** An edit-scene mutation is exactly one `Command` per call (one `CompositeCommand` per `edit.batch`), labelled `[agent] …`; the result includes `undoIndex`.
- **`dryRun: true`** validates and returns the would-be result without applying it, and is accepted only by methods with `supportsDryRun`. Others return `Unsupported`; this includes `asset.import`, `project.export`, `play.*`, `input.*` and `test.run`. Scene mutations execute then undo inside a sandboxed history. **File-writing methods** (`asset.create`, `asset.setProperties`, `script.write`, `script.create`, `prefab.create`, `project.setSettings`) write into an `OverlayMount` (§4.10) layered over `project://` for the duration of the call: parsing, compiling, type-checking, registry updates and `$ref` results all happen in memory, and nothing touches the disk, the file watcher, the trash or provenance. `edit.batch {dryRun: true}` rejects a batch containing an op without `supportsDryRun` before running anything, reporting `failedOp: k` and `Unsupported`.
- **`ifRevision: N`**: optimistic concurrency; if the scene changed since, the call fails with `Conflict` and the current revision.
- **Atomic batch.** `edit.batch {label, ops: [{method, params}], dryRun?}` runs inside one `CompositeCommand`; if op *k* fails, ops 0..k−1 are undone and the error reports `failedOp: k`. `{"$ref": "<opIndex>.<jsonPath>"}` substitutes earlier results (e.g. `"3.entity.id"`).
- **Diagnostics delta.** Every response carries `_meta: {revision, dirty, undoLabel?, tick?, playState, diagnostics: {newErrors, newWarnings, newScriptErrors, logCursor, firstNew: [≤ 3 entries]}}`, so an agent learns about problems without polling.
- **Bounded output.** Lists take `limit` (default 100, max 1,000) and `cursor`; any result over 48 KB is written to `Library/Automation/Out/<id>.json` and returned as `{path, truncated: true, summary}`. Screenshots are downscaled to `maxDimension` (default 1024).
- **Provenance.** The editor records every file it writes under `Assets/` and the `.eproj` in the **committed** `Projects/<Game>/Automation/Provenance.json`: one entry per path, `{path, xxh64, method, requestId, client, transcriptLine}`, where `transcriptLine` is the line of the request in `BuildLog.jsonl` (§13.8). Entries are kept sorted by path and written canonically. Writes by the windowed UI are recorded with `method: "ui"`; the audit rejects these for demo projects. Dry runs never record. `project.upgrade` records its rewrites with `method: "project.upgrade"` (§13.12).

### 13.5 Method list (86)
| Domain | Methods |
|---|---|
| session (3) | `session.hello`, `session.info` (versions, capabilities, project, play state, renderer, lockstep owner, read-only flag), `session.shutdown {save?, force?}` |
| rpc (1) | `rpc.discover {method?, domain?}` (descriptions, schemas, flags, examples) |
| project (10) | `project.create {path, name, template}`, `project.open {path, recover?}`, `project.save` (the open scene if dirty, dirty native assets and settings), `project.info`, `project.getSettings`, `project.setSettings {patch}` (RFC 7386 merge patch, undoable), `project.refreshAssets`, `project.validate {scope?, fix?: true \| [diagnosticIds \| codes]}` (each diagnostic has a stable `id`; `fix: true` applies every safe fix), `project.upgrade {dryRun?}` (applies migrations and canonical re-saves to every project file, records each rewrite in provenance and the transcript, reports changed files), `project.export {config, outDir?, smokeTest?, testing?}` |
| scene (8) | `scene.new {path, template?}`, `scene.open {path, save?, discardChanges?, reload?, repair?}` (with a dirty open scene exactly one of `save`/`discardChanges` is required, else `InvalidState`; `reload` adopts a `SceneChangedOnDisk` version; `repair` loads with structural repairs and reports them, §6), `scene.save {path?}`, `scene.tree {root?, depth?, format: text|json}`, `scene.query {where: {name?, tag?, component?, path?}, select?, limit?, cursor?}`, `scene.get {target?}` (full JSON, offloaded when large), `scene.diff {against: saved|revision, revision?}` (RFC 6902 per entity), `scene.raycast {origin, direction, maxDistance, layerMask?}` |
| entity (7) | `entity.create {name, parent?, index?, active?, tags?, components?}`, `entity.get {entity, components?: [...]|"all", children?}`, `entity.update {entity, name?, active?, tags?, components?: {Type: {Field: value}}, removeComponents?: []}` (adds missing components), `entity.destroy {entities}`, `entity.duplicate {entities}`, `entity.reparent {entity, parent, index?, keepWorld?}`, `entity.bounds {entities}` (world AABBs) |
| component (2) | `component.list`, `component.schema {name}` (fields, types, ranges, defaults, enum values, script methods, docs) |
| edit (6) | `edit.batch`, `edit.undo {steps?}`, `edit.redo {steps?}`, `edit.history {limit?}`, `edit.select {entities}`, `edit.getSelection` |
| asset (11) | `asset.list {dir?, type?, recursive?}`, `asset.info {asset}` (metadata, dependencies, dependents, diagnostics), `asset.import {source, destDir, settings?}`, `asset.reimport {asset}`, `asset.create {type: Material|Scene|Prefab|SoundEffect|Folder, path, values?}` (a sound effect needs at least one layer; invalid values of a material or a sound effect are `ValidationFailed` located under `/values`, here and in `asset.setProperties`), `asset.getProperties {asset}`, `asset.setProperties {asset, values}` (native assets: materials, sound effects), `asset.getImportSettings`, `asset.setImportSettings`, `asset.move {asset, path}`, `asset.delete {asset}` (to trash, undoable) |
| prefab (5) | `prefab.create {entity, path, replaceWithInstance?}`, `prefab.instantiate {prefab, parent?, transform?, name?}`, `prefab.apply {instance}`, `prefab.revert {instance, overrides?}`, `prefab.unpack {instance}` |
| script (7) | `script.create {path, template: Behaviour|Module|Test}`, `script.read {path}`, `script.write {path, source}` (undoable; triggers compile + check; diagnostics in the result), `script.check {paths?}`, `script.errors {since?, limit?}`, `script.fields {script}`, `script.eval {code, context: edit|play, entity?}` (returns value + captured prints; edit context is read-only) |
| play (8) | `play.start {mode: play|simulate, lockstep?, seed?, scene?, parameters?, paused?, timeScale?, pauseOnError?}`, `play.stop`, `play.pause`, `play.resume`, `play.step {ticks, input?: [events], render?: every|last|none}` (result includes `tick` and `stateHash`), `play.state` (mode, tick, `stateHash`, lockstep owner, `modified`, recording), `play.waitFor {until: luauExpr, timeoutTicks}`, `play.setTimeScale {scale}` |
| input (3) | `input.inject {events, releaseAll?}`, `input.record {action: start|stop, scene?, parameters?, seed?, restart?, path?, expect?}` (§13.6), `input.replay {path, verify?, strictHash?}` (result includes per-`Expect` outcomes, `finalTick` and `stateHash`) |
| viewport (5) | `viewport.screenshot {view: scene|game, width?, height?, camera?, debugView?, annotate?, maxDimension?, inline?}`, `viewport.camera {position?, target?}` (get/set editor camera), `viewport.frame {entities}`, `viewport.setOptions {grid?, gizmos?, colliders?, icons?, wireframe?}`, `viewport.pick {x, y, view}` |
| editor (2) | `editor.screenshot {maxDimension?}` (whole editor UI), `editor.state` (open panels, mode, selection, lockstep owner) |
| observe (6) | `log.read {cursor?, minLevel?, loggers?, contains?, limit?}`, `events.read {cursor?, types?}`, `stats.get`, `physics.bodyInfo {entity}` (`{entity, body, origin, type, isSensor, layer, colliders, position, rotation, linearVelocity, angularVelocity, sleeping, boundsMin, boundsMax, contacts: [{other, collider, otherCollider, isTrigger, sinceTick}], grounded, groundNormal, tick}`; reads the play session: InvalidState in Edit mode, NotFound at `/entity` for an entity without a body), `audio.stats` (device state and name, time source, sample rate, master and group volumes, voice count and capacity, stolen and pulled frames, and every voice with its handle, clip, owning entity, group, volume, pitch, loop, spatial, position, paused, streamed, priority, cursor and length; Unsupported when the host has no audio engine), `docs.get {topic?}` (generated reference and skills; topic list when omitted) |
| test (2) | `test.list` (suites with their cases, replays), `test.run {filter?, timeoutTicks?, record?}` (results in the §11.10 schema, failures with locations, coverage report, JUnit path; `record: path` requires the filter to select exactly one suite and writes a replay of all input applied during it, §13.6) |

Both screenshot methods return `{path, mimeType, width, height}`; `viewport.screenshot` adds `view` and, with `inline`, PNG base64 `data` when the PNG is at most 30 KB. Larger images report `inlineOmitted: true` so the result stays below the offload threshold. Output PNGs outlive the server, including a `--batch` run. Viewport captures use the requested camera, target scene, project rendering quality, debug view and strictly validated annotations. All M9 data views are supported. `editor.screenshot` is a pending operation: it requests and waits for a UI frame completed after admission, then reads back the whole UI with its embedded viewport textures. This includes earlier changes in the same automation pump. A minimized editor renders that fresh frame offscreen at the last usable extent, without acquiring a swapchain image. Cancellation releases the request; failure to publish a fresh frame within the method's 60-second deadline returns Timeout. Both methods return Unsupported with `--renderer none` (ADR 0017 and ADR 0018).

**Runtime subset** (Runtime Debug/Release with `--automation`; handlers shared from `Engine/Automation/Methods`): `session.*`, `rpc.discover`, `scene.tree`, `scene.query`, `scene.get`, `scene.raycast`, `entity.get`, `entity.bounds`, `play.pause`, `play.resume`, `play.step`, `play.state`, `play.waitFor`, `play.setTimeScale`, `input.*`, `viewport.screenshot` (game view), `log.read`, `events.read`, `stats.get`, `physics.bodyInfo`, `audio.stats`, `script.errors`, `script.eval` (play context). Handlers reach the host's audio engine through `AutomationMethodContext::GetAudioEngine()` (the Runtime's comes from `RuntimeAutomationServerSpecification::Audio`; ADR 0015 decision 15).

### 13.6 Lockstep, input injection, replays, batches
- `play.start {lockstep: true, seed: 42}` decouples simulation from the wall clock; ticks advance only through `play.step` (one tick = fixed step + frame phase). `play.step {ticks: 600, render: "last"}` fast-forwards 10 s of game time and renders one frame. A `play.step` is a pending operation that runs as many ticks per editor frame as fit a **50 ms budget** (at least one), so the UI, the watchdog and other clients stay responsive during long fast-forwards. Its result carries the final `tick` and `stateHash`. The UI keeps drawing and shows "Agent controls time". Lockstep is owned by one client; others get `InvalidState`. A disconnect releases it (§13.2). Audio advances by simulation time while lockstep owns time (§10.1).
- Input events are stamped with a tick offset relative to the step start: `{"tick": 0, "type": "action", "name": "MoveLeft", "state": "tap"}` (`tap` = down on that tick, up on the next, so both edges are observed). Types: `action {state | value}`, `key`, `mouseButton {position?}`, `mouseMove`, `mouseDelta`, `scroll`, `gamepadButton`, `gamepadAxis {value}`, `text`. Type and state names are case-insensitive; files use PascalCase (§6.7).
- `play.waitFor {until: "return Scene.FindByName('GameOver'):IsActive()", timeoutTicks: 3600}` evaluates the predicate in the play VM after every tick and returns `{satisfied, tick, value}`.
- **Recording** always starts at tick 0, so a replay can reproduce it. `input.record {action: "start", scene?, parameters?, seed?}` is accepted in Edit mode: it starts a lockstep play session of the scene (default: the open scene) with the given load parameters and seed (default: the project seed), and records from tick 0. While playing, `start` requires `restart: true`, which restarts the session at tick 0 with the recorded seed; otherwise it fails with `InvalidState` ("recording must start at tick 0"). The recorder captures **every event applied at step 1** of each tick (§5.7), whatever its source: `play.step` input, `input.inject`, `Test.Inject*` from test threads, and real devices in the game viewport. `input.record {action: "stop", path, expect?}` writes the replay with its full header (§6.7) and `FinalStateHash`. A hot reload or a play-scene mutation during recording invalidates it (`stop` then fails with the reason).
- **Replays** are checked with `input.replay {verify: true}`: a fresh session of the header's scene starts with its parameters and seed, events are applied by tick, every `Expect` must hold, and `strictHash: true` also compares `FinalStateHash`. They double as regression tests (§15.7) and run in exported builds through the cooked replay player (§14.4).
- **Autopilot workflow** (documented in the `game-building` skill). Steering a physics ball blind with tick-stamped injections is impractical, so the agent writes a test-only **autopilot suite**, `Assets/Tests/Autopilot/Level1.test.luau`, bound to the level scene with `Modes: ["Editor"]`. Its single case reads the ball's state, follows an authored list of waypoint entities with a PD controller, and injects analog `MoveX`/`MoveZ` actions with `Test.InjectAction` every tick until the goal is reached. The case only reads the scene and injects input. It never mutates entities and never uses the session's `Random` stream, so the run is identical to a player pressing the same inputs. `test.run {filter: "Autopilot.Level1", record: "Assets/Tests/Replays/Level1.replay"}` records the run. The replay is then verified **without** the autopilot (`input.replay {verify: true}` and `Testing.Replays`), which proves that plain input reaches the goal. `Level2` and `Level3` suites set `Parameters` (e.g. the carried timer), which the replay header stores.
- **Recording publication** (ADR 0019 decision 6). `test.run {record}` verifies the candidate in fresh ordinary gameplay without the test driver, including the original final hash, before writing. It preserves all input from case-isolated segments with cumulative tick offsets, without encoding resets. A non-reproducible start, clock grouping or reset returns located Validation with no recording file; ordinary test runs remain supported. Verification is cancellable and counts toward the explicit run-wide tick limit, not case ticks or coverage.
- **Re-recording.** When an engine change legitimately alters simulation results (a physics or gameplay fix), the agent re-runs the autopilot suites with `test.run {record}` through automation, so provenance and the transcript record the new replays. Replays without an autopilot (Tetris openings) are re-recorded by repeating the recorded `play.step` inputs from the transcript. The change's review must state why the hashes changed.
- Batch files per §6.7.

### 13.7 Observability
- **Screenshots** (§8.13): fresh frame after `WaitIdle`; `annotate: {labels: "all"|"selection"|[ids], colliders, bounds, axes}` draws entity names and short ids so a vision model can tie pixels to entities. The MCP tool returns an image content block plus the saved path.
- **`scene.tree {format: "text"}`**, the token-efficient outline:
```
Main.scene  rev 58  dirty  14 entities
├─ Camera      7c1f09a2  Transform Camera(Ortho 11)              (4.5, 9.5, 20)
├─ Sun         09bb1e2f  Transform DirectionalLight              (0, 10, 0)
└─ Game        8f3a2c1d  Transform Script(Scripts/Game.luau)     (0, 0, 0)
   ├─ ScoreText 1d2e3f4a Transform Text("Score 0")
   └─ … 3 children (depth limit)
```
- **Logs and events** are structured with source locations and entity context, read by cursor so nothing is lost between polls. **Script errors** are deduplicated with tracebacks and pause play on the first error; the agent then inspects with `script.eval {context: "play", entity: "/Game", code: "return self:BoardToAscii()"}`.
- **`scene.diff {against: "saved"}`** gives the agent a precise self-review. `entity.bounds` and `physics.bodyInfo` support layout and physics reasoning (gaps between track pieces); in scripts and test suites the same questions are answered by `Entity:GetWorldBounds()`, `Physics.GetColliderBounds()` and `Physics.Raycast` (§11.5).
- **`project.validate`** returns `{id, severity, code, message, entity?, component?, field?, asset?, file?, line?, hint, autoFixable}`. `fix: true` applies every safe fix, and `fix: [ids or codes]` applies only the selected ones (the editor's per-item buttons use the same path); either way the fixes form one undoable command. Codes (generated into `Docs/Reference/Validation.md`): `SCENE_NO_PRIMARY_CAMERA`, `SCENE_MULTIPLE_PRIMARY_CAMERAS`, `SCENE_DUPLICATE_UNIQUE_COMPONENT`, `SCENE_NONCANONICAL_ORDER`, `SCENE_INVALID_HIERARCHY` (dangling or cyclic parent), `SCENE_INCONSISTENT_PREFAB_LINK`, `ENTITY_DUPLICATE_ID`, `ENTITY_DANGLING_REFERENCE`, `COMPONENT_FIELD_OUT_OF_RANGE`, `COMPONENT_MISSING_REQUIREMENT`, `COMPONENT_CONFLICT`, `ASSET_MISSING`, `ASSET_TYPE_MISMATCH`, `ASSET_IMPORT_FAILED`, `ASSET_ORPHAN_META`, `ASSET_DUPLICATE_HANDLE`, `ASSET_ORPHAN_DEPENDENCY`, `ASSET_TANGENTS_APPROXIMATED`, `ASSET_UNSUPPORTED_UV_SET`, `ASSET_VERTEX_COLORS_IGNORED`, `PATH_CASE_MISMATCH`, `PREFAB_MISSING_ASSET`, `PHYSICS_NONCONVEX_DYNAMIC`, `PHYSICS_MIXED_TRIGGER`, `PHYSICS_DYNAMIC_TRIGGER`, `PHYSICS_ALL_DOFS_LOCKED`, `PHYSICS_INVALID_SHAPE`, `PHYSICS_ADJACENT_STATIC_BODIES`, `PHYSICS_UNKNOWN_LAYER`, `PHYSICS_NONUNIFORM_SCALE`, `PHYSICS_DYNAMIC_UNDER_MOVING_PARENT`, `PHYSICS_LIMIT_EXCEEDED`, `SCRIPT_COMPILE_ERROR`, `SCRIPT_TYPE_ERROR`, `SCRIPT_NOT_A_BEHAVIOUR`, `SCRIPT_UNKNOWN_FIELD_OVERRIDE`, `SCRIPT_FIELD_TYPE_MISMATCH`, `INPUT_UNKNOWN_ACTION` (string literals passed to `Input.*Action*`/`GetAxis`, found by AST scan), `AUDIO_NO_LISTENER`, `AUDIO_MULTIPLE_PRIMARY_LISTENERS`, `RENDER_NO_LIGHTING`, `RENDER_LIGHT_LIMIT_EXCEEDED` (more than 256 visible lights), `RENDER_SPOT_SHADOW_BUDGET` (more than 8 shadowed spot lights), `BUILD_START_SCENE_MISSING`, `BUILD_SCENE_MISSING`, `TEST_SUITE_INVALID` (a `Testing.Suites` entry whose script is not a TestSuite or whose scene is missing). Severities where the list leaves them open: the physics codes as ADR 0014 decision 9 gives them (`PHYSICS_ADJACENT_STATIC_BODIES`, a warning fixable by a Static `RigidBody` on the nearest common ancestor unless it is missing or has trigger colliders of its own; `PHYSICS_NONUNIFORM_SCALE` and `PHYSICS_DYNAMIC_UNDER_MOVING_PARENT`, warnings; `PHYSICS_LIMIT_EXCEEDED`, a warning past 90 % of a limit and an error at it; the others errors whose body is not created, except `PHYSICS_UNKNOWN_LAYER`, whose body goes to layer 0); `AUDIO_MULTIPLE_PRIMARY_LISTENERS`, a warning fixable in the open scene by clearing `Primary` on each extra listener, and `AUDIO_NO_LISTENER`, an unfixable warning (ADR 0015 decision 12); `RENDER_LIGHT_LIMIT_EXCEEDED`, an unfixable warning on the scene file that counts its effectively enabled lights (ADR 0013 decision 7).
- **`stats.get`**: measured FPS, CPU frame and per-pass CPU/GPU timings, dropped simulation time, entity/body/voice counts and native Vulkan allocation counts. Scene and game views have independent histories; transient screenshot captures never replace them. Exact decimal frame strings distinguish current CPU work from delayed GPU samples, with explicit availability flags when no sample exists. Reading stats never waits for the GPU or advances simulation. Luau heap usage and soft/hard limits come from the live session's allocator; they are explicitly unavailable without a live VM (ADR 0019 decision 9).
- **State hash** is reported by `play.state`, `play.step` and `input.replay` (and `Test.GetStateHash()` in scripts), so acceptance checks such as "repeated runs give identical hashes" need no evaluation trick.

### 13.8 MCP bridge (`Tools/MCP`)
- Python 3.10+, the **official `mcp` Python SDK** (low-level server API, dynamic tool registration), hash-pinned in `requirements.lock`, installed into `Tools/MCP/.venv` by `Setup.py`. Registered for Claude Code in `.mcp.json`, which `Setup.py` **generates** (gitignored) with the absolute path of the venv interpreter: `{"mcpServers": {"engine": {"type": "stdio", "command": "<repo>/Tools/MCP/.venv/Scripts/python.exe", "args": ["<repo>/Tools/MCP/run.py"]}}}` (`.venv/bin/python` on POSIX). No `python` on `PATH` is assumed, which matters on Windows, where only the `py` launcher may exist. `run.py` also works when started by any other interpreter (`py -3 Tools/MCP/run.py`, `sys.executable`): it locates the venv and re-executes itself inside it, or fails with "run python Scripts/Setup.py".
- Package: `server.py` (tool registration), `connection.py` (framing, handshake, reconnect; reuses `Tools/Automation/engine_client.py`), `launcher.py` (finds `bin/<cfg>-<sys>-<arch>/Editor/Editor(.exe)`, preferring Release; spawns `--automation --project …`; a missing binary returns an error naming `python Scripts/Build.py --config Release --project Editor`), `transcript.py`, `catalog.json` (generated by `--dump-reference`; tools are listed before the editor runs; `GenerateDocs.py --check` keeps it current).
- **Launch or attach.** Before spawning, `editor_launch` scans `Sessions/*.json` (§13.2) for a live editor whose `projectPath` matches. If one exists and its "Allow AI automation" preference is on (so its server is listening), the bridge **attaches** to it instead of spawning; two processes never write the same project. If there is no matching session file but `Library/Editor.lock` is held (an editor without automation enabled), `editor_launch` fails with `EditorAlreadyOpen {pid}` (pid read from the lock file) and the hint "enable Allow AI automation in the editor's Automation panel, then call editor_attach". The `Library/Editor.lock` check (§4.13) is the backstop for any other launcher. `editor_attach {project? | pid?}` attaches explicitly. A bridge never supervises or kills an editor it attached to; `editor_shutdown` on an attached editor only disconnects.
- **Compact tool schemas.** MCP `inputSchema`s stay small: component and asset values are free-form objects (`{"type": "object", "description": "Field values by registry name; see component_schema"}`) instead of inlined `$defs` for every component, so the tool list does not flood the agent's context. Full schemas remain available through `rpc.discover` and `component_schema`, and the editor validates every call against them anyway.
- **Tools.** Bridge-local: `editor_launch {project, create?, template?, headless?, renderer?}`, `editor_attach {project?, pid?}`, `editor_status`, `editor_shutdown`, `engine_methods {domain?}`, `engine_call {method, params}` (escape hatch to all 86 methods). Proxied (methods flagged `exposeAsTool`, named `domain_verb`): `project_create`, `project_open`, `project_save`, `project_get_settings`, `project_set_settings`, `project_validate`, `project_export`, `scene_new`, `scene_open`, `scene_save`, `scene_tree`, `scene_query`, `scene_diff`, `entity_create`, `entity_get`, `entity_update`, `entity_destroy`, `entity_duplicate`, `entity_reparent`, `entity_bounds`, `component_list`, `component_schema`, `edit_batch`, `edit_undo`, `edit_redo`, `asset_list`, `asset_import`, `asset_create`, `asset_set_properties`, `asset_move`, `asset_delete`, `prefab_create`, `prefab_instantiate`, `prefab_apply`, `script_create`, `script_read`, `script_write`, `script_check`, `script_errors`, `script_eval`, `play_start`, `play_stop`, `play_step`, `play_wait_for`, `input_inject`, `input_record`, `input_replay`, `viewport_screenshot`, `editor_screenshot`, `log_read`, `test_run`, `docs_get` (52 proxied + 6 local; `project.upgrade` is reached through `engine_call`).
- Results return as `structuredContent` plus a short text summary that always includes the `_meta` delta; text over 48 KB is truncated with a pointer to the offloaded file; screenshots return image content, read from the PNG the result names (or its inline `data`), and the summary names the file.
- **Resilience.** The bridge supervises the editor it launched. On unexpected exit the next call returns `EditorCrashed {exitCode, lastLogLines (50), crashReportPath, autosaveAvailable}`; `editor_launch` then offers recovery. Calls time out after each method's `timeoutSeconds` (default 60).
- **Transcript.** The bridge **always** appends every request and a response summary as JSONL to `<ProjectDir>/Automation/BuildLog.jsonl` of the project it launched or attached to, and passes each request's transcript line number to the editor (request field `_meta.transcriptLine`), which stores it in provenance (§13.4). Nothing depends on remembering an environment variable; `ENGINE_MCP_TRANSCRIPT=<path>` only redirects the file (Python tests use it to keep fixtures clean).

### 13.9 Headless and batch modes
`Editor --headless` initializes GLFW on the null platform and renders offscreen with the same renderer and the full editor UI (so `editor.screenshot` works); `--renderer none` serves logic-only CI and makes screenshot methods return `Unsupported`. GPU-dependent imports return `Unsupported` there unless a cooked bake exists (§7.4). `--batch f.jsonl` replays RPC calls and exits non-zero at the first error. The non-Dist engine option `--audio-device system|null|none` chooses the audio device and keeps the window mode's decoding; windowed Editor and Runtime processes that tests start pass `--audio-device none` (C++ through `Test::WithoutAudioDevice`), so no test opens a real audio device (§15.1; ADR 0015 decision 23). All headless runs use `ManualClock`, and headless play without lockstep is throttled to `FixedHz` (§4.2). `Runtime [--headless] [--frames N] [--expect-no-errors] [--screenshot-at tick:path] [--feature-test [--filter F]] [--automation[=port]] [--replay R --verify] [--gpu-validation]`. Dist honours only `--headless`, `--frames`, `--expect-no-errors`, `--feature-test`, `--filter` and `--log-level`. Dist has no standalone `--replay`, but `--feature-test` runs every suite **and every cooked replay** listed in the manifest's testing data through the same runner (§11.10).

### 13.10 Agent-facing documentation and skills
- `AGENTS.md`: build/test commands, module rules, error policy, style, the commit gate and review checklist. `CLAUDE.md` contains `@AGENTS.md`.
- Skills (`.claude/skills/<name>/SKILL.md` with `name`/`description` frontmatter): **build-and-test**, **add-component**, **add-script-api**, **add-automation-method** (each lists files to touch, generated artifacts to refresh and the FeatureTest coverage to add), **golden-images** (update policy and review), **commit-review** (§15.9; not named `code-review`, which would collide with the generic skill Claude Code ships), **game-building** (the end-to-end MCP workflow and definition of done; `Testing.Suites` setup; the autopilot-and-record workflow and replay re-recording of §13.6; the shared static body per level for tracks), **luau-gameplay** (idioms: the `Script.Define(name, class)` pattern, the `GetScript() :: Other.Type?` cast, fields, tasks, state machines, grid logic, DAS/ARR input, follow cameras on `RenderPosition` in `OnLateUpdate`, pooling with `SetActive`, `Transform:Teleport` for respawns; pitfalls: module tops without engine side effects (what the load-time VM offers), `Task.Wait` only inside `Task.Spawn`/`Task.Delay`/`Test.Case`, phase-aware input, strict-mode `self` typing, nil-guarding optional fields, `math.pow` instead of `^` for non-trivial exponents), **game-debugging** (the observability loop and an error-code → fix table).
- Generated reference in `Docs/Reference/`, served by `docs.get`.

### 13.11 Walkthrough: an agent builds Tetris
1. **Launch.** `editor_launch {project: "Projects/Tetris", create: true, template: "Empty"}` (it attaches instead if the user already has that project open with automation allowed, §13.8); `docs_get {topic: "skills/game-building"}`. From here on the bridge writes `Projects/Tetris/Automation/BuildLog.jsonl` and the editor writes `Automation/Provenance.json`.
2. **Settings.** `project_set_settings` merge patch: window "Tetris" 720×900, `FixedHz` 60, seed, `StartScene: "Assets/Scenes/Main.scene"`, `Export.BuildScenes: ["Assets/Scenes/Main.scene"]`, actions `MoveLeft`, `MoveRight`, `SoftDrop`, `HardDrop`, `RotateCW`, `RotateCCW`, `Hold`, `Restart` (keyboard + gamepad), and `Testing.Suites: [{Script: "Assets/Tests/Board.test.luau"}, {Script: "Assets/Tests/Tetris.test.luau", Scene: "Assets/Scenes/Main.scene"}]`, `Testing.Replays: ["Assets/Tests/Replays/*.replay"]`. `BUILD_START_SCENE_MISSING` stays in `_meta` until step 3 creates the scene.
3. **Content and scene.** `scene_new {path: "Assets/Scenes/Main.scene"}`, then one atomic `edit_batch` labelled "Scaffold Tetris":
```json
{ "label": "Scaffold Tetris", "ops": [
	{ "method": "asset.create", "params": { "type": "Material", "path": "Assets/Materials/I.material", "values": { "BaseColor": [0.1, 0.8, 0.9, 1], "Roughness": 0.35, "Emissive": [0.02, 0.08, 0.09] } } },
	"… six more piece materials (O, T, S, Z, J, L), Wall, Ghost (AlphaMode Blend) …",
	{ "method": "asset.create", "params": { "type": "SoundEffect", "path": "Assets/Audio/Lock.sfx", "values": { "Layers": [ { "Wave": "Square", "Notes": ["A3:0.05"] } ] } } },
	{ "method": "entity.create", "params": { "name": "Camera", "components": { "Transform": { "Translation": [4.5, 9.5, 20] }, "Camera": { "Projection": "Orthographic", "OrthographicSize": 11, "Primary": true } } } },
	{ "method": "entity.create", "params": { "name": "Environment", "components": { "Environment": { "Environment": "engine://Environments/Studio", "Intensity": 0.6 }, "PostProcess": { "Tonemap": "AgX", "BloomIntensity": 0.06 } } } },
	{ "method": "entity.create", "params": { "name": "Sun", "components": { "Transform": { "EulerAngles": [-50, -30, 0] }, "DirectionalLight": { "Intensity": 3, "LightAngle": 2 } } } },
	{ "method": "entity.create", "params": { "name": "Game" } },
	{ "method": "entity.create", "params": { "name": "WallLeft", "parent": { "$ref": "13.entity.id" }, "components": { "Transform": { "Translation": [-1, 9.5, 0], "Scale": [1, 21, 1] }, "MeshRenderer": { "Mesh": "engine://Meshes/Cube", "Materials": [ { "$ref": "7.asset.id" } ] } } } },
	"… WallRight, Floor, ScoreText / LevelText / LinesText / NextLabel (Text, Screen), GameOverText (active false) …"
] }
```
4. **Prefab.** `entity_create` "Cell" (cube mesh, scale 0.95), `prefab_create {entity: "/Cell", path: "Assets/Prefabs/Cell.prefab"}`, `entity_destroy {entities: ["/Cell"]}`.
5. **Logic.** `script_write` for `Assets/Scripts/Pieces.luau` (Module: tetromino tables, SRS kick data, frozen with `table.freeze`), `Assets/Scripts/Board.luau` (Module: pure 10×22 grid with no engine calls: collision, line clears, scoring, `ToAscii`), `Assets/Scripts/Game.luau` (Behaviour requiring `Pieces` and `Board`, which the load-time VM resolves during import (§11.2): 7-bag on `Random.New(seed)`, gravity by tick counters per level, lock delay, DAS 10 ticks / ARR 2 ticks on actions, a pool of 220 cell entities toggled with `SetActive` and recoloured with `MeshRenderer:SetMaterial`, ghost piece, hold, line clears with `Audio.PlayOneShot`, levels, game over and restart) and `Assets/Scripts/Hud.luau`. Each `script_write` result carries compile and type diagnostics; the agent iterates until `script_check {}` reports zero.
```lua
function Game.OnFixedUpdate(self: Game, dt: number)
	if self.GameOver then
		if Input.IsActionPressed("Restart") then self:Reset() end
		return
	end
	self:HandleShift()
	if Input.IsActionPressed("RotateCW") then self:TryRotate(1) end
	if Input.IsActionPressed("HardDrop") then self:HardDrop() return end
	self.GravityTicks += if Input.IsActionDown("SoftDrop") then 20 else 1
	if self.GravityTicks >= Board.GravityTicksForLevel(self.Level) then
		self.GravityTicks = 0
		if not self:TryMove(0, -1) then self:BeginLockDelay() end
	end
end
```
6. **Wire-up.** `entity_update {entity: "/Game", components: {Script: {Script: "Assets/Scripts/Game.luau", Fields: {CellPrefab: "Assets/Prefabs/Cell.prefab", PieceMaterials: [...], ScoreText: "/Game/ScoreText", LockSound: "Assets/Audio/Lock.sfx"}}}}`; `_meta` confirms every override matched a declared field.
7. **Look.** `viewport_screenshot {view: "game", annotate: {labels: "all"}}`; adjust the camera with `entity_update`.
8. **Verify deterministically.** `play_start {lockstep: true, seed: 42}`; `play_step {ticks: 1}` (`_meta` shows 0 errors); `script_eval {context: "play", entity: "/Game", code: "return self.Board:ToAscii()"}` shows the spawned piece; `play_step {ticks: 3, input: [{tick: 0, type: "action", name: "MoveLeft", state: "tap"}]}` then eval: the piece column decreased by 1; `play_step {ticks: 120}` checks gravity against the level-1 interval; eval seeds a nearly full row and forces an I piece, `HardDrop` tap, `play_step {ticks: 2}`, eval asserts `Lines == 1` and `Score == 100`; `viewport_screenshot`; `play_stop`.
9. **Tests.** `script_create {template: "Test"}` + `script_write` for two suites (§11.10): `Assets/Tests/Board.test.luau` (pure logic in the empty scene: kicks at both walls and the floor, clears of 1–4 lines and their scores, top-out, 7-bag distribution, hold once per piece) and `Assets/Tests/Tetris.test.luau` (bound to `Main.scene`: integration cases with `Test.InjectAction`, `Test.WaitUntil` and `Test.GetStateHash`). Then, in Edit mode, `input_record {action: "start"}` (the session starts at tick 0), play an opening with `play_step` + input events, and `input_record {action: "stop", path: "Assets/Tests/Replays/Opening.replay", expect: [{tick: 600, luau: "return (Scene.FindByName('Game') :: Entity):GetScript().Lines >= 1"}]}`. Finally `test_run {filter: ""}` until green: it runs every suite and verifies every replay.
10. **Ship.** `scene_save`; `project_validate` must be clean; `project_export {config: "Dist", smokeTest: true}` (the exported executable runs `--headless --frames 300 --expect-no-errors` and must exit 0); the agent updates `Projects/Tetris/AGENTS.md` (documentation, outside the audit).

**Rolling Ball 3D** follows the same loop. Layers are `Ball`, `Track` and `Trigger`. Track pieces are built from scaled primitives with collider-only box colliders and saved as prefabs (`Straight`, `Ramp`, `Curve` segments, `Narrow`, `Gap`). The kinematic `MovingPlatform` prefab carries its own `Kinematic` `RigidBody` driven by `MoveKinematic`. Each level has a root entity `Level` with a **`Static` `RigidBody`**, so every track piece instantiated under it joins one static compound, and the ball rolls over seams without bumps (§5.3, §9.2; `project_validate` would report `PHYSICS_ADJACENT_STATIC_BODIES` otherwise). Each level is laid out as a single `edit_batch` of about 60 `prefab.instantiate` ops chained with `$ref`, plus an ordered list of `PathNode` waypoint entities along the main route and `Gap` markers where jumps are intended. `entity_bounds` gives the agent layout feedback. The ball prefab is a Dynamic sphere with `LinearCast`, `EnhancedInternalEdgeRemoval`, `MaxAngularVelocity` 120 and camera-relative torque. A follow camera in `OnLateUpdate` uses `Math.SmoothDamp` toward `ball.Transform.RenderPosition` (§5.2). Checkpoint and goal triggers sit under the level root as implicit sensor bodies. A kill plane respawns the ball at the last checkpoint with `RigidBody:Teleport` and a zero velocity. The game also has a HUD timer, synthesized sounds, and `Scene.Load(next, {Time = ...})` between three levels. Tests: `RollingBall.test.luau` (respawn at the last checkpoint, platform carry, timer carried across `Scene.Load`, goal loads the next level, and a **main-path support check** that samples the `PathNode` polyline every 0.25 m and requires a downward `Physics.Raycast` hit on the `Track` layer everywhere except inside `Gap` markers, reporting the first unsupported point). One autopilot suite per level records a replay with `test_run {record}` (§13.6). Each replay must then reach the goal under `input_replay {verify: true}` in lockstep without the autopilot.

### 13.12 Demo acceptance rule
For Tetris and Rolling Ball (and the Breakout dry run), every engine-consumed file under `Projects/<Game>/` (`*.eproj`, `Assets/**`, `*.meta`) must have been written by the editor through automation. Everything the audit needs is committed: `Projects/<Game>/Automation/Provenance.json` (§13.4) and the transcript `Projects/<Game>/Automation/BuildLog.jsonl` (§13.8). Nothing in the gitignored `Library/` is consulted. `Scripts/AuditProvenance.py` passes when, for every engine-consumed file:
1. `Provenance.json` has an entry whose `xxh64` equals the file's current hash;
2. that entry was produced by an automation method (never `ui`), and its `requestId` appears at its `transcriptLine` in `BuildLog.jsonl` with the same method; or it was produced by `project.upgrade`, which is itself in the transcript;
3. no provenance entry refers to a file that no longer exists, unless the transcript records its deletion or move.

**Keeping the audit reproducible across engine changes.** An engine change that alters the canonical form of project files (a new component field written with its default, a migration, a `.meta` schema change) makes the byte-identical re-save test (§6) fail for the demo projects. The same change must therefore run `project.upgrade` on each affected project through automation (`Editor --headless --project P --upgrade` runs the same method and appends its own transcript line, marked `client: "cli"`). That rewrites the files, appends provenance entries and transcript lines, and keeps both the audit and the re-save test green. `project.validate {fix}` and replay re-recording (§13.6) are likewise automation calls, recorded the same way. Documentation (`AGENTS.md`, `README.md`) is exempt. Any capability an agent finds missing becomes an engine change (method + test) before the demo continues. T5 runs the audit on every commit (§15.1).

---

## 14. Runtime and export

### 14.1 Package format
```
Build/Windows-Dist/Tetris/
  Tetris.exe                         Runtime (chosen config), renamed; icon + version resource patched from Export.Icon/Version/Company
  vcruntime140.dll vcruntime140_1.dll msvcp140.dll   app-local CRT (staticruntime "off")
  Game.json                          manifest
  Data/Engine.pak                    target-config SPIR-V, built-in meshes/textures/fonts/sfx, blue noise, baked default HDRIs
  Data/Game.pak                      cooked project assets (Luau bytecode with line info, no source)
  THIRD_PARTY_NOTICES.txt            generated from Vendor/*/VENDOR.md + Resources/LICENSES.md
Linux:  Tetris + Data/ + Game.json, RPATH $ORIGIN; system libvulkan.so.1 required (clear error if missing)
macOS:  Tetris.app/Contents/{MacOS/Tetris, Resources/{Game.json, Data/*.pak},
        Frameworks/{libvulkan.1.dylib, libMoltenVK.dylib}, Resources/vulkan/icd.d/MoltenVK_icd.json}; ad-hoc signed;
        Runtime linked with -Wl,-rpath,@executable_path/../Frameworks so the bundled loader is found
```
```json
{
	"Format": "GameManifest", "Version": 1, "Name": "Tetris", "EngineVersion": "0.1.0+3f2a9c1",
	"StartScene": "8a61c0d2e4f31b77",
	"Paks": [ { "Path": "Data/Engine.pak", "XXH64": "…" }, { "Path": "Data/Game.pak", "XXH64": "…" } ],
	"Window": { "Title": "Tetris", "Width": 720, "Height": 900, "VSync": true, "Fullscreen": false },
	"Simulation": { "FixedHz": 60, "MaxStepsPerFrame": 5, "Seed": 1, "MaxEntities": 65536 },
	"Testing": false
}
```
`Name` determines the exported game's user-data folder (`<UserData>/Tetris/` for logs, crashes and `user://`), never `ENGINE_PRODUCT_NAME`. In a testing export (`"Testing": true`), the project's `Testing` settings (suites, replays, timeouts) ride in the Game.pak TOC like the other project settings.
- **Pak layout** (little-endian): 64-byte header `{char Magic[8] = "ENGPAK01"; uint32 Version; uint32 EntryCount; uint64 TocOffset; uint64 TocSize; uint64 TocXXH64; uint8 Reserved[24]}`, 16-byte-aligned entry data (each a cooked artifact with the §6.8 header), then a canonical JSON TOC (easy to inspect) `{"Entries": [{"Handle", "Type", "Path", "Offset", "Size", "XXH64"}]}` sorted by handle and loaded into a binary-searched table. Project settings and the asset registry ride in the TOC.
- **Integrity:** the TOC hash is verified at mount; every entry's XXH64 is verified on first load in all configurations, so script bytecode is always verified before `luau_load`. A failed check is an `InitFailed` (mount) or asset diagnostic (entry) with a clear message.
- **Compression:** none in v1 (no approved compressor). **Determinism:** same inputs → byte-identical paks (tested).

### 14.2 Export pipeline (`EditorCore/Export/Exporter`, `project.export`, `Editor --export`)
1. **Validate**: every build scene loads strictly, every script compiles and type-checks, no asset diagnostic has severity Error, every referenced handle resolves. A testing export also requires every `Testing.Suites` entry to be valid (`TEST_SUITE_INVALID`) and every replay to compile. Validation works under `--renderer none`, using cooked environment bakes (§7.4).
2. **Cook** every included asset (§7.6) incrementally from `Library/Cache` and the engine cooked cache, in parallel on jobs.
3. **Write** `Engine.pak` (with the target configuration's SPIR-V) and `Game.pak` atomically, then reopen and verify every hash.
4. **Copy** `bin/<Config>-<sys>-<arch>/Runtime/Runtime(.exe)`, renamed. A missing binary is an error naming `python Scripts/Build.py --config <Config> --project Runtime`; the editor never spawns a build. Windows also copies `Runtime/Redist/*.dll` (the app-local CRT, §2.3); macOS assembles the bundle with the SDK's loader and MoltenVK.
5. **Patch resources** (Windows): `Platform/Windows/ResourcePatcher.cpp` writes the icon group (converted from `Export.Icon` to 16–256 px ICO images) and a `VS_VERSIONINFO` (`Version`, `Company`, product and file description = project `Name`) into the copied executable with `BeginUpdateResource`/`UpdateResource`/`EndUpdateResource`. Without an icon, the engine's default icon stays. macOS writes the same data into `Info.plist` and an `.icns`; Linux has no executable resources.
6. **Write** `Game.json` (with pak hashes) and the notices file.
7. **Smoke test** (optional, always in CI): run the exported executable with `--headless --frames 300 --expect-no-errors`; exit 0 required.
8. Return a JSON report: assets, sizes, warnings, timings.

### 14.3 Runtime executable
`RuntimeApp` reads `Game.json` next to the executable (or in the bundle), sets the app name for logs, crashes and `user://` from the manifest `Name` (§4.4), mounts the paks with `RuntimeAssetManager`, creates the window from the manifest, loads the start scene through the **same `SceneSerializer::FromJson` path Play uses**, and runs a `PlaySession` until `Application.Quit()` or window close. With no Vulkan loader or device it shows a message box naming the problem and exits 3. `Escape` does nothing by default; games decide. When minimized, the loop throttles and the simulation keeps running (§4.2).

For `--feature-test` with `Testing:true`, an unspecified development user-data root defaults to `<export>/bin/TestUserData/<Name>` for logs, crashes and `user://`, including Dist. An explicit development root takes precedence. Ordinary game runs retain the platform user-data folder (ADR 0019 decision 10).

### 14.4 Dist stripping
Dist Runtime builds contain no editor or `EditorCore`, no automation server or handlers, no ImGui (Release keeps an F1 stats overlay), no asserts or Trace logs, no validation layers, no shader or asset hot reload, no slangc lookup, no Luau Analysis, no Luau compiler (bytecode only), no `AssetPipeline`, and the WindowedApp subsystem; logs go to the user data folder. The **FeatureTest harness stays in Dist**: `FeatureTestRunner` with the suite runner, the cooked-replay player and the coverage counters. It is inert unless the manifest has `"Testing": true`, because testing the actual shipping binary matters more than a few kilobytes. In a testing run, behaviour that differs from the editor is defined, not accidental: hot reload does not exist (EditorOnly members are excluded from that mode's gates, §15.6), `Debug.Break` is a counted no-op, `Application.Quit` ends the current suite with a recorded result instead of exiting, `Application.IsEditor()` returns false, and replay expectations run from precompiled bytecode, because Dist has no compiler. The script watchdog stays on (Dist minimum 5 s outside test runs, §11.1). The Dist Runtime links with a map file (`/MAP` on MSVC, `-Wl,-Map` elsewhere); a test fails if it lists any `AutomationServer`, `Luau::Frontend`, `ImGui` or `AssetPipeline` symbol.

---

## 15. Testing strategy

### 15.1 Tiers
| Tier | Content | Runner | Gate |
|---|---|---|---|
| T0 Static | format, include rules, banned APIs, clang-tidy naming, header self-containment, generated-file staleness, JSON validity, vendor define consistency, `spirv-val`, `Shaders.LayoutsMatchReflection`, `SharedStructsMatchReflection` | `Lint.py`, `CheckBuildConfig.py`, `Tests --test-suite=Static` | every commit |
| T1 Unit | per-module doctest; no window, GPU or audio device (processes the tests start are headless or pass `--audio-device none`; only miniaudio's Null backend runs as a device, in the device tests of §10.4) | `Tests` | every commit |
| T2 GPU | device/swapchain state machines, every pipeline creates, readback, CPU-reference GPU tests, golden images, leak counters, zero validation messages | `Tests --test-suite=GPU,Golden` | every commit (this machine's GPU) |
| T3 FeatureTest | every component, field, enum value, setting, format, function, callback (§15.5) in three modes: editor headless lockstep, exported Release and exported Dist (testing exports); gates evaluated per mode (§15.6) | `Editor --run-tests`, `FeatureTest(.exe) --feature-test` | every commit |
| T3d Determinism | one 600-tick physics-and-script replay plus every FeatureTest replay: final state hashes identical across Editor Debug, Editor Release, exported Release and exported Dist, and equal to the committed `FinalStateHash` | `Test.py --suite determinism` | every commit (editor Debug vs Release from M14, all four runs from M15) |
| T4 Automation | Python `unittest` against a headless editor: every method, undo/redo, batch rollback, `$ref`, dry runs, security, watchdog, crash recovery, attach/lock/disconnect, export + run exported, MCP bridge conformance via the SDK client | `Test.py --suite automation` | every commit |
| T5 Games | Breakout/Tetris/RollingBall suites + every `*.replay` with verify + canonical-format check + provenance audit against the committed `Provenance.json` and transcript (§13.12) | `Test.py --suite games` | from the respective milestone |
| T6 Hardening | MSVC ASan build (`--sanitize=address` option), long fuzz runs, 10k-frame soak per demo (memory flat), Release perf capture | `CI.py --stages hardening` | milestone gates M15+ (local) |

### 15.2 Unit tests per module (representative, not exhaustive)
- **Core:** `ENGINE_TRY`/context chaining; UUID hex round trip, prefix matching, seeded generator; XXH64 test vectors; `HandlePool` generation reuse; VFS escape, reserved-name and case rejection; `OverlayMount` never writes through; atomic writes under injected failure; `JobSystem(0)` ordering; `FixedStepScheduler` tables; `ScriptedClock` cycling; DetMath accuracy (≤ 1 ULP of correctly rounded references) and committed output hash; JsonReader error paths with pointers; canonical float formatting; `BinaryReader` bounds; deterministic mutation fuzzing (seeded byte flips, truncations) of BinaryReader, JsonReader, pak and cooked-header readers with a never-crash, always-`Result` oracle.
- **Platform:** input latching tables (taps between steps, multiple steps per frame, zero steps per frame); action/axis resolution incl. up-positive stick Y and `Invert`; null-platform window event path; crash child (`Tests --crash-child` writes a report and exits 4); `Editor.lock` exclusivity (a second lock attempt fails; the lock is released when the child is killed).
- **Reflection/Scene:** registry-parametrized component and struct tests (§5.4) incl. `Map`/`Variant` and non-finite rejection; hierarchy ops and cycle rejection; canonical order independence from insertion history; TransformSystem vs reference math; byte-identical re-save; every migration; `UnsupportedVersion` for a newer file; one fixture per structural defect (duplicate IDs, dangling and cyclic parents, child before parent, inconsistent prefab links, duplicate unique components) in strict and repair mode; prefab derived IDs, overrides surviving a prefab edit, user children preserved, external references intact; deferred destroy; serializer-copy state hash equality; interpolation snapshot rules (§5.2: script-moved, kinematic, frame-phase-written, teleported, created).
- **Asset:** Khronos glTF sample fixtures (textured, normal-mapped, alpha modes, embedded/data-URI, external `.bin`/`.png` closure); URI rejection (`..`, absolute, `http:`); `TEXCOORD_1` and `COLOR_0` diagnostics; dependency metas move and trash with their owner; a standalone texture referenced by a glTF is not duplicated; required-extension rejection; stable sub-asset handles after reimport; double-import hash equality; cache corruption recovery; registry diagnostics; pak round trip, tamper detection and determinism; `SoundSynth` bit-exactness; hot-reload races (no echo reimport after an editor write, stale job completion dropped, `SceneChangedOnDisk` raised and never auto-applied, reload deferred during lockstep).
- **Renderer (CPU):** device scoring (incl. storage-format support and the Bloom fallback); projection math (perspective and orthographic reverse-Z mappings, linear-depth and view-ray reconstruction for both, clip-space +Y up without a Y flip); cascade splits, frustum-corner fit for both projections, texel snapping; GTAO radius for both projections; atlas allocation; light culling; transparent sort ties; tonemapper CPU references; blue-noise hash.
- **Physics, Audio:** §9.7 and §10.4. **Scripting:** one test per registered function, method, property, operator and constructor (enforced by comparing the test registry with `ScriptApiRegistry`); every callback; sandbox escape suite (`getfenv`/`setfenv`, `setmetatable(_G)`, writing library tables or another module's globals, `string.dump`, `debug` access, `require` outside `Assets/`); load-time VM (multi-file require graph, shared budget, engine API at module top rejected, classification of Behaviour/Module/TestSuite); yield rule (§11.3); watchdog (including during GC-heavy loops and nested callbacks inheriting the deadline); memory soft limit, second breach and hard limit; error-message format and locations (callbacks and resumed threads); hot reload (behaviour state, module patch plus dependents, chain rollback); `math.random` and DetMath-rebound `math` determinism; test runner (suite collection, sequential cases, per-case timeout, isolation, quit handling, filter, result schema); type-checker fixtures (§11.9). **Play-session fuzz:** 10,000 seeded random calls through the bindings and automation (NaN, ±Inf, 0, negative, huge, wrong types, destroyed entities) never crash, assert or reach Jolt; every rejection is a located script or protocol error.
- **EditorCore:** command property tests (§12.3); composite atomicity; dirty tracking; validator codes (incl. selective fixes); exporter validation; automation framing, auth, schema validation, `dryRun` (overlay, `supportsDryRun`, no provenance), `ifRevision`, `$ref`, bounded outputs, case-insensitive enums; an in-process round trip for every method.

**Test infrastructure** (`Tests/Source/Support`): custom doctest main (`DOCTEST_CONFIG_IMPLEMENT`) that creates the `ProcessContext` (headless platform) and parses `--require-gpu`, `--death-test=`, `--windowed-child=`, `--update-golden`; the recording assert handler and death-test helper (§4.5); the `ExpectLog` listener (§4.4); `TempDirectory` RAII; seeded `UUIDGenerator`/`Random`; `HeadlessGpuFixture` (skips with a reason when no Vulkan 1.3 device exists unless `--require-gpu`, which `CI.py` and `PreCommit.py` pass unless `--gpu-optional`; fails a test on any validation or NVRHI error or warning and on any object still alive; `Test::ProbeGpuForProcess` for tests whose code under test creates its own device); per-test timeout watchdog; glm helpers `ApproxEqual(a, b, epsilon)` plus `doctest::StringMaker` specializations for vec/quat/mat.

### 15.3 GPU correctness tests (device-independent oracles)
Shadow fallback coverage also creates a device with `GraphicsSpecification::DisableDepthClamp`: the reported capability and enabled feature are both false, and real caster/receiver tests exercise the extended-near path under both API caps. The next default device restores hardware support (ADR 0017).

Clear and exact readback; compute arithmetic; matrix convention; winding/culling; octahedral pack/unpack vs CPU; **white furnace**: BRDF energy ≈ 1 with multi-scatter compensation, prefiltered constant environment = 1 ± 0.5% in every mip, SH irradiance constant; DFG LUT vs CPU reference; GPU prefilter vs CPU reference at 16²; equirect-to-cube seam continuity; tonemapper curves vs CPU at 1,024 points; shadow stability under a 0.37-texel camera move; picking returns the expected UUID at known pixels; ImGui texture protocol; pipeline count; zero leaks at device destruction; host-image-copy and staging uploads read back identically (§8.1); the swapchain state machine (windowed child process) recovers from out-of-date and minimize; fault injection (§8.14) exits 4 with a crash report and, in the editor, an autosave (until M10, the fatal-error hook that will write it, §8.14 item 8); each test also runs with `--vulkan-api=1.3`. The GPU stage enables `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` together with standard validation, and any validation or synchronization-validation message, error or warning, fails the test: in process through `HeadlessGpuFixture`, which reads the device's counts after its teardown, and in the Editor and Runtime processes a test starts through `--expect-no-gpu-errors`. A meta-test provokes a real hazard and a real validation error to prove the wiring.

### 15.4 Golden images
Deterministic scenes from `Projects/FeatureTest/Assets/Scenes/Golden/*.scene`, rendered headless at 640×360 at fixed ticks with FXAA on and fixed noise: PBR roughness × metallic sphere grid under the Studio HDRI, IBL only, CSM PCSS near/far cascades, orthographic-camera shadows (`ShadowsOrtho`), spot shadows, GTAO on/off and with an orthographic camera (`GtaoOrtho`), each tonemapper, bloom, alpha mask/blend, debug draw, selection outline, SDF text, an imported glTF fixture, the default editor layout (`editor.screenshot`). Goldens live in `Tests/Golden/<DeviceClass>/` (device class = vendor + driver major, e.g. `nvidia-58x`). Comparison: fail if more than 0.1% of pixels differ by more than 2/255 or any pixel by more than 24/255 (thresholds tunable per test); failures write `actual`, `expected` and `diff` PNGs to `bin/TestResults/Golden/`. On a device class without goldens the suite runs in **smoke mode** (renders, zero validation errors, finite pixels, luminance statistics within loose bounds) and reports "goldens missing" as a warning, never a pass of the comparison. `Test.py --update-golden` writes candidates that must be reviewed (skill `golden-images`) before commit. No software rasterizer goldens and no cross-device comparison. The scenes are written by the committed scaffold `Projects/FeatureTest/Scaffold/Golden.jsonl` (§15.5; the fixtures it imports sit in `Scaffold/Sources` as byte-identical copies of `Tests/Data/Assets`, which a test checks) and rendered by `Test::RenderGoldenScene` from a temporary copy of the project (an `EditorAssetManager` with the engine resources, an `EnvironmentBaker` and the editor's generators, then `ViewportCapture`), which fails on any load or scan error and on any Error-level asset diagnostic instead of rendering placeholders. The M8 golden scenes and the legacy LitScene fixture turn SSAO off and disable shadow casting, so M9's passes leave their images unchanged. M9 scenes explicitly enable the passes they exercise. The editor layout golden renders the actual Editor host at its full retained UI extent, rather than the 640×360 scene-capture extent. Until scripts emit debug primitives (M13), the DebugDraw golden appends them to the extracted snapshot. The committed goldens are those of device class `nvidia-61x` (ADR 0013 decisions 14, 21 and 24).

### 15.5 The FeatureTest project (`Projects/FeatureTest`)
Each scene below is bound to one or more suites in `Testing.Suites` (§11.10). Every suite runs in all three modes unless noted.

| Scene | Exercises |
|---|---|
| `Components.scene` | one or more entities per component, **every field set to a non-default value** somewhere and **every value of every serialized enum** used somewhere (gate 1b); a suite checks the round trip and renders it |
| `Scripting.scene` | `Assets/Tests/Api/*.test.luau`: one suite per API module and userdata type (`Entity`, `Transform`, `Scene`, `Input`, `Time`, `Physics`, `Audio`, `Assets`, `Task`, `Random`, `Math`, `Quat`, `Color`, `Debug`, `Log`, `Application`, `Script`, `Field`, `Test`, and the `RigidBody`, `CharacterController`, `AudioSource`, `Camera`, `MeshRenderer` proxies), covering every function, method, property, operator, constructor and every value of small string-enum parameters, plus proxy read/write of every scriptable field of every component |
| `Callbacks.scene` | suites that assert every lifecycle callback fires in the documented order, including `OnEnable`/`OnDisable`, collision and trigger enter/exit (incl. synthesized exits on destroy and disable), `OnDestroy`, and `OnHotReload` through `Test.ReloadScript` (Editor mode only, EditorOnly members) |
| `Physics.scene` | bodies, compounds (contacts name the child collider), shared static bodies, implicit static and sensor bodies, kinematic platform, CCD ball, character controller (inner body seen by triggers and raycasts), triggers on sleeping bodies, queries and bounds, layer matrix, seam roll |
| `Audio.scene` | spatial/non-spatial sources, groups, one-shots, streamed clip, `.sfx` clips, asserted with `Test.CaptureAudio` (deterministic: lockstep pulls 800 frames per tick, no-threading resource manager, §10.1) |
| `Prefabs.scene` | instantiate from scripts and editor, overrides, nested-flattened prefab, references into instances |
| `SceneLoadA.scene` → `SceneLoadB.scene` | `Scene.Load` with parameters, VM reset |
| `Input.scene` | driven by `Assets/Tests/Replays/Input.replay`: keys, mouse, gamepad (incl. up-positive stick Y), actions, axes, phase-aware edges |
| `Timing.scene` | a suite with `Clock: [0.004, 0.016, 0.0334, 0.083, 0.5, 0.0]`, which yields 0, 1, 2 and 5 steps per frame plus clamping and dropped time. It asserts exactly-once `Pressed`/`Released` edges in both phases, `Time.GetDeltaTime()` per phase, the extraction `Alpha` (`Test.GetLastExtraction`), and interpolated positions of a script-moved, a physics-moved and a teleported entity (`Test.GetExtractedPosition`), all against analytic expectations |
| `Errors.scene` | **runtime faults only**: a runtime error, a timeout (suite override `CallbackBudgetMs: 50`) and a memory-cap breach (override `MemoryLimitMB: 16`). Asserts isolation, `Test.ExpectScriptError` and structured reports. Compile and type errors are not in the project, because they would block test runs and export (§11.9); their error kinds are covered by unit fixtures (`Tests/Data/Scripts/Errors/`) |
| `Formats.scene` | loads every asset under `Assets/Formats/`: PNG, JPEG, TGA, BMP; WAV, FLAC, MP3, Ogg Vorbis; TTF, OTF; Radiance HDR; glTF variants (`.glb`, `.gltf` with external buffers and images, embedded and data-URI); `.sfx`. The suite instantiates or plays each one, so every importer × extension is cooked and loaded in every mode (gate 9). PNG, BMP, TGA, WAV and HDR fixtures are generated deterministically by `Tests/Data/Generate/MakeFormatFixtures.py` (stdlib), JPEG by `Tests --write-format-fixtures` (stb_image_write), glTF by `MakeGltfFixtures.py`, the audio WAV and FLAC by `MakeAudioFixtures.py` (stdlib). MP3 and Ogg Vorbis are small pinned public-domain files and OTF a pinned OFL file, fetched with operator approval (§2.3; ADR 0015 decision 25) |
| `QuitApp.scene` | a suite with `ExpectQuit: 3` that calls `Application.Quit(3)` |
| `Golden/*.scene` | §15.4 |

**Settings coverage.** `Projects/FeatureTest/FeatureTest.eproj` uses non-default values wherever they are observable at run time (gravity, layers and matrix, fixed rate, seed, input actions with inversion, rendering, scripting limits, `MaxEntities`). `Tests/Data/Project/AllSettings.eproj` sets **every** `ProjectSettings` field to a non-default value. A unit test loads it, applies it to an `EngineContext` and asserts each value is observed by its consumer (gate 8).

**Determinism replay.** `Assets/Tests/Replays/Determinism.replay` (600 ticks: a 50-body pile, a rolling ball on a compound track, scripted forces, script-side DetMath trigonometry and `^` with a non-trivial exponent) carries a committed `FinalStateHash`. It runs in every mode, and the `determinism` stage compares the hashes (§15.8).

FeatureTest scenes are produced by committed automation batch scaffolds (`Projects/FeatureTest/Scaffold/*.jsonl`, run with `Editor --batch`), so they stay reproducible and exercise the automation surface; test scripts live under `Assets/Tests/`. The harness (`Engine/Testing/FeatureTestRunner`, §11.10) runs every suite and replay in `Testing`, collects results in the shared schema, evaluates the coverage gates for the current mode and writes JUnit XML plus `bin/TestResults/FeatureTest-<mode>.json`; exit 1 on any failure.

### 15.6 Coverage gates
- **Gate 1, components:** every registered component appears in FeatureTest scenes that are simulated or rendered, and every serialized field holds a non-default value somewhere.
- **Gate 1b, enum values:** every value of every serialized enum (`Tonemap` AgX/ACES/PbrNeutral/Linear, `Attenuation`, `BodyType`, `MotionQuality`, `SsaoQuality`, `ClearMode`, `ProjectionType`, `TextSpace`, `TextAlignment`, `AudioGroup`, …) appears in a simulated or rendered FeatureTest scene.
- **Gate 2, script API:** every `ScriptApiRegistry` counter is non-zero: functions, methods, properties, operators and constructors (§11.4), plus every per-value counter of string-enum parameters.
- **Gate 3, script callbacks:** every lifecycle callback was invoked at least once.
- **Gate 4, proxy fields:** every scriptable component field was read and written through a proxy at least once.
- **Gate 5, automation methods:** every registered method was called by `Tests/Automation` (the bridge-side client counts calls).
- **Gate 6, validation codes:** every `project.validate` code is triggered by a fixture in `Tests/Data/Validation/`.
- **Gate 7, descriptions:** every type, field, function, method, property and automation method has a non-empty description.
- **Gate 8, project settings:** every `ProjectSettings` field is non-default in `Tests/Data/Project/AllSettings.eproj`, and the settings test observes each one (§15.5).
- **Gate 9, formats:** every importer and every supported extension has a fixture in `Assets/Formats/` that is cooked and loaded in the run.

**Per-mode evaluation.** FeatureTest runs in three modes: editor headless lockstep (which also catches editor/runtime divergence), exported Release and exported Dist (cook and strip bugs). Each run records its counters in `FeatureTest-<mode>.json`. Gates 1–4, 1b and 9 are evaluated **per mode**: in each mode, every entry available in that mode (`RunModes::All`, plus `EditorOnly` in the editor) must be covered, and an `EditorOnly` entry counts only in the editor run. A merge step (`Test.py`) then checks that the union over the three runs covers every registered entry, so nothing is exempt in all modes. Gates 5–8 are mode-independent. Each gate has a seeded-gap fixture (`Tests/Data/CoverageGaps/`) proving that it fails and names exactly what is uncovered. Gates 1–4, 1b and 9 have one fixture per mode, for example an entry covered only by the editor run makes the Dist evaluation fail.

### 15.7 Automation and game tests
`Tests/Automation/test_*.py` (stdlib `unittest` + `Tools/Automation/engine_client.py`) start headless editors on temp projects: project lifecycle, launcher state, every method, scaffold batches, play/step/waitFor (incl. the 50 ms step budget and `stateHash`), recording rules (Edit-mode start, `restart`, capture of `Test.Inject*`, header parameters), replays, undo/redo byte-equality, dry runs (no file, watcher event, trash item or provenance entry; `Unsupported` for non-dry-runnable methods; batch rejection with `failedOp`), crash recovery, `Busy` watchdog (a script spin-loop under a raised budget), project lock (a second editor exits 3; `--read-only` opens), `editor_launch` attaching to a running editor, client disconnect (pending operations cancelled, lockstep released, play paused), `project.upgrade` provenance entries, security (bad token, HTTP probe, path escapes, oversized frame), export and run exported, and MCP conformance (the SDK's client lists tools with compact schemas, calls `scene_tree`, receives an image from `viewport_screenshot`). Game suites run each project's `Testing` suites and replays.

### 15.8 CI entry point
`python Scripts/CI.py` runs stages in order, each failing fast: `setup → generate → lint → build → bake → unit → gpu → golden → feature → automation → export → determinism → games → portability` (plus `hardening` at milestone gates). Every stage names its configuration:

| Stage | Configuration(s) | Content |
|---|---|---|
| setup, generate, lint | n/a | toolchain checks, project generation, static checks incl. `CheckBuildConfig.py` and header self-containment (`Lint.py`) |
| build | Debug, Release, Dist | all projects existing in each configuration (EditorCore, Editor, Tests have no Dist) |
| bake | Release | `Editor --headless --renderer vulkan --bake-engine-assets` fills the configuration-independent `bin/EngineCache` (incremental; a no-op when nothing changed); without a device it fails, and with `--gpu-optional` it reruns with `--renderer none` (the environments left out with a warning) and ends as a warning |
| unit | Debug, Release | `Tests` (T0 static suite + T1), incl. committed-hash physics and DetMath tests |
| gpu | Debug, Release | `Tests --test-suite=GPU --require-gpu`, validation + synchronization validation, both API caps; the fixture fails a test on any validation or NVRHI error or warning (`--gpu-optional` drops `--require-gpu` on a machine without a Vulkan device) |
| golden | Release | `Tests --test-suite=Golden --require-gpu` (CPU inputs are configuration-independent per §9.1; SPIR-V flags are identical in Debug and Release, so the commit gate compares the same goldens in Debug) |
| feature | Debug, Release | `Editor --headless --run-tests` on FeatureTest (editor mode) |
| automation | Release | Python suites and MCP conformance against the Release editor (the bridge's default); `--require-gpu` like gpu for the tests that start a rendering editor (the screenshot methods), `--gpu-optional` drops it |
| export | Release editor → Release and Dist testing exports | exports FeatureTest with `testing: true`, runs `--feature-test` in both, then merges coverage over the three modes (§15.6) |
| determinism | Debug, Release editors; exported Release and Dist | compares the final state hash of `Determinism.replay` and of every FeatureTest replay across all four runs and with the committed `FinalStateHash`; any difference fails |
| games | Release editor; Dist exports | each demo's suites and replays with `strictHash` in the Release editor and in a testing Dist export (identical hashes required), canonical re-save check, provenance audit, plain Dist export smoke test |
| portability | n/a | generation for other OSes (file lists, precompiled-header paths, xcode4 Dist LTO), clang-cl builds of Engine + Tests (Release) and of every Dist project |
| hardening | Release + ASan | ASan unit and feature suites, long fuzz, soak, perf capture |

Because simulation is deterministic across configurations (§9.1), replays and expected hashes recorded in any configuration are verified in all of them; the `determinism` stage is the mechanical proof. **Portability** (local, no remote needed): premake generation for `--os=linux gmake`, `--os=linux ninja` and `--os=macosx xcode4` must succeed with the expected file lists and project/configuration sets; the gmake and xcode4 precompiled-header paths must exist (xcode4's `GCC_PREFIX_HEADER` resolved as Xcode does), and the first-party xcode4 projects must enable LTO (`LLVM_LTO`) in Dist; then `clang-cl` builds (`toolset "clang"`) of Engine + Tests in Release and of every Dist project (asserts compiled out, so Clang reports helpers only asserts use), with warnings tolerated only in vendored code, when the VS Clang component is installed, otherwise reported as skipped (a failure with `--require-clang-cl`). Header self-containment is not a portability check: it is the `headers` step of `Lint.py` in the lint stage, because it needs the compilation database `Lint.py` generates, so it also runs on the Linux and macOS CI jobs against libstdc++ and libc++. GitHub Actions (`.github/workflows/ci.yml`: windows, ubuntu-24.04 with GCC 14 and Clang 19, macos arm64): every job runs its stages through `CI.py` (the Windows job with `--require-clang-cl`; on Linux the lint stage runs in the Clang job only, since both jobs lint the same code against the same libstdc++); it becomes a gate only once a remote exists. The hosted runners have no GPU. The Linux jobs run the gpu, golden and automation stages on Lavapipe (Mesa's software Vulkan driver, `mesa-vulkan-drivers`, which meets the Vulkan 1.3 feature set of §8.1) with the SDK's `VK_LAYER_KHRONOS_validation`, inside the unit stage's Xvfb display and with `--require-gpu`; golden images run in smoke mode there (§15.4: no software-rasterizer goldens). The Windows and macOS jobs run them with `--gpu-optional`, so every GPU test case, and every automation test that starts a rendering editor, reports why no device is usable and passes without running. Results: JUnit in `bin/TestResults/`, a summary table on stdout, exit code per §4.1.

### 15.9 Commit gate and code review
Before every commit: `python Scripts/PreCommit.py` green (generate; the full lint-stage list of static checks: `CheckBuildConfig.py` on the workspace and on every fixture workspace, `Lint.py` and its self-test, format check; Debug build; unit, gpu, golden, feature and automation suites in Debug, the automation suite against the Debug editor, and the gpu, golden and automation suites with `--require-gpu` (§15.1 gates every commit on this machine's GPU; `--gpu-optional` only on a machine without a Vulkan device)), then a **recorded review**: the `commit-review` skill (§13.10) reviews the staged diff against `Docs/ReviewChecklist.md` (correctness, error handling per §4.6, ownership, threading, determinism, tests added and meaningful, generated files refreshed, style and naming, no TODOs masking defects, docs updated). Blocking: any failing check, any unresolved checklist item, missing tests for new behaviour, a style violation. The commit message ends with a `Reviewed:` trailer summarizing the review outcome and the PreCommit result. Milestone commits additionally require the full `CI.py` green. The gate is strict for every commit except the commit of a milestone's contract task, which runs `PreCommit.py --contract` (Roadmap rule 3): outside contract mode, no `ENGINE_CONTRACT_STUB` and no test case skipped outside the child-process targets (`Test::ChildTargetSuite`) may remain (the `contract` step of `Lint.py` and the skip check of `Test.py`, `Docs/Decisions/0004-contract-stub-gate.md`).

---

## 16. Cross-platform concerns

| | Windows x64 | Linux x64 (Ubuntu 24.04+) | macOS arm64 |
|---|---|---|---|
| Status | **verified** on every change | generation-checked locally; verified only by CI on Linux | generation-checked locally; verified only by CI on macOS |
| Toolchain | VS 2026 (v145, MSVC 14.51), `vs2026` | GCC 14 / Clang 19+, `gmake`/`ninja` | Xcode 26 (minimum), `xcode4`, deployment target macOS 14.0 |
| Windowing | Win32 GLFW | **X11 GLFW only** (vendored build); Wayland sessions run through XWayland; native Wayland deferred (steps in `Vendor/GLFW/VENDOR.md`) | Cocoa GLFW; framebuffer ≠ window size (Retina) |
| Vulkan | driver `vulkan-1.dll`, loaded dynamically | `libvulkan.so.1` (package `libvulkan1`) | loader + MoltenVK (Vulkan 1.3/1.4 near-conformant, macOS 14+); KosmicKrisp used when installed (Vulkan 1.4 conformant, needs macOS 26 + Apple silicon); portability enumeration + `VK_KHR_portability_subset` |
| Audio | WASAPI → DirectSound → WinMM | PulseAudio/PipeWire → ALSA → JACK (dlopen) | CoreAudio (linked, `MA_NO_RUNTIME_LINKING`) |
| Export | app-local CRT DLLs; icon and version resources patched | RPATH `$ORIGIN` | `.app` bundle with loader + ICD, rpath `@executable_path/../Frameworks`, `Info.plist` + `.icns`, ad-hoc signed |

Common rules: precise floating point and `JPH_CROSS_PLATFORM_DETERMINISTIC` on every platform (§2.2); all binary formats little-endian (asserted); text LF; JSON UTF-8 without BOM; data paths always `/`; the VFS case policy protects case-sensitive filesystems; paths are UTF-8 internally and converted only in `Platform`; platform code lives only in `Platform/<OS>/` and `Graphics` device setup (lint-enforced), so the untested platforms concentrate risk in few small files. The renderer avoids features with weak Metal mappings (geometry shaders, wide lines, large bindless arrays). Shaders are SPIR-V 1.6 from one source on every OS. `std::stacktrace`, modules, `std::flat_map` and coroutines are not used (CodeStyle §13).

---

## 17. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Linux/macOS rot without local builds | platform code isolated; generation smoke tests and clang-cl/header checks every run; honest status labels; CI yaml ready for a remote |
| ODR/ABI mismatch with vendored libs (NDEBUG, Jolt, spdlog, miniaudio) | `Use<Lib>()` copied from VENDOR.md; `CheckBuildConfig.py`; workspace-scope `NDEBUG`; `VerifyJoltVersionID` fatal at startup |
| NVRHI or Slang drift | pinned commits/versions; NVRHI only in Graphics/Renderer/ImGui; reflection checks CPU-only; slangc version in cache key and checked |
| Swapchain sync hazards | per-image render-complete semaphores; resize/minimize state-machine tests; validation on in every GPU test |
| Golden flakiness | no temporal effects, fixed noise, per-device-class goldens, smoke mode elsewhere, device-independent oracle tests carry correctness |
| Non-determinism (threads, unordered iteration, Jolt callback order, time, build configuration) | canonical order, sorted events, seeded UUIDs/RNG, sim-time APIs, `JPH_CROSS_PLATFORM_DETERMINISTIC` + precise FP everywhere, DetMath on the simulation path, lint on unordered iteration in serialization and on CRT transcendental functions, thread-count hash tests, cross-configuration `determinism` stage |
| Variable frame rate bugs hidden by `ManualClock` | per-step interpolation snapshot of every entity, `RenderPosition` for cameras, `ScriptedClock` `Timing` suite in all three modes |
| Agent scripts hang or crash the editor | watchdog (GC-safe, nested deadline), soft memory limit with headroom, instance isolation, pause-on-error, non-finite and range rejection before Jolt, `ShapeSettings::Create` results as diagnostics, entity cap, play-session fuzz test, autosave + recovery, bridge supervision |
| Two editors writing one project | `Library/Editor.lock`, `editor_launch` attaches instead of spawning, `--read-only` second instance |
| GPU failure modes (device loss, OOM, hangs) | `VkResult`-checked swapchain, bounded timeline waits, null-checked NVRHI creation, frame-boundary catch, fault-injection tests, CPU-only autosave |
| Automation as an attack surface | loopback + token, HTTP probe rejection, path confinement, no exec methods, absent from Dist, visible kill switch |
| Luau Analysis/definition churn | pinned release, one `.cpp`, `declare extern type` syntax, doc examples type-checked |
| Asset reference breakage on reimport/move | handles in `.meta`, stable sub-asset keys, derived prefab IDs, auto-fixable registry diagnostics |
| Agent context overflow | text tree, projections, pagination, file offload, short ids, downscaled screenshots |
| Tangent quality without MikkTSpace | glTF tangents used when present; approval requested to vendor MikkTSpace; documented fallback + diagnostic |
| Scope creep | non-goals, "pays for itself" rule, milestone gates on tests, deferred list (Appendix C) |

---

## 18. Milestone plan (summary; details, acceptance tests and parallelization in `Docs/Roadmap.md`)

Every milestone ends with `python Scripts/CI.py` green on Windows (every stage that exists, in the configurations of the §15.8 matrix), a recorded review and a commit; Linux/macOS stay generation-checked.

| # | Milestone | Key deliverables |
|---|---|---|
| M0 | Bootstrap | workspace, Dependencies.lua, deterministic Jolt build (precise FP, `JPH_CROSS_PLATFORM_DETERMINISTIC`), `Shaders` utility project, project skeletons, Scripts, CheckBuildConfig, AGENTS.md, skills skeleton, CI.py, all operator approvals collected |
| M1 | Core | Base, Assert, Log, Result, UUID, Hash, Random, DetMath, Time and clocks (incl. Scripted), scheduler, Json, VFS (incl. overlay), BinaryReader, Jobs, EventLog |
| M2 | Platform and app loop | ProcessContext (two-level init), Window (incl. null), Input latching, actions, CrashHandler, Process, project lock, Application, EngineContext, EntryPoint, FrameLoop (frame-boundary catch, minimized throttle) |
| M3 | Reflection, scene, serialization | TypeRegistry (incl. `Map`, `Variant`, finite checks), all component data types, Scene/Entity, hierarchy, TransformSystem, serializer with structural pre-validation and repair, prefabs |
| M4 | EditorCore + automation core + MCP | headless editor (`--renderer none`, launcher state), commands/undo, protocol, ~40 core methods, dry runs, committed provenance + transcript, attach/disconnect rules, bridge, generated `.mcp.json` |
| M5 | Graphics foundation | device (storage-format checks, host image copy path), `VkResult` swapchain, bounded frame pacing, null-checked creation, fault injection, headless readback, shader pipeline + reflection checks, ImGui backend, golden comparator |
| M6 | Assets | registry/meta (incl. dependency metas), cooked formats, loaders, importers, built-ins, project and engine caches, race-free hot reload, pak, `asset.*`/`prefab.*` methods, `entity.bounds` |
| M7 | Walking skeleton | PlaySession + lockstep (step budget, state hash), interpolation snapshot, render extraction, minimal forward pass, Runtime exe, Exporter v0 (target-config shaders, no environments yet), export → run → exit 0 |
| M8 | Renderer I | PBR, IBL baker, skybox, tonemappers, bloom, FXAA, transparency, debug draw, text |
| M9 | Renderer II | CSM PCSS, spot atlas, GTAO (perspective and orthographic), picking, outline, overlays, debug views, stats |
| M10 | Editor UI | all panels, gizmo, content browser, launcher, autosave/recovery, `editor.*`/`viewport.*` |
| M11 | Physics | Jolt integration, shared static compounds, collider identity, sensors and synthesized exits, queries and bounds, character with inner body, invalid-input rejection, simulate mode, cross-configuration hash |
| M12 | Audio | AudioEngine (device-less engine + owned device), device-loss handling, simulation-time audio in lockstep, AudioSystem, SoundSynth, `audio.stats` |
| M13 | Scripting | VM, sandbox, load-time VM with require, full API via typed registry, run modes, callbacks, tasks and the yield rule, test runner, errors, hot reload with module patching, type checker, `script.*`, recording/replay rules |
| M14 | FeatureTest and docs | full FeatureTest (incl. Timing, Formats, settings), all coverage gates evaluated per mode, generated reference, complete skills |
| M15 | Export, hardening, **engine + editor complete** | full exporter (testing exports, cooked replays, resource patching), Dist stripping checks, three-way FeatureTest, determinism stage, CRT/redist, portability stage, soak/ASan |
| M16 | Breakout dry run (time-boxed) | agent builds `Tests/Projects/Breakout` via MCP; friction fixes |
| M17 | Tetris | built by an agent via automation; tests, replays, audit, export |
| M18 | Rolling Ball 3D | three levels via automation; replays reach every goal; audit, export |

---

## Appendix A. Decisions for CodeStyle [Architecture] topics

| Topic | Decision (section) |
|---|---|
| Error handling | `Result<T>`/`Status` (`std::expected`), asserts for bugs, fatal exit codes; surfacing per §4.6 |
| Exceptions / RTTI | enabled; first-party code never throws (lint); `try` only in allowlisted boundary files; no `dynamic_cast`/`typeid` in first-party code |
| Warnings | `/W4 /WX` (MSVC), `-Wall -Wextra -Wshadow -Werror` (GCC/Clang) for first-party; vendor via external includes, warnings off |
| Floating point | precise model everywhere (`/fp:precise`; GCC/Clang `-fno-fast-math -ffp-contract=off`, clang-cl through `/clang:`), never fast-math; `JPH_CROSS_PLATFORM_DETERMINISTIC`; DetMath instead of CRT transcendental functions on the simulation path (§2.2, §4.12, §9.1) |
| `Ref` | `std::shared_ptr`; no `WeakRef` alias; `Scope` = `std::unique_ptr` (§4.7) |
| Log/assert macros | `ENGINE_CORE_{TRACE,INFO,WARN,ERROR,CRITICAL}`, `ENGINE_{…}`, `ENGINE_{CORE_}ASSERT`, `ENGINE_{CORE_}VERIFY` (active in Dist), `ENGINE_UNREACHABLE`; loggers Engine/App/Script (§4.4–4.5) |
| Bit-flag enums | `template<> inline constexpr bool EnableFlagOperators<T> = true;` enables constrained `| & ^ ~ |= &=` and `HasFlag(value, flag)` from `Core/Base.h` |
| Threading | §4.11; memory strategy §4.7 |
| Namespaces | `Engine` everywhere (also Editor, Runtime, Tests); sub-namespaces `Utils`, `Detail`, `Automation`, `ScriptBindings`, `Lua` (checked Scripting helpers only), `Test` |
| Platform layout | `Engine/Source/Engine/Platform/{Windows,Linux,MacOS,Posix}/`, selected by premake `filter "system:*"` and guarded by `ENGINE_PLATFORM_*` |
| PCH | Engine (`EnginePCH.h`), Editor/EditorCore (`EditorPCH.h`), Tests (`TestsPCH.h`); Runtime none |
| Minimum Xcode / libc++ | Xcode 26, deployment target macOS 14.0 (unverified locally) |
| Headless/GPU tests | `HeadlessGpuFixture`, skip-with-reason unless `--require-gpu` (§15.2); goldens §15.4 |
| glm test helpers | `ApproxEqual` + `doctest::StringMaker` (§15.2) |
| FeatureTest | §15.5–15.6 |
| JSON key naming | PascalCase in authored files; camelCase automation envelopes (§6, §13.4) |
| Luau API naming | PascalCase modules, functions, properties; native `vector` and value-type fields lowercase; enums as string literals (§11.4) |
| Slang bindings | HLSL registers + Slang shifts matching NVRHI offsets, `registerSpaceIsDescriptorSet`, set 0 view / 1 material / push per draw; PascalCase resource names (§8.4) |
| Python / Lua tooling | stdlib only (except the MCP venv); Python is PEP 8 with a 120-column limit (CodeStyle §15); `Lint.py` checks syntax with `ast.parse` (also against the Python 3.10 grammar) + AST checks; premake Lua reviewed against CodeStyle §15 |
| Naming enforcement | `.clang-tidy` `readability-identifier-naming` (classes/functions/enums/constexpr `CamelCase`, private/protected members prefix `m_`, statics prefix `s_`, globals `g_`, locals/parameters `camelBack`, macros `ENGINE_` prefix via regex) run by `Lint.py` on `compile_commands.json`; falls back to the `RegexNamingChecker` in `Lint.py` (regex rules reading the same `.clang-tidy` options: `m_`/`s_`/`g_` prefixes and cases) when clang-tidy is absent or `--mode regex` is given |

## Appendix B. Judge findings and where they are resolved

| Finding | Resolution |
|---|---|
| Per-frame present semaphore hazard | per-image render-complete semaphores (§8.1) |
| `JPH_CROSS_PLATFORM_DETERMINISTIC` vs vendored Jolt | v1.0: not defined; **superseded in v1.1**: defined with precise FP for cross-configuration determinism (§2.2, §9.1; Appendix D) |
| `declare class` removal in Luau 0.741 | `declare extern type … with … end` (§11.9) |
| Version/platform claims vs vendored tree (EnTT 4.0.0, GLFW 3.5.1 X11-only, Vulkan-Headers 1.4.352, no stb_truetype/stb_vorbis/MikkTSpace) | §2.2, §7.4, §16, Appendix C |
| Luau interrupt raising during GC | `gc >= 0` early return (§11.1) |
| Unachievable CI gates (nightlies, remote CI, lavapipe on Windows) | Windows-local gates, honest status, portability stage, smoke-mode goldens (§15) |
| Exported Windows build without CRT | app-local CRT DLLs (§2.3, §14) |
| Throwing assert handler in tests | recording handler + subprocess death tests (§4.5) |
| Headless ImGui without GLFW | GLFW null platform (§4.3, §13.9) |
| `NDEBUG` / vulkan.hpp dispatcher rule | workspace-scope Dist `NDEBUG`, one dispatcher TU, init order (§2.2, §8.1) |
| Project-scoped `configurations` does not drop Dist | `removeconfigurations { "Dist" }` (§2.2) |
| Unshifted `[[vk::binding]]` vs NVRHI offsets | Slang shifts = NVRHI defaults + `registerSpaceIsDescriptorSet` (§8.4) |
| Custom Jolt job adapter | Jolt's `JobSystemThreadPool` (§4.11) |
| `os.time`/`os.clock` left, watchdog off in Dist | removed; watchdog 5 s in Dist (§11.1) |
| Inconsistent spawn semantics | immediate create + `OnCreate`, deferred `OnStart`, deferred destroy (§5.7) |
| Unsorted contacts, `GetActiveBodies` order | sorted events, canonical write-back (§9.3–9.4) |
| Random runtime UUIDs break sorted order | seeded runtime `UUIDGenerator` (§4.8) |
| Prefab update breaking references | derived instance IDs + overrides (§5.5) |
| Input edges per frame vs fixed-step reads | phase-aware latching (§4.3) |
| No array script property | `Field.Array` (§11.2) |
| Tests compiling Editor sources | `EditorCore` static lib (§12.1) |
| Late automation | automation core in M4 (§18) |
| Agents writing scripts with file tools | `script.write` + provenance audit (§13.5, §13.12) |
| FeatureTest not run on shipping Dist | three-way runs, harness kept in Dist (§14.4, §15.6) |
| Lifecycle callbacks and proxy fields not gated | gates 3 and 4 (§15.6) |
| Demo audio sourcing | `.sfx` synthesizer + built-in presets (§10.3) |
| HDRI downloads inside the editor | `FetchAssets.py` script, allowlisted, md5 (§2.3) |
| Physics/Scene/Renderer coupling | ECS-agnostic Physics/Audio, CPU-only assets, RenderSnapshot (§3) |
| IBL baked at every load | baked at import, cooked (§8.6) |
| Hazel naming not enforced | clang-tidy naming (Appendix A) |
| Vulkan 1.4 usage undefined | §8.1 policy, concrete gated host-image-copy path, dual-API tests |
| Audio device loss | notification-driven recovery/fallback (§10.1) |
| Pre-commit review undefined | §15.9 |
| Friction per collider vs Jolt per body | on `RigidBody` (§5.3) |
| Reverse-Z Y flip winding | `frontCounterClockwise = true` + test (§8.3) |
| Import allowlist requiring human input | `asset.import` reads any absolute source path read-only (§13.2) |
| Editor spawning builds during export | never; error names the build command (§14.2) |
| Native file watchers | polling only (§7.5) |
| No clean error without a Vulkan loader | exit 3 + message box (§8.1, §14.3) |

## Appendix C. Deferred decisions (with defaults in force)

| Decision | Default until decided |
|---|---|
| Vendor MikkTSpace (zlib) for glTF tangents | use file tangents; otherwise Lengyel per-vertex tangents + `ASSET_TANGENTS_APPROXIMATED` warning. The approval request is part of the M0 approvals task, so M6 never stalls on it |
| Vendor `stb_vorbis.c` for OGG | resolved: approved and vendored with M12; `AudioImporter` takes Ogg Vorbis, and Ogg Opus is refused with a conversion hint (ADR 0015 decision 2) |
| Vendor `stb_truetype.h` separately | `imstb_truetype.h` from the vendored Dear ImGui (same library) in `FontImporter` only |
| Native Wayland | X11 via XWayland |
| Point-light shadows, TAA, auto-exposure, clustered lights, instancing | not built |
| Faster registry-based Play copy | serializer copy (benchmark-gated) |
| Pak compression, reachability pruning | uncompressed, export everything under `Assets/` minus excludes |
| Second UV set and vertex colours in the mesh format | ignored with `ASSET_UNSUPPORTED_UV_SET` / `ASSET_VERTEX_COLORS_IGNORED` |
| Further Vulkan 1.4 features (push descriptors, line rasterization) | not used; need NVRHI support (§8.1) |
| Multiple open scenes in the editor | one open scene per editor (§12.1) |

## Appendix D. v1.1 principal-engineer review: findings and resolutions

| Finding (severity) | Resolution |
|---|---|
| Replays and hashes not reproducible across build configurations (critical) | `JPH_CROSS_PLATFORM_DETERMINISTIC` + precise FP for Jolt and first-party code, checked by `CheckBuildConfig.py`; DetMath on the simulation path; `determinism` CI stage; stage-by-configuration matrix (§2.2, §4.12, §9.1, §15.8) |
| Exported FeatureTest cannot pass every gate (critical) | `RunModes` on registry entries, per-mode gates plus a union check with per-mode gap fixtures; testing exports include `Assets/Tests/**` and cook replays with precompiled expectations; the replay player stays in Dist; test-mode semantics of `Debug.Break`, `Application.Quit` and `IsEditor` (§7.6, §11.4, §11.10, §14.4, §15.6) |
| Test-script model undefined (critical) | `Test.Suite`/`Test.Case`, `Testing.Suites` bound to scenes or an empty scene, sequential threaded cases with timeouts, scene restart between suites, one result schema, filter semantics (§11.10) |
| Load-time VM lacks `require` and standard libraries | pure standard libraries + load-time `require` with a shared budget, require edges in the dependency graph, Module/Behaviour/TestSuite kinds, `SCRIPT_NOT_A_BEHAVIOUR` (§11.2, §7.5) |
| Yield across the C-call boundary | explicit yield rule with `lua_isyieldable` check and documented error; task and case bodies are threads (§11.3) |
| Class pattern fails strict mode | `local T = {}` … `return Script.Define(name, T)` with `Define<T>`; `GetScript(): any` + cast idiom; nil-guarded example; fixtures (§11.2, §11.9) |
| No map or dynamic `FieldType` | `FieldType::Map` and `Variant` with runtime schema resolvers; three named users (§5.4) |
| Replay recording semantics undefined | tick-0 recording, capture of all applied input, scene handle + parameters + version in the header, autopilot suite workflow, re-recording procedure (§6.7, §13.6) |
| `entity.bounds` check script not implementable | `Entity:GetWorldBounds`, `Physics.GetBodyBounds`/`GetColliderBounds`; main-path raycast support check in the Rolling Ball suite (§9.5, §11.5, §13.11) |
| Seams between implicit static bodies | shared Static body per level root, `EnhancedInternalEdgeRemoval`, `PHYSICS_ADJACENT_STATIC_BODIES`, collider identity in contacts, Sensor–Static/Sensor–Sensor filtered (§5.3, §9.2, §9.4) |
| Interpolation incomplete | per-step snapshot of every entity, frame-phase and teleport resets, `RenderPosition`/`RenderRotation`, `Time.GetInterpolationAlpha` (§5.2, §5.7) |
| Variable-rate frames never tested end to end | `ScriptedClock` and the `Timing` suite in all three modes (§4.2, §15.5) |
| Orthographic GTAO and shadows | projection-aware reconstruction, ortho GTAO radius, generic-corner cascade fit, CPU tests, `GtaoOrtho`/`ShadowsOrtho` goldens (§8.3, §8.7, §8.8) |
| Two editors on one project; orphaned lockstep | `Library/Editor.lock`, attach-instead-of-spawn, `editor_attach`, `--read-only`, disconnect cancels and releases (§4.13, §13.2, §13.8) |
| Provenance audit not reproducible | committed `Automation/Provenance.json`, `project.upgrade`, always-on transcript, re-recording procedure (§13.4, §13.8, §13.12) |
| Vulkan failure surfaces | `VkResult`-based swapchain, frame-boundary catch, bounded waits instead of `waitEventQuery`, null-checked creation, device fault data, fault injection, minimized throttle (§4.2, §4.6, §8.1, §8.14). Correction to the finding: NVRHI already catches `vk::DeviceLostError` inside `Queue::submit` and reports it through the message callback; the engine handles that path too |
| Script or automation values crash Jolt | finite-only values, `FieldMeta` minimums, `ShapeSettings::Create` results, new physics validation codes, entity cap, play-session fuzz (§5.3, §5.4, §9.1, §15.2) |
| Duplicate IDs on load assert | structural pre-validation with located results, `UnsupportedVersion`, repair loading, fixtures (§6) |
| Hot-reload races | no-echo writes, generation-checked jobs, `SceneChangedOnDisk`, deferral in deterministic sessions, module patching with dependents (§7.5, §11.8) |
| Audio device loss cannot keep voices | device-less `ma_engine` + engine-owned `ma_device`, simulation-time audio in lockstep, Null-backend tests (§10.1, §10.4) |
| glTF external files and texture duplication | dependency closure, dependency metas, URI rejection, UV1/colour diagnostics (§6.4, §7.4, §13.2) |
| Methods/properties not registered or gated | `ScriptApiRegistry::Type(...)` with methods, properties, operators, constructors; `Bindings/Script.cpp`; shortcut rule (§11.4) |
| Feature-level coverage not enforced | gates 1b, 8, 9 and per-value enum counters (§15.6) |
| Test module cannot observe what scenes promise | `GetScriptErrors`, `ExpectScriptError`, `CaptureAudio`, `ReloadScript`, `GetStateHash`; no-threading resource manager; per-suite overrides; runtime-only `Errors.scene` (§10.1, §11.5, §15.5) |
| Startup order contradictions | process-level vs per-context initialization; windowed GPU tests in a child process (§4.1) |
| Engine.pak HDRIs need a GPU | engine cooked cache, `--bake-engine-assets`, `Unsupported` under `--renderer none` without a bake, target-config SPIR-V (§7.5, §7.6, §14.2) |
| `dryRun` writes real files | `supportsDryRun`, `OverlayMount`, no provenance for dry runs (§13.4) |
| Storage image formats unchecked | `shaderStorageImageExtendedFormats` + per-format checks, Bloom fallback, `[vk::image_format]`, synchronization validation (§8.1, §8.4, §15.3) |
| Module placement ambiguities | `EditorAssetManager` in AssetPipeline, `IScriptDiagnosticsProvider`, explicit layer-5 DAG, NVRHI-free snapshot headers (§3) |
| Consistency items | world-transform rw rules, `PrefabEntityID` key, two scene/prefab importers, `Lint.py` CheckIncludes step, missing codes, budget precedence, `.jsonl` exemption, single open scene, manifest path, push constants in set 0, `imgui.ini` under `user://`, generated blue noise and icons, `StartScene`/`BuildScenes` in the walkthrough, case-insensitive automation enums |
| Automation ergonomics | compact MCP schemas, generated `.mcp.json`, `stateHash` in results, launcher state, headless throttle, selective fixes, `scene.open` dirty handling, 50 ms step budget (§13) |
| Exit semantics and CharacterVirtual | synthesized exits at flushes; inner body + `CharacterContactListener` (§9.4, §9.6) |
| Memory cap and nested deadlines | soft limit with headroom, second breach stops the session, nested callbacks inherit the deadline (§11.1) |
| PreStep teleport lag | `TransformSystem::Update` before `PreStep` (§5.7, §9.3) |
| Exported game naming and resources | user data from manifest `Name`, resource patching, `Export.Icon/Version/Company`, macOS rpath (§4.4, §14) |
| Vulkan 1.4 used only as `apiVersion` | gated host-image-copy upload path with dual-cap tests (§8.1) |

**v1.2: accepted M0 deviations.** `Docs/Decisions/0002-m0-deviations.md` is the record of the v1.2 changes and of the reason for each: the GCC/Clang floating-point spelling (§2.2, Appendix A), `Engine/Config/entt/ext/config.h` deferred to the M1 contract task (Roadmap M1), the regex naming fallback in `Lint.py` (Appendix A), the ninja shader rule on `Engine` (§2.2, §8.12), `ast.parse` instead of `compileall` (§2.3), header self-containment in the lint stage (§15.8), the module-rule details (§3), the wider `CheckBuildConfig.py` checks (§2.2), the `commit-review` skill name (§13.10, §15.9), `Tests/Source/ThirdParty/` (§2.1) and Python at 120 columns (Appendix A, CodeStyle §15). v1.2 also records xcode4 Dist LTO (§2.2), Lint's `--mode` (§2.3), the Core spdlog rule (§3), the portability checks and CI jobs (§15.8) and the PreCommit steps (§2.3, §15.9).
| Input and audio details | up-positive stick Y + `Invert`, replay events reuse the automation event set, `PlayOneShot` defaults (§4.3, §6.7, §10.2) |
