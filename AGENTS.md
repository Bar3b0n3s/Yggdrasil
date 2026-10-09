# AGENTS.md

This is the development guide for humans and AI agents working on this repository. It is loaded into every agent's context, so it stays short and points to the authoritative documents for detail. Where it disagrees with a document under `Docs/`, the document wins; fix this file in the same change.

## What this is

A production-grade C++23 3D game engine with an editor, a standalone runtime and full AI automation. Its display name is "Yggdrasil". It targets Windows x64, Linux x64 (Ubuntu 24.04+) and macOS arm64.

- **Stack:** premake5, Vulkan 1.4 through NVRHI, GLFW, glm, EnTT, Jolt Physics, miniaudio, Luau, Dear ImGui, Slang shaders and doctest.
- **Testability:** every subsystem is testable without a window, GPU or audio device.
- **Determinism:** simulation is deterministic across frame rates, thread counts and build configurations.
- **Automation:** an agent must be able to build, test and export a game through the editor automation API alone.

## Repository map

| Path | Contents |
|---|---|
| `premake5.lua`, `Dependencies.lua` | Workspace, `ApplyFirstPartySettings()`, one `Use<Lib>()` per vendored library |
| `Engine/` | `Engine` static library (`Source/Engine/<Module>/`, include root `Engine/Source`) and the `Shaders` utility project; `Config/` (EnTT configuration: `entt/ext/config.h` routes `ENTT_ASSERT` to the engine's assert handler, `Docs/Decisions/0003-m1-contract-decisions.md` decision 9) |
| `Editor/` | `EditorCore` (UI-free static library) and `Editor` (executable); include root `Editor/Source` |
| `Runtime/` | The exported-game executable |
| `Tests/` | doctest executable (`Source/` mirrors the include roots), fixtures in `Data/` (asset fixtures in `Data/Assets/`, their generators in `Data/Generate/`), goldens, Python automation tests |
| `Resources/` | Engine resources: `Shaders/` (Slang and `Shaders.json`), environments, fonts, templates |
| `Scripts/` | Python entry points and shared helpers in `Lib/`. Toolchain pins: `Lib/Toolchain.json` (Vulkan SDK, slangc), `Lib/toolchain.py` (MSVC, GCC, Clang, Xcode, clang-format and clang-tidy versions), `Lib/premake.py` (premake release and SHA-256) |
| `Tools/` | `Automation/engine_client.py` (the standard-library Python client of the automation protocol, Architecture §13.2) and `MCP/` (the MCP bridge, §13.8: `run.py`, `engine_mcp/`, `catalog.json` regenerated with `Editor --headless --renderer none --dump-reference <dir>`, the hash-pinned `requirements.lock` and `tests/`; `Setup.py` creates its `.venv`) |
| `Projects/` | FeatureTest and the demo games (M14+) |
| `Vendor/<Lib>/` | Third-party code, each with its own `premake5.lua` and `VENDOR.md` |
| `Docs/` | Authoritative docs, `ReviewChecklist.md`, `Decisions/` (ADRs and approvals), `Reference/` (generated) |
| `.claude/skills/` | Agent skills: `build-and-test`, `commit-review` and `add-automation-method`; more land with their milestones |
| `bin/`, `bin-int/` | Build output and intermediates (gitignored) |

## Authoritative documents

Order of authority: `Docs/Architecture.md`, then `Docs/Roadmap.md`, then this file. `Docs/CodeStyle.md` governs formatting and naming; Architecture Appendix A settles every topic it marks **[Architecture]**. Read the parts that matter for your task before writing anything.

| Read | When |
|---|---|
| Roadmap "Rules for every milestone" and your milestone's section | Always, before starting a task |
| Architecture §2 (layout, premake, scripts) | Touching build files or scripts, adding files or projects |
| Architecture §3 (modules, layers, globals) | Adding an include, a module or a dependency |
| Architecture §4.4–4.7 (logging, asserts, errors, ownership) | Any C++ |
| Architecture §4.11–4.12 and §9.1 (threading, determinism, DetMath) | Code in Core, Scene, Physics, Scripting or Session |
| Architecture §5–§14 | The subsystem you are changing |
| Architecture §15 (testing, CI matrix, commit gate) and §16 (platforms) | Writing tests, CI changes, before every commit |
| `Docs/CodeStyle.md` | Before writing C++, Python, Lua, Slang or JSON |
| `Vendor/<Lib>/VENDOR.md` | Using, configuring or updating a vendored library |
| `Docs/Decisions/` | Recorded decisions and operator approvals |

## Commands

Run every command from the repository root. Windows uses `python`; Linux and macOS use `python3`. Every script has `--help` and `--json`, and exits non-zero on failure (Architecture §2.3). The `build-and-test` skill covers output paths, single test cases and failure diagnosis.

| Task | Command |
|---|---|
| Set up and check the toolchain | `python Scripts/Setup.py` (also creates `Tools/MCP/.venv` and the gitignored `.mcp.json` for the MCP bridge) |
| Generate project files (again after adding or removing files) | `python Scripts/Generate.py` (vs2026, gmake or xcode4; `--action` overrides) |
| Build | `python Scripts/Build.py --config Debug` (also `Release`, `Dist`; `--project Tests` for one project) |
| Unit tests | `python Scripts/Test.py --suite unit --config Debug --junit` (fails on a test case skipped outside the child-process targets; `--allow-skips` is contract mode) |
| GPU tests | `python Scripts/Test.py --suite gpu --config Debug --junit --require-gpu` (validation and synchronization validation on; runs under API 1.4 and again capped with `--vulkan-api=1.3`; `--vulkan-api 1.3\|1.4` runs one cap; without `--require-gpu` a test case without a device passes without running and the run ends as a warning) |
| Golden images | `python Scripts/Test.py --suite golden --config Release --junit --require-gpu` (compares with `Tests/Golden/<DeviceClass>/`; smoke mode, a warning, on a device class without goldens; `--update-golden` writes candidates there, to review in the diff before committing) |
| Automation tests | `python Scripts/Test.py --suite automation --junit` (the Python suites `Tests/Automation` and `Tools/MCP/tests` against the Release editor, in `Tools/MCP/.venv` from `Setup.py`, then the method coverage gate; `--filter <test>` runs part of it; `--require-gpu` fails the tests that start a rendering editor when there is no Vulkan device, which otherwise pass without running and end the run as a warning) |
| Format C++ | `python Scripts/Format.py` (rewrites files); `--check` only reports |
| Lint | `python Scripts/Lint.py` (`--self-test` proves every seeded fixture in `Tests/Data/Lint/` still fails; `--mode regex` forces the checkers that do not need clang-tidy and clang-query; `--allow-contract-stubs` is contract mode) |
| Check build configuration (ABI defines, Jolt instruction set, FP model) | `python Scripts/CheckBuildConfig.py` |
| Compile shaders only | `python Scripts/CompileShaders.py --config Debug` |
| Commit gate | `python Scripts/PreCommit.py` (`--contract` only for a milestone's contract commit; `--gpu-optional` on a machine without a usable Vulkan device) |
| Full CI | `python Scripts/CI.py` (`--stages build,unit` for a subset; `--contract` as for PreCommit; `--gpu-optional` lets the gpu, golden and automation stages pass without a device, as on the hosted Windows and macOS runners, and makes the bake stage bake without the built-in environments (`--renderer none`) and end as a warning) |

- **Toolchain:**
  - Windows: Visual Studio 2026 (toolset v145, MSVC 14.51).
  - Linux: GCC 14 or Clang 19+.
  - macOS: Xcode 26 or later.
  - All platforms: Python 3.10+, and the Vulkan SDK 1.4.350.0 with `VULKAN_SDK` set (it provides `slangc` and `spirv-val`). `Scripts/Lib/Toolchain.json` pins the SDK and slangc versions; the other pins are listed in the repository map above.
- **Configurations:**
  - Debug and Release both keep engine asserts on.
  - Dist is the shipping configuration: asserts off, LTO, and only Engine, Shaders, Runtime and the vendor libraries. Editor, EditorCore and Tests do not exist in Dist.
- **Output:**
  - Binaries go to `bin/<Config>-<system>-<arch>/<Project>/`, for example `bin/Debug-windows-x86_64/Tests/Tests.exe`.
  - Shaders go to `bin/<Config>-<system>-<arch>/Shaders/`.
  - JUnit XML goes to `bin/TestResults/`.

## Milestone workflow (Roadmap "Rules for every milestone")

1. **Green or not done.** A milestone is complete only when `python Scripts/CI.py` passes on Windows. Every stage that exists must pass in the configurations the §15.8 matrix names, followed by a recorded review and a commit. A hash or replay recorded in one configuration must verify in all of them.
2. **Session-sized tasks.** Each task fits in one session and has its own tests. A task never leaves the tree red.
3. **Contract task first.** The first task of a milestone freezes the public headers:
   - documented signatures;
   - stubs that return `Unsupported` and start with the marker `ENGINE_CONTRACT_STUB();` (`Core/Base.h`);
   - tests marked `doctest::skip`.

   Changing a frozen header needs the contract owner's review. The contract commit is the only commit made in contract mode (`python Scripts/PreCommit.py --contract`). Every other commit runs the strict gate, which fails on any `ENGINE_CONTRACT_STUB` and on any test case skipped outside the child-process targets, so no stub or skip survives into a milestone commit (`Docs/Decisions/0004-contract-stub-gate.md`).
4. **File ownership.** Touch only the files your task owns.
   - Shared integration files have one owner per milestone: the premake files, `Scripts/ModuleRules.json`, `BuiltinComponents.h`, `RegisterBindings.cpp`, `RegisterMethods.cpp` and `Docs/Reference/*`.
   - Route changes to those files through their owner, and list what you need in your result.
5. **Parity.** Every editor-visible capability lands together with its automation method and a Python test. Every new component, field, enum value, setting, format, script API or method also gets FeatureTest coverage, in every run mode it supports (§15.6).
6. **Approvals first.** Operator approvals are never requested in the middle of a milestone (see Approvals).

## Commit gate (Architecture §15.9)

1. `python Scripts/PreCommit.py` is green: generate, the static checks (`CheckBuildConfig.py` on the workspace and its fixtures, `Lint.py`, `Lint.py --self-test`, the format check, the fixture generators' `--check` and `FetchAssets.py defaults --check`; the same list as `CI.py`'s lint stage), Debug build, and the unit, gpu, golden, feature and automation suites in Debug (the automation suite drives the Debug editor from the MCP virtual environment that `Setup.py` creates). The gpu, golden and automation suites require a Vulkan device (`--require-gpu`); only a machine without one passes `--gpu-optional`.
   - **Strict by default.** Lint rejects `ENGINE_CONTRACT_STUB` and any `doctest::skip` outside the child-process targets (`Test::ChildTargetSuite`), and the unit suite fails on, and names, any other skipped test case.
   - **Contract mode.** Only the commit of a milestone's contract task runs `python Scripts/PreCommit.py --contract`, which allows both and says so in its summary. Every other commit is strict.
2. **Recorded review.** Run the `commit-review` skill on the staged diff against `Docs/ReviewChecklist.md`. Any of these blocks the commit:
   - a failing check;
   - an unresolved checklist item;
   - missing tests for new behaviour;
   - a style violation.
3. **Tests for every change.** New behaviour gets tests. Every bug fix gets a regression test that failed before the fix.
4. **Trailer.** The commit message ends with a `Reviewed:` trailer that summarizes the review outcome and the PreCommit result.

Milestone commits also need the full `python Scripts/CI.py` green. Push to `origin` only after the review and the gate have passed. GitHub Actions (`.github/workflows/ci.yml`) then builds and unit-tests Windows, Linux and macOS. The Linux jobs also run the gpu, golden and automation stages on Lavapipe (Mesa's software Vulkan driver) with the SDK's validation layer, and bake the built-in environments there; golden images run in smoke mode there, since software-rasterizer goldens are never committed (§15.4). The Windows and macOS runners have no Vulkan device, so their GPU test cases and the automation tests that start a rendering editor report the reason and pass without running, and their bake stage leaves the built-in environments out of the engine cache with a warning (`--gpu-optional`). A platform counts as verified only once its CI job is green.

**Remote CI is non-blocking** (`Docs/Decisions/0011-non-blocking-remote-ci.md`). A milestone is done when the local Windows gate is green (strict `PreCommit.py` and `CI.py`, including the clang-cl Release and Dist builds of the portability stage); it is then merged and pushed without waiting for GitHub Actions. Linux and macOS failures are fixed in batches every few milestones and always before the demo games (Roadmap M16). A failure that points to a design problem is raised at once.

## Code style (summary; `Docs/CodeStyle.md` is the rule)

- **Formatter:** clang-format 22.x with the repository's `.clang-format`. Code is format-clean before commit: tabs, braces on their own line, no column limit.
- **Naming:**
  - PascalCase: types, functions, namespaces, enumerators, `constexpr` constants and public fields of plain structs.
  - Prefixes: `m_` for private and protected members, `s_` for mutable statics, `g_` for globals (avoid them).
  - camelCase: locals and parameters.
  - Macros start with `ENGINE_`.
  - Files are PascalCase after their primary type; test files are named `<Unit>Tests.cpp`.
- **Namespaces:** `namespace Engine` everywhere, including Editor, Runtime and Tests. Sub-namespaces are limited to `Utils`, `Detail`, `Automation`, `ScriptBindings` and `Test`.
- **Includes:**
  - Order: `EnginePCH.h` (first line, Engine `.cpp` files only), then the file's own header, then repository headers (quoted, full path from the include root), then third-party `<...>`, then standard `<...>`.
  - Headers are self-contained, never include a PCH, and never use relative paths.
- **Declarations:** `enum class` only; `explicit` single-argument constructors; `override` without `virtual`; every member default-initialized; `[[nodiscard]]` on results, handles and factories; no C-style casts.
- **Comments:** no commented-out code and no file banners. A `// TODO:` never marks a bug, missing error handling or unfinished behaviour.
- **Forbidden:**
  - modules, `std::stacktrace`, coroutines, `std::flat_map`;
  - `std::print`, `printf`, `std::cout` (use the logging macros);
  - `rand()`, `goto`, `std::endl`, `std::bind`, naked `new`/`delete`, `using namespace std`.
- **Python:** PEP 8, type hints, `pathlib`, `subprocess` with argument lists (never `shell=True`), standard library only. Entry points are PascalCase and end with `sys.exit(main())`.
- **Lua:** tabs and double quotes; close every `filter` with `filter {}`.

## Rules that are easy to violate

**Errors (§4.5–4.6)**
- First-party code never throws; lint bans `throw`. `try`/`catch` appears only at the boundaries that §4.6 allowlists.
- Expected failures return `Result<T>` or `Status`. Both are `[[nodiscard]]`; propagate them with `ENGINE_TRY`/`ENGINE_TRY_ASSIGN` and add `WithContext`/`WithHint`. Never write `error = std::move(error).WithX(...)`: it is a self-move, which empties the error outside MSVC (CodeStyle §7, Lint `banned-self-move`).
- Programmer errors use `ENGINE_CORE_ASSERT`, which is compiled out in Dist, so it must have no side effects. Use `ENGINE_CORE_VERIFY` (also active in Dist) when a violation would corrupt data or GPU state.
- An assert is never the only guard against external input: files, scenes, scripts or automation commands. Never use `assert()`.
- JSON: parse with `json::parse(text, nullptr, false)` plus `is_discarded()`, and read through `JsonReader`. `json::at` and `get<>` are banned outside `Core/Json`.
- Use `std::filesystem` only through its `std::error_code` overloads.
- Never ignore a third-party result: `VkResult`, NVRHI null handles, file I/O, miniaudio, Jolt.

**Ownership (§4.7)**
- `Scope<T>` is the default. Use `Ref<T>` only for shared immutable data such as assets and job results.
- Raw pointers and references never own, and are never stored beyond the call, except for documented back-references. No naked `new`/`delete`.
- Entities are referenced by `UUID` and assets by `AssetHandle`. Never keep a component reference across a structural change.
- Lambdas that are stored or run later list their captures explicitly.

**Determinism (§1.3, §4.12, §9.1)**
The same inputs and seed must give the same state hash in Debug, Release and Dist. A result that depends on the configuration is a bug, never a reason for per-configuration expectations.
- No CRT transcendental functions (`std::sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `pow`) on the simulation path (Core, Scene, Physics, Scripting, Session). Use `Core/DetMath`.
- Canonical order wherever order is observable: never build output, events or serialized data by iterating an unordered container. Sort, by UUID where one exists.
- Simulation time only (`Tick * FixedDelta`), never the wall clock.
- Randomness comes only from seeded `Random` and `UUIDGenerator` instances. No `std::rand` or `std::random_device` on the simulation path.
- Precise floating point everywhere: never `/fp:fast`, `-ffast-math`, or any `-ffp-contract` other than `off`. `JPH_CROSS_PLATFORM_DETERMINISTIC` is defined for Jolt and every consumer.

**Globals and threading (§3 rule 5, §4.11)**
- No mutable globals except the process-level state that §3 lists; `ProcessContext` initializes all of it.
- The ECS, scripts, physics stepping, NVRHI and ImGui run on the main thread.
- Jobs take and return values and never touch the ECS.
- First-party code never uses `dynamic_cast` or `typeid`.

**Layers (§3)**
- A module includes only lower layers and the listed exceptions. Layer 5 is an explicit DAG in `Scripts/ModuleRules.json`. `Lint.py` enforces all of this.
- `Renderer` never includes `Scene`.
- Public headers never expose GLFW, Jolt, Luau, miniaudio, cgltf or stb types.
- Platform code lives only in `Platform/Windows/`, `Platform/Posix/` (shared by Linux and macOS; `Platform/Linux/` and `Platform/MacOS/` when a difference grows) and in the Graphics device setup. Helpers that both OS halves of a unit share go in `Platform/Private/`, named after the unit. In Tests, only `Support/PlatformProbes.cpp` has OS headers (ADR 0005 decisions 1 and 25).

**No product name in code.** "Yggdrasil" appears only in the premake workspace name (`WorkspaceName` in `premake5.lua`, which scripts read through `Scripts/Lib/paths.py`), in `ENGINE_PRODUCT_NAME` and in docs. Code, macros, files, shaders, Python and premake scripts say `Engine`/`ENGINE_`; `Lint.py` checks C++, Slang, Python and Lua.

**Logging.** Engine code uses `ENGINE_CORE_*`; Editor and Runtime use `ENGINE_*`. There is no other output channel, and nothing logs at Trace or Info every frame.

## Tests (CodeStyle §14, Architecture §15)

- **Layout and naming:**
  - Tests use doctest, in `Tests/Source/<path of the unit>Tests.cpp`, inside `namespace Engine`.
  - Each file wraps its cases in `TEST_SUITE("<Module>")`.
  - Case names have the form `TEST_CASE("<Unit>: <present-tense behaviour>")`. Roadmap acceptance names are used verbatim.
- **Deterministic:** no sleeps or wall-clock timing, fixed seeds, no network, files only in a per-test temporary directory, and no dependence on test order. The wall-clock exceptions are the windowed child "FrameLoop: a minimized window uses little CPU time per second" (ADR 0005 decision 13) and, in the Python suite, `test_busy_watchdog_reports_phase` (ADR 0008 decision 15) and `test_headless_play_without_lockstep_is_throttled` (ADR 0012 decision 3). Bounded waits are not timing (ADR 0008 decision 15): waiting for another thread's or process's state with a generous deadline that only bounds a failure (`Test::WaitUntil`, a Python poll of another process), or a client timeout whose peer never answers, as long as no passing outcome depends on how long anything takes. A spin count never bounds a wait.
- **Windowed children** (`--windowed-child`) need a display; on Linux without a desktop, run the unit suite inside Xvfb with a window manager, as the `build-and-test` skill shows.
- **No audio device.** Every Editor or Runtime process a test starts is headless or passes `--audio-device none` (`Test::WithoutAudioDevice` in C++), and in-process contexts have an `AudioEngine` only when a test asks for one, without a device; only miniaudio's Null backend runs as a device, in the device tests (`Docs/Decisions/0015-m12-decisions.md` decision 23).
- **Public API only.** Expected error logs are declared with `Test::ExpectLog`. Expected asserts are death tests (`ENGINE_DEATH_TEST`).
- **GPU tests** carry `doctest::test_suite(Test::GpuSuite)` (golden ones live in `TEST_SUITE(Test::GoldenSuite)`) and start with `Test::HeadlessGpuFixture` plus `ENGINE_REQUIRE_GPU`, or `Test::ProbeGpuForProcess()` when the code under test creates its own device (an Editor or Runtime process, a windowed child, an in-process `Application`). Without a device they pass without running after naming the reason, unless `--require-gpu` is passed. The fixture fails a test on any validation or NVRHI error or warning, its device's teardown included, and on any GPU object still alive at its end; Editor and Runtime processes fail the same way through `--expect-no-gpu-errors`. Editor and Runtime processes that tests start without a GPU pass `--renderer none`. Python automation tests that need a rendering editor (the screenshot methods) call `self.require_gpu()` from `Tests/Automation/harness.py` first, which probes the editor and, without a device, names the reason and lets the test pass without running unless `--require-gpu` is in force.
- **No permanent `doctest::skip`.** It marks a contract task's tests until their implementation lands. The only permanent skips are child-process targets, which carry `doctest::test_suite(Test::ChildTargetSuite)` in the same decorator expression (Lint `test-skip`, Test.py's skip check).
- **Fixtures** live in `Tests/Data/`, with licenses in `Tests/Data/LICENSES.md`. Generated fixtures come from committed generators (`Tests/Data/Generate/*.py`, each with `--check`, which the static checks run).

## Vendored code (`Vendor/`)

- **Upstream sources are never edited or reformatted.** Build changes go in `Vendor/<Lib>/premake5.lua`, and `VENDOR.md` records them: version, commit, license, kept and removed files, build configuration, consumer settings and verification.
- **Consumer settings** live in `Use<Lib>()` in `Dependencies.lua`, copied verbatim from `VENDOR.md`.
  - ABI-relevant defines must match between a library and every consumer: `JPH_*`, `SPDLOG_*`, `MA_NO_*`, `VULKAN_HPP_*`, `VK_USE_PLATFORM_*`, `NOMINMAX`, `NDEBUG`, and the Luau (`LUA_VECTOR_*`, `LUA_USE_LONGJMP`, `LUA_API`, `LUACODE_API`), Dear ImGui (`IMGUI_*`, `ImTextureID`, `ImDrawIdx`) and GLFW (`_GLFW_*`, `GLFW_DLL`) configuration macros. Jolt consumers also use Jolt's instruction set (`/arch:SSE4.2`, `-msse4.2 -mpopcnt`).
  - These macros are set in premake only, never with `#define`/`#undef` in source (Lint `banned-abi-macro`).
  - After any build-file change, run `python Scripts/CheckBuildConfig.py` (PreCommit runs it too).
- **`NDEBUG`** is defined at workspace scope, for Dist only, and never in a project script (vulkan.hpp ODR rule, §2.2).
- **A new library or asset needs:**
  - the official upstream source;
  - a pinned version or commit;
  - a verified checksum;
  - its license committed;
  - a `VENDOR.md` (or a `LICENSES.md` entry for an asset);
  - a `Use<Lib>()` when consumers need settings.

## Working rules for agents

- **Never read or copy code from elsewhere on the machine.** Use nothing outside this repository as reference. Official web documentation of the libraries and tools is fine.
- **No hacks:**
  - no disabled or suppressed warnings to get green;
  - no `TODO` standing in for required behaviour;
  - no weakened or deleted tests.

  First-party C++ builds at `/W4 /WX`, or `-Wall -Wextra -Wshadow -Werror` on GCC/Clang.
- **Linux and macOS code must really compile** with GCC 14, Clang 19+ and Apple Clang (Xcode 26), because CI builds and tests it. Generating project files is not enough.
- **Git:** read-only git is always fine. Commit only through the commit gate, and only when your task includes committing. Never force-push, rewrite published history or skip hooks.
- **Deviations:** departing from Architecture or Roadmap needs an ADR in `Docs/Decisions/NNNN-<title>.md`. A doc that turns out to be wrong is fixed in the same change, by its owner.

## Approvals

Operator approvals (downloads, vendoring, licenses) are collected up front (Roadmap rule 6). They are recorded in `Docs/Decisions/0001-approvals.md`, which the lead owns.

- **Downloads:** the product owner has granted standing permission to download what the engine needs from official upstream sources. Pin the version, verify a checksum and commit the license.
- **Anything else that needs the operator** goes to the lead as a short request. Examples: a non-permissive license, a new kind of dependency, a scope or architecture change. Give the item, source, license, size, checksum, the reason and the fallback. Never decide it yourself in the middle of a task.
