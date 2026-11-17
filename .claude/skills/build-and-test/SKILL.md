---
name: build-and-test
description: Use this skill to generate project files, build, run unit tests or the CI pipeline, run a single doctest case, or work out why a build, shader compile, test or build-configuration check failed. It covers MSBuild/make/xcodebuild errors, slangc/spirv-val errors, CheckBuildConfig findings, ABI define mismatches, the NDEBUG ODR trap, MSB8029 and missing files after adding sources.
---

# Build and test

This skill gives the commands, configurations, output locations and failure diagnosis for this repository. The authoritative sources are Architecture §2 (build), §15 (testing, CI matrix) and `AGENTS.md`. Run everything from the repository root. Use `python` on Windows and `python3` on Linux and macOS. Every script has `--help` and `--json`, and exits non-zero on failure.

## Pages

Two reference pages beside this file hold the detail; each is a `docs.get` topic and a file in this skill's directory:
- `skills/build-and-test/test-suites` ([test-suites.md](test-suites.md)): the automation, feature, determinism, games and export suites, the provenance audit and the hardening stage in detail, the unit acceptance tests without a GPU, and the generated reference and doc-example checks (the skills' size rule among them).
- `skills/build-and-test/vendor-cache` ([vendor-cache.md](vendor-cache.md)): the shared vendor cache that MSBuild builds link the vendored libraries from: its output, keys, rejected entries, opting out, CheckBuildConfig rule 9 and the end-to-end check after a change to it.

## Toolchain

Windows uses x64 host tools (the workspace's `PreferredToolArchitecture=x64`). A link that reports LNK1102 or mentions `HostX86` ran x86 tools: MSBuild falls back to them, even when x64 is requested, when an agent's environment omits both Windows architecture variables, so check the effective host property and restore `PROCESSOR_ARCHITECTURE` in that process environment (ADR 0019 decision 9).

| | Windows | Linux | macOS |
|---|---|---|---|
| Compiler | VS 2026, toolset v145, MSVC 14.51 | GCC 14 (`CC=gcc-14 CXX=g++-14`) or Clang 19+ | Xcode 26+ (Apple Clang), arm64 |
| Generator | `vs2026` (`Yggdrasil.slnx`) | `gmake` (or `ninja`) | `xcode4` |
| Packages | Vulkan SDK 1.4.350.0 | `xorg-dev` (GLFW X11), Vulkan SDK 1.4.350.0; for the unit suite without a desktop also `xvfb openbox x11-utils` | Vulkan SDK 1.4.350.0 (MoltenVK) |

- `VULKAN_SDK` must point at SDK 1.4.350.0, which provides `slangc` and `spirv-val` for the `Shaders` project.
- `Scripts/Lib/Toolchain.json` pins the SDK version and slangc 2026.8. Any other slangc version makes `CompileShaders.py` exit 3.
- premake 5.0.0 lives in `Vendor/premake/bin/`. `Setup.py` downloads it, SHA-256 verified, when it is missing.
- **The unit suite needs a display.** Windowed child processes (`--windowed-child`, the windowed Editor test, the minimized-window test) open native windows, which Windows and macOS desktops provide. Linux needs an X display with an EWMH window manager, since GLFW reports a window minimized only once the window manager iconifies it: without a desktop session, run the suite inside `xvfb-run -a --server-args='-screen 0 1920x1080x24'` with `openbox --sm-disable &` started first and waited for (until `xprop -root _NET_SUPPORTING_WM_CHECK` succeeds), as `ci.yml` does (ADR 0005 decision 22). Without a display the windowed tests fail; without a window manager the minimize tests fail.

## Commands

```
python Scripts/Setup.py  # toolchain checks, premake download, Tools/MCP/.venv and .mcp.json
python Scripts/Generate.py  # vs2026 | gmake | xcode4 (+ compile-commands); --action to override
python Scripts/Build.py --config Debug  # Release, Dist; --project Tests: one project; --jobs 12 (at most half the CPUs): a capped build; Runtime builds write its THIRD_PARTY_NOTICES.txt, Dist archives its symbols in bin/Symbols; MSBuild links the vendored libraries from the shared vendor cache (--no-vendor-cache compiles them here; the vendor-cache page)
python Scripts/Locks.py  # who holds the machine-wide gate, gpu and build locks (pid, worktree, command); changes nothing
python Scripts/Test.py --suite unit --config Debug --junit # Release too; Dist has no Tests project; --allow-skips: contract mode; Debug runs a tenth of each fuzz sequence (--fuzz-scale 0.1), Release every count
python Scripts/Test.py --suite gpu --config Debug --junit --require-gpu  # both API caps; --vulkan-api 1.3|1.4 for one
python Scripts/Test.py --suite golden --config Release --junit --require-gpu  # --update-golden writes candidates
python Scripts/Test.py --suite automation --junit  # Python suites against the Release editor, then method coverage; --require-gpu as for gpu
python Scripts/CompileShaders.py --config Debug  # --program P, --force, --verbose
python Scripts/Test.py --suite feature --config Debug --require-gpu  # FeatureTest through the editor, all coverage gates (--junit: bin/TestResults/Feature-Debug.xml)
python Scripts/Test.py --suite export --config Release,Dist --junit --require-gpu  # testing exports of FeatureTest, --feature-test, per-mode gates, merge (run the feature suite in Release first)
python Scripts/Test.py --suite determinism --config Debug,Release,Dist --junit --require-gpu  # FeatureTest replays: the Debug and Release editors (checkpoints compared, the input check) plus the export suite's Release and Dist reports, hashes compared
python Scripts/Test.py --suite games --config Debug,Release,Dist --junit --require-gpu  # every staged demo game: provenance audit, Debug and Release editor halves (replays, the input check), the Dist half (testing export + plain smoke test)
python Scripts/AuditProvenance.py --project Tests/Projects/Breakout  # §13.12 audit of the staged files (--files disk: the working tree; --demos: every game project; --self-test: Tests/Data/Provenance, a static check)
python Scripts/CheckRuntimeMap.py --config Dist  # the Dist Runtime's map: no automation, Analysis, ImGui, AssetPipeline or Luau compiler symbol; --self-test: Tests/Data/MapFiles
python Scripts/Export.py --project P --config Release --out D  # one-shot export through Editor --export (--testing, --smoke-test)
python Scripts/BuildNative.py --project G --kind module --config Debug --out D  # a game's Native/ module (Windows; --kind runtime: an export's Runtime); D/NativeBuild.json has the diagnostics
python Scripts/Hardening.py --steps soak --soak-only "FeatureTest:Assets/Scenes/Input.scene"  # one hardening step; --soak-only, --fuzz-iterations, --fuzz-processes are development aids (warnings); --soak-frames 40000 soaks longer
python Scripts/Perf.py --config Release  # warning-only perf capture into bin/TestResults/Perf.json
python Scripts/Scaffold.py --check  # Projects/FeatureTest regenerates byte for byte from its scaffolds
python Scripts/GenerateDocs.py --config Debug  # rebuild Editor first; the whole Docs/Reference, Engine.d.luau and catalog.json; --check fails on stale, missing or extra files
python Scripts/CheckBuildConfig.py  # ABI defines, JPH_CROSS_PLATFORM_DETERMINISTIC, Jolt ISA, FP model, Dist solution, the vendor-cache entries a build would link (--no-vendor-cache leaves them out)
python Scripts/Format.py --check  # without --check it rewrites files, never a Native/ source (native.write's)
python Scripts/Lint.py  # --self-test: every seeded Tests/Data/Lint fixture fails as expected; --mode clang|regex; --allow-contract-stubs: contract mode
python Scripts/PreCommit.py  # a change straight to main too broad to scope (ADR 0051; otherwise its targeted checks): generate, static checks, Debug build, unit + gpu + golden + feature + automation + games (Debug editor); --gpu-optional without a Vulkan device
python Scripts/CI.py  # all stages; --stages build,unit for a subset; --gpu-optional as for PreCommit; --milestone (every stage plus hardening, clang-cl required, no other selection option) or --require-clang-cl (below); holds the gate, gpu and build locks
```

**CI stages** run in order and fail fast: `setup → generate → lint → build → bake → unit → gpu → golden → feature → automation → export → determinism → games → portability`, plus `hardening` (perf, soak, ASan, debug heap, fuzz) with `--milestone` (a plain run lists it as not run), which only a phase's last lander adds, on the phase's merged tree, when a milestone of the phase touches core systems, file readers (fuzzed formats) or the renderer; every other milestone end runs `CI.py --require-clang-cl` (ADR 0028 decision 12; the setup gate's rule is decision 16, and M27 decides in its plan). Checkpoint commits on a milestone branch run only the tests and suites their change touches, never `CI.py` or `PreCommit.py` (decision 11). The configurations follow the §15.8 matrix:
- lint: `CheckBuildConfig.py` on the workspace and on each fixture workspace under `Tests/Data/BuildConfig/` (each must fail with its own defect), `Lint.py` (its `contract` step included), `Lint.py --self-test`, `CheckRuntimeMap.py --self-test`, `AuditProvenance.py --self-test` (each case of `Tests/Data/Provenance/` yields exactly its `Expected.json`), `Format.py --check`, each fixture generator under `Tests/Data/Generate/` with `--check`, `GenerateDetMathReference.py --check` (its self-test, then `DetMathReferenceData.h` against a fresh generation), `FetchAssets.py defaults --check` (offline: the committed HDRIs and font match their pins) and `FetchAssets.py fixture --check` (offline: the pinned OTF, MP3 and Ogg test fixtures match their pins and the `Tests/Data/LICENSES.md` table). `PreCommit.py` runs exactly this list too (`Scripts/Lib/scripts.py`);
- build: Debug, Release and Dist, plus, after Debug, `"Shaders: slang-only change is not skipped by the up-to-date check"` (touching only a `.slang` file re-runs the shader rule; the next build skips it), and after Dist `"Dist: map file contains no automation, Analysis, ImGui or AssetPipeline symbols"` (`CheckRuntimeMap.py --config Dist`, which also requires the FeatureTest harness's symbols);
- bake: Release, `Editor --headless --renderer vulkan --bake-engine-assets` fills the configuration-independent `bin/EngineCache` (the engine cooked cache, ADR 0010 decision 13; incremental), the built-in environments with the GPU environment baker (ADR 0013 decision 9). It exits 1 when an entry fails; entries this build has no importer or generator for are warnings. Without a usable Vulkan device the stage fails, unless `--gpu-optional` is given: it then reruns with `--renderer none`, leaves the environments out with a warning and ends as a warning;
- unit: Debug and Release, each failing on a test case skipped outside the child-process targets;
- gpu: Debug and Release, each under API 1.4 and capped with `--vulkan-api=1.3`, with validation and synchronization validation and `--require-gpu` (`--gpu-optional` drops it on a machine without a device);
- golden: Release, compared with `Tests/Golden/<DeviceClass>/`; smoke mode (a warning) on a device class without goldens, a failure instead when a class of the same vendor has them (the driver changed; golden-images skill);
- feature: Debug and Release, `Editor --headless --project Projects/FeatureTest --run-tests` with `--require-gpu` (an `.hdr` or `.exr` import needs the GPU baker); see "The feature and determinism suites" in `skills/build-and-test/test-suites`;
- automation: Release, with `--require-gpu` like gpu (its screenshot tests start rendering editors); see "The automation suite" in `skills/build-and-test/test-suites`;
- export: Release and Dist testing exports of FeatureTest, with `--require-gpu`; see "The export and hardening stages" in `skills/build-and-test/test-suites`;
- determinism: the Debug and Release editors (no GPU needed) and the export stage's current Release and Dist reports, with `--require-gpu`; see "The feature and determinism suites" in `skills/build-and-test/test-suites`;
- games: Debug, Release and Dist in one `Test.py --suite games` run with `--require-gpu`; see "The games suite and the provenance audit" in `skills/build-and-test/test-suites`;
- portability: generates Linux gmake/ninja and macOS xcode4 projects (checked against the vs2026 reference; the xcode4 and gmake precompiled-header paths must resolve, the xcode4 Dist projects must enable LTO, and the Runtime's link settings must hold in every target: `Scripts/Lib/portability.py`), then builds Tests with clang-cl in Release and every Dist project when the VS Clang component is installed (`--no-clang-cl` leaves it out, `--require-clang-cl` makes a missing component a failure; `--milestone` requires it). Only warnings located under `Vendor/` are tolerated in those builds;
- hardening (only at each phase's last lander, `CI.py --milestone`; none in the World Tree program, M28 to M35, by the product owner's decision, ADR 0042, and once, at its last batch gate, in its phase 6, M36 to M40, ADR 0052): `Hardening.py` with every step, one CI step each (see "The export and hardening stages" in `skills/build-and-test/test-suites`).

**GitHub Actions** (`.github/workflows/ci.yml`) runs only when started by hand (`workflow_dispatch`): GitHub is an off-site backup, nothing reads or waits for its results, and Windows is the only verified platform (ADR 0028 decision 10). It runs `CI.py --gpu-optional` without the feature, export, games and hardening stages (a real GPU, the milestone gate): one Windows run with `--require-clang-cl`, one per step on Linux (GCC 14, Clang 19; Xvfb with openbox, Lavapipe) and macOS.

## Machine-wide locks

Every checkout (the main one and every worktree) shares three operating-system locks in the git common directory (`D:\Engine\.git\machine-locks\`; Architecture §2.3, `Scripts/Lib/locks.py`, ADR 0028 decision 16):
- **build**: one uncapped build (both slots) or two capped ones. `Build.py --jobs N` with N at most half the logical CPUs is capped (MSBuild `-m:2 -p:CL_MPCount=N/2`, so at most N `cl.exe` for the whole build, the vendor cache's compiles of missing entries included); more is uncapped; without `--jobs`, uncapped on a free lock and capped at half the CPUs beside one capped build. A waiting uncapped build holds off new capped ones.
- **gpu**: one at a time: `Test.py` with a gpu, golden, feature, automation, export, determinism or games suite, `Scaffold.py`, `Perf.py`, `Hardening.py`'s GPU steps, `Export.py` when it renders or smoke-tests, and the MCP bridge from a rendering editor's launch until it exits.
- **gate**: `CI.py` holds it for its whole run, then takes gpu and build. Meanwhile every new build, `Test.py` run (unit too), GPU run, export and bridge editor launch waits: read and edit during a gate, but do not build or test.

- First come, first served: a run that has to wait queues (a ticket in `machine-locks/queue/`), and each lock goes to the earliest live waiter (for build, the earliest that fit the free slots; nothing overtakes a waiting uncapped build). A run that waits for a lock keeps its place for the whole wait, so restarting it sends it to the back. The gate still goes first: while a `CI.py` holds or queues for the gate, new builds, GPU runs and tests wait, and the `CI.py` and its children pass the runs queued before it.
- A waiter prints `Waiting for the <lock> lock (<n> s of at most <bound> s): the <lock> lock is held by pid <pid> in <worktree> since <time>: <command>`, followed by `, and <n> earlier requests go first (...)` while others queued before it, at the start, when the holders or the queue ahead change and every five minutes. `--lock-timeout S` bounds it (default 3 h, then exit code 5); `--no-wait` refuses at once, also when only queued runs are ahead (exit code 3, a `[FAILED] <lock> lock` step). The bridge waits at most 25 min, keeping its place in the GPU lock's queue, its progress notifications naming the holder, then fails `editor_launch` with `LockUnavailable`. `python Scripts/Locks.py` lists the holders and each queue (pid, worktree, command, how long each has waited).
- The operating system releases a lock, and a waiter's place, when its process ends, however it ends: never clear one by hand; stop an unwanted holder or waiter by its pid. A process whose `Scripts/Lib/locks.py` predates the queue does not queue: it can take a free lock ahead of the queued runs, but never blocks or breaks the queue. A long-running process (an MCP bridge) keeps the `locks.py` it started with until it restarts, even in an up-to-date checkout.
- Children run under their parent's locks (`ENGINE_HELD_LOCKS`: `CI.py`'s and `Hardening.py`'s steps, the automation suite's bridge tests), and a run that holds a lock never waits for a gate that began after it. An MCP client that curates the bridge's environment (the MCP SDK's stdio client, as `Tools/MCP/tests/test_conformance.py` uses it) must pass `ENGINE_HELD_LOCKS` and `ENGINE_LOCK_DIRECTORY` on, or the bridge waits for the hold it runs under.
- A Tests or Editor binary started by hand takes no lock: run GPU cases through `Test.py --suite gpu --filter "<case>"` while other checkouts may use the GPU. The lock tests use their own directory (`ENGINE_LOCK_DIRECTORY`, `Tests/Automation/lock_holder.py`).

## Configurations

| | Debug | Release | Dist |
|---|---|---|---|
| Purpose | development | optimized, asserts and validation still on | shipping |
| Projects | all | all | Engine, RuntimeCore, Runtime, Shaders, vendor libraries only |
| Engine asserts | on | on | off (`VERIFY` stays) |
| Defines | `ENGINE_DEBUG` | `ENGINE_RELEASE` | `ENGINE_DIST`, `NDEBUG` (workspace scope) |

Simulation results must be identical in all three. A test that passes in Debug and fails in Release is a real bug.

## Where outputs land

`<OutputDir>` = `<Config>-<system>-<arch>`, for example `Debug-windows-x86_64`, `Release-linux-x86_64` or `Debug-macosx-AARCH64` (premake spells the macOS `ARM64` architecture `AARCH64` in paths).

| Output | Path |
|---|---|
| Executables and libraries | `bin/<OutputDir>/<Project>/`, e.g. `bin/Debug-windows-x86_64/Tests/Tests.exe`, `bin/Release-linux-x86_64/Tests/Tests` |
| Intermediates | `bin-int/<OutputDir>/<Project>/` |
| SPIR-V, reflection and depfiles | `bin/<OutputDir>/Shaders/<Program>/<Entry>[.<KEY>-<VALUE>...].spv` plus `.stamp` |
| Test results (JUnit) | `bin/TestResults/` |
| Log of a top-level Tests run | `<UserData>/<ENGINE_PRODUCT_NAME>/Logs/Tests.log`, where `<UserData>` is the run's `bin/TestResults/<report stem>/UserData` under `Test.py` (`--user-data-dir`; never emptied, since every run of that suite and configuration in the checkout shares it), and for a Tests binary started by hand without it `%LOCALAPPDATA%` (Windows), `$XDG_DATA_HOME` or `~/.local/share` (Linux), `~/Library/Application Support` (macOS). Child processes log only to the console their parent captures |
| Crash reports of a Tests run | `<UserData>/<ENGINE_PRODUCT_NAME>/Crashes/crash-<epoch>-<pid>.txt`, plus a `.dmp` minidump on Windows; `Test.py` prints each one its run's Tests process wrote as `CRASH REPORT: <path>` and names it in the step. A child writes reports only into the root its parent passed with `--user-data-dir` (a test's temporary directory) |
| Log and crash reports of the Editor | `<UserData>/<ENGINE_PRODUCT_NAME>/Logs/Editor.log` and `.../Crashes/`; `--user-data-dir=<absolute path>` replaces `<UserData>` (not in Dist) |
| Log and crash reports of the Runtime (an exported game, M7) | `<UserData>/<Game.json Name>/Logs/<Executable>.log` and `.../Crashes/`: the application name comes from the manifest, never `ENGINE_PRODUCT_NAME`; `--user-data-dir=<absolute path>` replaces `<UserData>` (not in Dist) |
| Linker map of Runtime | next to the executable (`Runtime.map`), every configuration and platform |
| Dist symbols | archived by a Dist build in `bin/Symbols/<OutputDir>/Runtime/` (PDB and map on Windows); exports never package them |
| Generated projects | next to each `premake5.lua` (`Yggdrasil.slnx` at the root); gitignored |
| Vendored libraries (MSBuild) | the shared vendor cache, `<git common directory>/vendor-cache/v1/entries/<Project>/<Config>-<key>/`; the files a build passes MSBuild to link them are in `bin-int/VendorCache/` |

## Running tests directly

The `Tests` binary is a doctest runner, so the doctest options work on it directly. Run this way it takes no machine-wide lock: run GPU and golden cases through `Test.py --filter` instead while other checkouts may use the GPU (see "Machine-wide locks").

```
bin/Debug-windows-x86_64/Tests/Tests.exe --list-test-cases
bin/Debug-windows-x86_64/Tests/Tests.exe --test-case="Smoke: test runner starts"
bin/Debug-windows-x86_64/Tests/Tests.exe -tc="Jolt:*" -s           # wildcard; -s also prints passing assertions
bin/Debug-windows-x86_64/Tests/Tests.exe --test-suite=Core          # one module (TEST_SUITE name)
bin/Debug-windows-x86_64/Tests/Tests.exe -tc="Unit: case" -sc="variation"   # one SUBCASE
bin/Debug-windows-x86_64/Tests/Tests.exe --reporters=junit --out=bin/TestResults/Local.xml
```

- **Commas separate filters**, so a case name that contains a comma needs `\,`, or a `*` wildcard in place of the comma. For example, use `-tc="Physics: state hash identical with 0*"`.
- **Engine-specific modes** (Architecture §15.2) serve tests that spawn child processes: run the parent test case, never them by hand. At most one child mode per run: `--death-test=<name>` (one death-test body), `--windowed-child=<case>` (one ChildTargets case in a windowed process), `--crash-child` (crashes once the process context is up; `--child-argument=fatal-error` takes the fatal-error path); the parent appends `--child-process` (console logging only, no crash report without `--user-data-dir`), `--user-data-dir=<absolute path>` (the child's user-data root, in the parent's temporary directory) and `--child-argument=<text>` (input for a child body).
- **GPU options:** `--require-gpu` turns GPU-suite skips into failures, `--vulkan-api=1.3|1.4` caps the API of every GPU test's device and of the processes it starts, and `--update-golden` writes golden candidates for this machine's device class.
- **`--test-timeout=<seconds>`** is the per-case limit for test cases that set none (default 120). A case that needs longer calls `Test::SetTestCaseTimeLimit(seconds)` as the first statement of its body; `--test-timeout-scale=<s>` multiplies both. Never use `doctest::timeout`: doctest checks it itself after the case, unscaled, and the unit case "TestTimeout: no test case carries a doctest::timeout decorator" fails while any case carries one. A case that runs longer than its limit is reported on stderr and the run exits with code 5.
- **`--no-skip`** also runs the `ChildTargets` suite: cases that exist only as child-process targets of other tests, which hang or end the process by design. Exclude them: `Tests.exe --no-skip --test-suite-exclude=ChildTargets`.
- **Skipped cases.** `--list-test-cases` omits skipped cases. `Tests.exe --no-skip --list-test-cases --reporters=xml --out=<file>` lists every case with its `testsuite` and `skipped` attributes; this is how `Test.py` finds skipped cases outside the `ChildTargets` suite.
- **Exit codes** (§4.1): 0 success, 1 test failure, 2 usage error, 3 init failed, 4 crash or assert (exit 4 is what the parent of a death test expects), 5 timeout.

## Reading failures

- **Build errors.** On MSVC the format is `file(line,col): error C####: ...`; on GCC and Clang it is `file:line:col: error: ...`.
  - Warnings are errors in first-party code. Fix the cause and never suppress the warning.
  - Fix the first error first; later errors are often consequences of it.
- **Shader errors** come from `CompileShaders.py`. For each failing variant it prints slangc's diagnostic, then a canonical `<file>: error: shader <Program>/<Entry> (<KEY>=<VALUE>, ...) failed to compile` line (no parenthesis without permutations).
  - When a compile fails, the previous `.spv` files and the stamp are kept; `shaders.reload` takes a fixed build into a running editor.
  - Exit 1 is a compile, validation or tool failure (a slangc or spirv-val run stopped after 300 s included). Exit 2 is a usage or configuration error (an unknown `--program`, an invalid `Shaders.json`). Exit 3 means slangc or spirv-val is missing or slangc is not the pinned version. Neither 2 nor 3 is a shader bug.
- **doctest failures** look like `file(line): ERROR: CHECK( a == b ) is NOT correct!` followed by `values: CHECK( 1 == 2 )`. Re-run the single case with `-s` to see the passing assertions around the failure.
- **Exit code 4 (crash or assert).** The process printed `Crash: <reason>; report written to <path>` on stderr (or `...; no report written` without a report directory, as in death-test children; on Windows also `incomplete report written` when the reporter could not finish, and `crash handler fault on the <crashing|reporter> thread` when the handler itself crashed). The report holds the reason, the breadcrumbs (frame phase, scene, automation method), a symbolized stack trace and the last 256 log lines; a failed assertion or another fatal error instead logs `Fatal error (<Kind>): <message>` at Critical, followed by `Crash report written to '<path>'` in a process that writes reports. Reports of a top-level run are in `<UserData>/<ENGINE_PRODUCT_NAME>/Crashes/` (see "Where outputs land"). A recovered device loss or reset hang, and an out-of-memory request after startup (failed alone with `OutOfMemory`), never exit 4 (Architecture §8.14 item 6). A report's `TestCase` breadcrumb names the test case that was running.
- **A Tests run that dies with an exit code and nothing else** (no doctest summary, no failed case, no JUnit report: `Test.py`'s `Tests exited with code <n>`). While doctest runs, the crash handler's exit watch (Windows; Architecture §4.13, §15.2) turns an end through `exit()` on any thread, or through `ExitProcess` (a driver's or a library's `exit()`, `_exit` and `quick_exit` call it), into a crash report and a stderr line `Unexpected exit (exit()) on thread <id>, ...` or `Unexpected exit (ExitProcess) with exit code <n> on thread <id>, ...; report written to <path>`; `Test.py` names that report like a crash's. Read its stack trace down to the exiting thread's start routine (the module there tells whose thread called it: a driver's, a Vulkan layer's, the engine's) and its `TestCase` breadcrumb. doctest's own fatal-condition handler takes an unhandled exception or `abort()` during a case first: it prints `CRASHED` and the summary, then calls `std::exit(1)` on the faulting thread, which the watch reports as an `exit()` of that thread, its stack running down through the exception's dispatch (`KiUserExceptionDispatcher` or `RaiseException`) to the faulting frame, with the crashed case's `TestCase` breadcrumb; an `exit()` during a case prints `CRASHED: SIGABRT` after the watch's report, when static destruction meets the timeout watchdog's running thread. Without any report, `Test.py`'s detail says so and names, from the run's log files, the last test case the process started (the log's `Test case of process <pid>: <name>` lines) and that thread's last log lines; unless the exit code is the binary's own (2 a malformed option, 3 a failed initialization, 4 a crash whose report could not be written, 5 a test case past its limit, each announced in its log), on Windows the end was then `TerminateProcess`, called inside the process or outside it, or an exception status (`0xC0000409`, a `__fastfail`) no handler saw. A log whose last Info lines after its last Warn or Error line are all there was flushed at the end, which `TerminateProcess` never does (the file sink buffers Info lines). Rerun the run's suite alone first; the evidence of R-GPUCRASH (`Docs/Backlog.md` §3) lists earlier occurrences.
- **CheckBuildConfig findings** look like `[windows] Debug: Tests includes JoltPhysics headers but lacks JPH_PROFILE_ENABLED ...`; the bracket names the target (`windows`, `windows-clang`, `linux`, `linux-clang`, `macosx`). The exit codes are 0 clean, 1 findings or a generation failure, 2 usage error, 3 premake missing or not the pinned 5.0.0, 5 timeout. `--keep` keeps the generated projects so you can inspect them.
- **Exit code 3 from any script** means a required tool or file is missing or has the wrong version (premake, clang-format, clang-tidy or clang-query in `--mode clang`, slangc, a compiler, an archiver or lld). Run `python Scripts/Setup.py`, which names what to install.
- **CI.py** prints a summary table with one row per stage and configuration. Fix the first failing stage; later stages did not run.
- **Contract stubs and skips** (Roadmap rule 3, ADR 0004). Lint `contract-stub` means an `ENGINE_CONTRACT_STUB();` stub is still in the tree: implement the function, replacing the whole stub body. Lint `test-skip` or a Test.py `UNEXPECTED SKIP: <case>` line means a test case is still marked `doctest::skip`: remove the decorator once its implementation has landed. Only child-process targets stay skipped, with `doctest::test_suite(Test::ChildTargetSuite)` in the same decorator expression. Contract mode (`Lint.py --allow-contract-stubs`, `Test.py --allow-skips`) is for a contract checkpoint's targeted checks; its wave's integration replaces every stub and skip (ADR 0028 decision 11).

- **GPU tests** (doctest suites `GPU` and `Golden`, excluded from the unit stage). Every device is created with validation and synchronization validation; the fixture fails the test on any validation or NVRHI error or warning (`Vulkan validation: ...`, `NVRHI: ...` in the log) and on GPU objects still alive at its end (`GpuResourceTracker` counts). `Vulkan loader (...)` lines describe the machine's layers and drivers (an overlay layer, a missing driver) and never fail a test. Each `Graphics device: ...` line ends with `implicit layers: ...`, the layers the loader inserted without the engine asking (the driver's own, and any overlay or capture layer another program installed, such as OBS Studio's `VK_LAYER_OBS_HOOK`), so a log or crash report names what ran in the process. "GPU test skipped: <reason>" means no usable device: no loader, no Vulkan 1.3 device (the reason lists each candidate's rejection), or a missing `VK_LAYER_KHRONOS_validation`. A golden mismatch writes `<Name>-actual.png`, `-expected.png` and `-diff.png` into `bin/TestResults/Golden/`.
- **Editor and Runtime without a GPU** need `--renderer none`; with the default Vulkan renderer they exit 3 when no device can be created.

## Pitfalls

- **New file not compiled, or a shader edit not rebuilt.** premake expands globs and `buildinputs` when it generates the projects. Re-run `Generate.py` after adding, removing or renaming source or shader files.
- **MSB8029** ("Intermediate or Output directory cannot reside under the Temporary directory"). It appears when a workspace generated with `--to=<temp dir>` is built. Outputs follow `%{wks.location}/bin`, so they land under `%TEMP%`. `CheckBuildConfig.py` only generates into temp and never builds there. For a throwaway build, generate into a directory outside `%TEMP%`. Never silence the warning.
- **ABI define mismatch.** Symptoms: heap corruption, crashes inside a vendor library, or `JPH::VerifyJoltVersionID()` failing at startup. Cause: a library and one of its consumers were compiled with different `JPH_*`, `SPDLOG_*`, `MA_NO_*`, `VULKAN_HPP_*`, `VK_USE_PLATFORM_*`, `NOMINMAX` or `NDEBUG`.
  - Add defines only through `Use<Lib>()`, copied from `VENDOR.md`.
  - Run `CheckBuildConfig.py` after any premake change.
- **NDEBUG ODR trap.** `NDEBUG` changes the layout of vulkan.hpp's dispatcher. Defining it in one project (or `#define NDEBUG` in code) produces Dist-only crashes in Vulkan calls. It is defined at workspace scope for Dist only, and nowhere else.
- **Floating-point flags.** Never add `/fp:fast`, `/Qfast_transcendentals`, `-ffast-math` or any of its components (`-ffinite-math-only`, `-fno-signed-zeros`, `-fno-trapping-math`, ...), `-ffp-contract=fast|on`, or `-ffp-model=precise`. The last one re-enables contraction and trips `-Woverriding-option` under `-Werror` on Clang. GCC and Clang use `-fno-fast-math -ffp-contract=off`; clang-cl takes the same options as `/clang:-fno-fast-math /clang:-ffp-contract=off`. This spelling is ADR 0002's deviation from Architecture §2.2. `CheckBuildConfig.py` enforces it.
- **Instruction set.** Never give a Jolt consumer other instruction-set options than JoltPhysics (`/arch:SSE4.2`, `-msse4.2 -mpopcnt`): Jolt derives `JPH_USE_AVX2` and friends from `__AVX2__`, and `VerifyJoltVersionID()` does not cover them. `CheckBuildConfig.py` compares them.
- **slangc version mismatch.** Install Vulkan SDK 1.4.350.0 and point `VULKAN_SDK` at it. Change the pin in `Toolchain.json` only as a deliberate toolchain upgrade, together with CI.
- **`python` on POSIX.** The shader build rule calls `python3` on Linux and macOS, so a machine without `python3` fails in the Shaders project.
- **Dist has no Tests, Editor or EditorCore.** Unit tests run in Debug and Release only, and those three projects cannot be built in Dist.
- **The built Runtime needs a game (M7).** `bin/<OutputDir>/Runtime/Runtime` reads `Game.json` beside it, or the one `--manifest <path>` names (not in Dist); without one it exits 3 naming `Game.json` before any log exists, and an invalid manifest or a pak whose hash differs exits 3 the same way. To run it, export a project with `project.export` (the exported folder holds the renamed Runtime, `Game.json` and `Data/`), or write a test game the way the C++ tests do (`Tests/Source/Support/TestGame.h`: `Test::WriteTestGame`) and pass `--manifest`.
- **Linux Dist with LTO.** The archiver must understand LTO objects (`gcc-ar-N`, `llvm-ar-N`) and Clang links with lld (`lld-N`); `Build.py` selects them through `Scripts/Lib/toolchain.py` or fails naming the missing package; `CC`, `CXX` and `AR` from the environment win.
- **macOS warnings** like `libtool: ... has no symbols` for libraries that hold only a PCH object are benign until real code lands. Treat any other warning as a defect.
- **A test still runs the old code after a build.** The builds recompile what is newer than its objects, so a source put back with its older modification time (`mv`, `cp -p`, `shutil.copy2` or an archive restoring a kept copy, as when proving a regression test fails without its fix) would not compile again: `Build.py` keeps the stamp of every C and C++ source and every shader rule input (`.slang`, `Shaders.json`) in `bin-int/SourceStamps.json`, as each build begins and ends, and gives such a file the current time (`Source stamps: ... given the current one so they compile again`; an input that changed while a build ran gets it as that build ends); an input it cannot read or touch is a `Source stamps: warning:` line naming it. After an MSBuild build it also relinks an executable older than a first-party library it links (a warning naming it, `staleLinks` in `--json`). Restore kept copies with a plain `cp` or `Write` anyway, and quote either line in the result if it appears.
- **Stale outputs after a toolchain change** (VS update, new SDK). Rebuild from clean: delete `bin/` and `bin-int/`, then re-run `Generate.py`. Never reuse build outputs across toolchains. The vendor cache needs nothing: its keys hold the Visual Studio, MSVC and Windows SDK versions, so the first build after the change compiles new entries.
