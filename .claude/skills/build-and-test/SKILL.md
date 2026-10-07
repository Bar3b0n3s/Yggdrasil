---
name: build-and-test
description: Use this skill to generate project files, build, run unit tests or the CI pipeline, run a single doctest case, or work out why a build, shader compile, test or build-configuration check failed. It covers MSBuild/make/xcodebuild errors, slangc/spirv-val errors, CheckBuildConfig findings, ABI define mismatches, the NDEBUG ODR trap, MSB8029 and missing files after adding sources.
---

# Build and test

This skill gives the commands, configurations, output locations and failure diagnosis for this repository. The authoritative sources are Architecture §2 (build), §15 (testing, CI matrix) and `AGENTS.md`. Run everything from the repository root. Use `python` on Windows and `python3` on Linux and macOS. Every script has `--help` and `--json`, and exits non-zero on failure.

## Toolchain

| | Windows | Linux | macOS |
|---|---|---|---|
| Compiler | VS 2026, toolset v145, MSVC 14.51 | GCC 14 (`CC=gcc-14 CXX=g++-14`) or Clang 19+ | Xcode 26+ (Apple Clang), arm64 |
| Generator | `vs2026` (`Yggdrasil.slnx`) | `gmake` (or `ninja`) | `xcode4` |
| Packages | Vulkan SDK 1.4.350.0 | `xorg-dev` (GLFW X11), Vulkan SDK 1.4.350.0; for the unit suite without a desktop also `xvfb openbox x11-utils` | Vulkan SDK 1.4.350.0 (MoltenVK) |

- `VULKAN_SDK` must point at SDK 1.4.350.0, which provides `slangc` and `spirv-val` for the `Shaders` project.
- `Scripts/Lib/Toolchain.json` pins the SDK version and slangc 2026.8. Any other slangc version makes `CompileShaders.py` exit 3.
- premake 5.0.0 lives in `Vendor/premake/bin/`. `Setup.py` downloads it, SHA-256 verified, when it is missing.
- **The unit suite needs a display.** Windowed child processes (`--windowed-child`, the windowed Editor test, the minimized-window test) open native windows. Windows and macOS desktops have one. On Linux they need an X display with an EWMH window manager, because GLFW reports a window as minimized only once the window manager iconifies it. A Linux desktop session works as it is; without one, run the suite inside Xvfb with openbox, exactly as CI does (`Docs/Decisions/0005-m2-decisions.md` decision 22):

  ```
  xvfb-run -a --server-args='-screen 0 1920x1080x24' sh -c '
    openbox --sm-disable &
    tries=0
    until xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1; do
      tries=$((tries + 1))
      if [ "$tries" -gt 300 ]; then echo "openbox did not start within 30 seconds" >&2; exit 1; fi
      sleep 0.1
    done
    exec python3 Scripts/CI.py --stages build,unit --configs Debug'
  ```

  Without a display the windowed tests fail (GLFW cannot initialize X11); without a window manager the minimize tests fail.

## Commands

```
python Scripts/Setup.py                                    # toolchain checks, premake download, Tools/MCP/.venv and .mcp.json
python Scripts/Generate.py                                 # vs2026 | gmake | xcode4 (+ compile-commands); --action to override
python Scripts/Build.py --config Debug                     # Release, Dist; --project Tests builds one project
python Scripts/Test.py --suite unit --config Debug --junit # Release too; Dist has no Tests project; --allow-skips: contract mode
python Scripts/Test.py --suite gpu --config Debug --junit --require-gpu   # both API caps; --vulkan-api 1.3|1.4 for one
python Scripts/Test.py --suite golden --config Release --junit --require-gpu   # --update-golden writes candidates
python Scripts/Test.py --suite automation --junit          # Python suites against the Release editor, then method coverage; --require-gpu as for gpu
python Scripts/CompileShaders.py --config Debug            # --program P, --force, --verbose
python Scripts/CheckBuildConfig.py                         # ABI defines, JPH_CROSS_PLATFORM_DETERMINISTIC, Jolt ISA, FP model, Dist solution
python Scripts/Format.py --check                           # without --check it rewrites files
python Scripts/Lint.py                                     # --self-test: every seeded Tests/Data/Lint fixture fails as expected; --mode clang|regex; --allow-contract-stubs: contract mode
python Scripts/PreCommit.py                                # the commit gate, strict (generate, static checks, Debug build, unit + gpu + golden + feature + automation against the Debug editor); --contract only for a milestone's contract commit; --gpu-optional without a Vulkan device
python Scripts/CI.py                                       # all stages; --stages build,unit for a subset; --contract as for PreCommit; --gpu-optional as for PreCommit
```

**CI stages** run in order and fail fast: `setup → generate → lint → build → bake → unit → gpu → golden → automation → portability`. Later milestones add feature, export, determinism and games. The configurations follow the §15.8 matrix:
- lint: `CheckBuildConfig.py` on the workspace and on each fixture workspace under `Tests/Data/BuildConfig/` (each must fail with its own defect), `Lint.py` (its `contract` step included), `Lint.py --self-test`, `Format.py --check`, each fixture generator under `Tests/Data/Generate/` with `--check` and `FetchAssets.py defaults --check` (offline: the committed HDRIs and font match their pins). `PreCommit.py` runs exactly this list too (`Scripts/Lib/scripts.py`);
- build: Debug, Release and Dist, plus, after Debug, `"Shaders: slang-only change is not skipped by the up-to-date check"` (touching only a `.slang` file re-runs the shader rule; the next build skips it);
- bake: Release, `Editor --headless --renderer none --bake-engine-assets` fills the configuration-independent `bin/EngineCache` (the engine cooked cache, ADR 0010 decision 13; incremental, and CPU-only until M8's environment bakes). It exits 1 when an entry fails; entries this build cannot bake yet (the HDRIs until M8) are warnings;
- unit: Debug and Release, each failing on a test case skipped outside the child-process targets;
- gpu: Debug and Release, each under API 1.4 and capped with `--vulkan-api=1.3`, with validation and synchronization validation and `--require-gpu` (`--gpu-optional` drops it on a machine without a device);
- golden: Release, compared with `Tests/Golden/<DeviceClass>/`; smoke mode (a warning) on a device class without goldens;
- automation: Release, with `--require-gpu` like gpu (its screenshot tests start rendering editors); see "The automation suite" below;
- portability: generates Linux gmake/ninja and macOS xcode4 projects (checked against the vs2026 reference; the xcode4 and gmake precompiled-header paths must resolve and the xcode4 Dist projects must enable LTO), then builds Tests with clang-cl in Release when the VS Clang component is installed (`--no-clang-cl` leaves it out, `--require-clang-cl` makes a missing component a failure). Only warnings located under `Vendor/` are tolerated in that build.

**GitHub Actions** (`.github/workflows/ci.yml`) runs `CI.py` everywhere, with the stages a hosted runner supports (setup, generate, lint, build, bake, unit, gpu, golden, automation, portability):
- Windows: one `CI.py` run, with `--require-clang-cl` and `--gpu-optional` (the image has no Vulkan device, so the GPU test cases and the automation tests that start a rendering editor report the reason and pass without running).
- Linux (GCC 14 and Clang 19) and macOS: one `CI.py` run per step (setup and generate, lint, Debug, Release, Dist, portability), so every configuration is reported even when another one failed (on Linux the lint step runs in the Clang 19 job only: its checks and the libstdc++ headers are the same in both jobs). Each run writes `bin/TestResults/CI-<step>.xml`. The Linux jobs install `xvfb openbox x11-utils mesa-vulkan-drivers` and run the Debug step (build, unit, gpu) and the Release step (build, bake, unit, gpu, golden, automation) inside the Xvfb display with openbox shown above, on Lavapipe (`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`) with the SDK's validation layer (`VK_LAYER_PATH=$VULKAN_SDK/share/vulkan/explicit_layer.d`) and `--require-gpu`; golden runs in smoke mode there (no software-rasterizer goldens, §15.4). macOS runs the GPU stages and the automation suite with `--gpu-optional`.
- JUnit results are uploaded as artifacts named `test-results-windows`, `test-results-linux-gcc`, `test-results-linux-clang` and `test-results-macos`.

## The automation suite

`python Scripts/Test.py --suite automation [--config Release] [--junit] [--filter <pattern>]` runs the Python suites of Architecture §15.7:
- `Tests/Automation/` (scenario tests through `harness.py`, which starts `Editor --headless --renderer none --automation --automation-test-hooks` on temporary projects with a temporary `--user-data-dir`; `test_client.py` tests `Tools/Automation/engine_client.py` against the stand-in server `fake_editor.py`) and `Tools/MCP/tests/` (the MCP bridge, through the official SDK's client).
- Both run with `Tools/MCP/.venv`'s interpreter (the bridge's tests need the MCP SDK), created by `python Scripts/Setup.py` from the hash-locked `Tools/MCP/requirements.lock`. A missing venv or editor build exits 3. Tests never skip: a skipped test fails the run.
- `ENGINE_AUTOMATION_CONFIG` (set by Test.py from `--config`) picks the editor the tests and the bridge start. One test only: `--filter test_batch_rollback_reports_failed_op` (a unittest `-k` pattern).
- **Rendering editors** (`viewport.screenshot`, `editor.screenshot`, ADR 0009 decision 33): tests that need one call `self.require_gpu()`, which runs `Editor --headless --frames 1` with the GPU test options once per run. Without a usable Vulkan device the test prints `GPU test without a device (passes without running): <test>: <reason>` and returns; Test.py counts those lines and ends the run as a warning, and with `--require-gpu` (set for the tests as `ENGINE_AUTOMATION_REQUIRE_GPU=1`) the test fails instead.
- **Method coverage** (§15.6 gate 5): every `engine_client` call is appended to `$ENGINE_AUTOMATION_COVERAGE`; afterwards `Editor --headless --renderer none --dump-reference <tmp>` lists the registered methods (`Methods.json`, without the `debug.*` hooks) and the run fails naming each one no test called (`UNCOVERED METHOD: <name>`). A `--filter` run skips this gate.
- JUnit: `bin/TestResults/Automation-<Config>.xml`, one test suite per directory plus `Automation.<Config>.coverage`.
- On Linux without a desktop, run it inside the Xvfb display with openbox (above): `test_launch_reports_editor_already_open_without_automation` starts a windowed editor.
- Diagnosis: an editor that exits before it listens is reported with its exit code and the end of its console output (`RuntimeError: Editor exited with code 3 ...`: locked or invalid project; code 2: a command-line error). `EditorCrashed` in a bridge test names the crash report under the test's user-data folder.

## Configurations

| | Debug | Release | Dist |
|---|---|---|---|
| Purpose | development | optimized, asserts and validation still on | shipping |
| Projects | all | all | Engine, Runtime, Shaders, vendor libraries only |
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
| Log of a top-level Tests run | `<UserData>/<ENGINE_PRODUCT_NAME>/Logs/Tests.log`, where `<UserData>` is `%LOCALAPPDATA%` (Windows), `$XDG_DATA_HOME` or `~/.local/share` (Linux), `~/Library/Application Support` (macOS). Child processes log only to the console their parent captures |
| Crash reports of a Tests run | `<UserData>/<ENGINE_PRODUCT_NAME>/Crashes/crash-<epoch>-<pid>.txt`, plus a `.dmp` minidump on Windows. A child writes reports only into the root its parent passed with `--user-data-dir` (a test's temporary directory) |
| Log and crash reports of the Editor and Runtime | `<UserData>/<ENGINE_PRODUCT_NAME>/Logs/<Executable>.log` and `.../Crashes/`; `--user-data-dir=<absolute path>` replaces `<UserData>` (not in Dist) |
| Linker map of Runtime | next to the executable (`Runtime.map`), every configuration and platform |
| Dist symbols | the PDB next to the executable for now (`bin/Dist-windows-x86_64/Runtime/Runtime.pdb`); archiving into `bin/Symbols/` lands with the M15 Dist stripping |
| Generated projects | next to each `premake5.lua` (`Yggdrasil.slnx` at the root); gitignored |

## Running tests directly

The `Tests` binary is a doctest runner, so the doctest options work on it directly:

```
bin/Debug-windows-x86_64/Tests/Tests.exe --list-test-cases
bin/Debug-windows-x86_64/Tests/Tests.exe --test-case="Smoke: test runner starts"
bin/Debug-windows-x86_64/Tests/Tests.exe -tc="Jolt:*" -s           # wildcard; -s also prints passing assertions
bin/Debug-windows-x86_64/Tests/Tests.exe --test-suite=Core          # one module (TEST_SUITE name)
bin/Debug-windows-x86_64/Tests/Tests.exe -tc="Unit: case" -sc="variation"   # one SUBCASE
bin/Debug-windows-x86_64/Tests/Tests.exe --reporters=junit --out=bin/TestResults/Local.xml
```

- **Commas separate filters**, so a case name that contains a comma needs `\,`, or a `*` wildcard in place of the comma. For example, use `-tc="Physics: state hash identical with 0*"`.
- **Engine-specific modes** are added to the Tests main as their milestones land (M1–M5, Architecture §15.2). They are for tests that spawn child processes; run the parent test case instead of calling them by hand. At most one child mode per run:
  - `--death-test=<name>` runs one death-test body.
  - `--windowed-child=<case>` runs one ChildTargets test case in a windowed process (GLFW's native platform).
  - `--crash-child` crashes after the process context is up (`--child-argument=fatal-error` takes the fatal-error path instead).
  - `--child-process` marks a process another Tests process started: it logs only to the console and writes no crash report unless it got `--user-data-dir`. Parents append it themselves.
  - `--user-data-dir=<absolute path>` is the child's user-data root, so its crash reports land in the parent's temporary directory.
  - `--child-argument=<text>` is input for a child body, such as the lock file a lock holder takes.
  - `--require-gpu` (M5) turns GPU-suite skips into failures.
  - `--vulkan-api=1.3|1.4` (M5) caps the API of every GPU test's device and of the Editor and Runtime processes it starts.
  - `--update-golden` (M5) makes golden tests write their image as the candidate of this machine's device class.
- **`--test-timeout=<seconds>`** is the per-case limit for test cases without a `doctest::timeout` decorator (default 120). A case that runs longer is reported on stderr and the run exits with code 5.
- **`--no-skip`** also runs the `ChildTargets` suite: cases that exist only as child-process targets of other tests, which hang or end the process by design. Exclude them: `Tests.exe --no-skip --test-suite-exclude=ChildTargets`.
- **Skipped cases.** `--list-test-cases` omits skipped cases. `Tests.exe --no-skip --list-test-cases --reporters=xml --out=<file>` lists every case with its `testsuite` and `skipped` attributes; this is how `Test.py` finds skipped cases outside the `ChildTargets` suite.
- **Exit codes** (§4.1): 0 success, 1 test failure, 2 usage error, 3 init failed, 4 crash or assert (exit 4 is what the parent of a death test expects), 5 timeout.

## Reading failures

- **Build errors.** On MSVC the format is `file(line,col): error C####: ...`; on GCC and Clang it is `file:line:col: error: ...`.
  - Warnings are errors in first-party code. Fix the cause and never suppress the warning.
  - Fix the first error first; later errors are often consequences of it.
- **Shader errors** come from `CompileShaders.py`. For each failing variant it prints slangc's diagnostic followed by a canonical `<file>: error: shader <Program>/<Entry> (<KEY>=<VALUE>, ...) failed to compile` line (without the parenthesis for a program without permutations).
  - When a compile fails, the previous `.spv` files and the stamp are kept.
  - Exit 1 is a compile, validation or tool failure (a slangc or spirv-val run over 300 s is terminated and counts as one). Exit 2 is a usage or configuration error, such as an unknown `--program` or an invalid `Shaders.json`. Exit 3 means slangc or spirv-val is missing or slangc is not the pinned version. Neither 2 nor 3 is a shader bug.
- **doctest failures** look like `file(line): ERROR: CHECK( a == b ) is NOT correct!` followed by `values: CHECK( 1 == 2 )`. Re-run the single case with `-s` to see the passing assertions around the failure.
- **Exit code 4 (crash or assert).** The process printed `Crash: <reason>; report written to <path>` on stderr (or `...; no report written` when it had no report directory, as death-test children do; on Windows `...; incomplete report written to <path>` when the reporter could not finish within 10 s or crashed, and `; crash handler fault on the <crashing|reporter> thread: <fault>` after the reason, in the line and the report's Reason, when the handler itself crashed). The report holds the reason, the breadcrumbs (frame phase, scene, automation method), a symbolized stack trace and the last 256 log lines; a failed assertion or another fatal error instead logs `Fatal error (<Kind>): <message>` at Critical, followed by `Crash report written to '<path>'` in a process that writes reports. Reports of a top-level run are in `<UserData>/<ENGINE_PRODUCT_NAME>/Crashes/` (see "Where outputs land").
- **CheckBuildConfig findings** look like `[windows] Debug: Tests includes JoltPhysics headers but lacks JPH_PROFILE_ENABLED ...`; the bracket names the target (`windows`, `windows-clang`, `linux`, `linux-clang`, `macosx`). The exit codes are 0 clean, 1 findings or a generation failure, 2 usage error, 3 premake missing or not the pinned 5.0.0, 5 timeout. `--keep` keeps the generated projects so you can inspect them.
- **Exit code 3 from any script** means a required tool or file is missing or has the wrong version (premake, clang-format, clang-tidy or clang-query in `--mode clang`, slangc, a compiler, an archiver or lld). Run `python Scripts/Setup.py`, which names what to install.
- **CI.py** prints a summary table with one row per stage and configuration. Fix the first failing stage; later stages did not run.
- **Contract stubs and skips** (Roadmap rule 3, `Docs/Decisions/0004-contract-stub-gate.md`). Lint `contract-stub` means an `ENGINE_CONTRACT_STUB();` stub is still in the tree: implement the function, replacing the whole stub body. Lint `test-skip` or a Test.py `UNEXPECTED SKIP: <case>` line means a test case is still marked `doctest::skip`: remove the decorator once its implementation has landed. Only child-process targets stay skipped, with `doctest::test_suite(Test::ChildTargetSuite)` in the same decorator expression. Only a milestone's contract commit may run `PreCommit.py --contract` (the summary prints "contract mode: stubs and skipped tests allowed").

- **GPU tests** (doctest suites `GPU` and `Golden`, excluded from the unit stage). Every device is created with validation and synchronization validation; the fixture fails the test on any validation or NVRHI error or warning (`Vulkan validation: ...`, `NVRHI: ...` in the log) and on GPU objects still alive at its end (`GpuResourceTracker` counts). `Vulkan loader (...)` lines describe the machine's layers and drivers (a third-party overlay layer, a missing driver) and never fail a test. "GPU test skipped: <reason>" means no usable device: no loader, no Vulkan 1.3 device (the reason lists each candidate's rejection), or a missing `VK_LAYER_KHRONOS_validation`. A golden mismatch writes `<Name>-actual.png`, `-expected.png` and `-diff.png` into `bin/TestResults/Golden/`.
- **Editor and Runtime without a GPU** need `--renderer none`; with the default Vulkan renderer they exit 3 when no device can be created.

## Pitfalls

- **New file not compiled, or a shader edit not rebuilt.** premake expands globs and `buildinputs` when it generates the projects. Re-run `Generate.py` after adding, removing or renaming source or shader files.
- **MSB8029** ("Intermediate or Output directory cannot reside under the Temporary directory"). It appears when a workspace generated with `--to=<temp dir>` is built. Outputs follow `%{wks.location}/bin`, so they land under `%TEMP%`. `CheckBuildConfig.py` only generates into temp and never builds there. For a throwaway build, generate into a directory outside `%TEMP%`. Never silence the warning.
- **ABI define mismatch.** Symptoms: heap corruption, crashes inside a vendor library, or `JPH::VerifyJoltVersionID()` failing at startup. Cause: a library and one of its consumers were compiled with different `JPH_*`, `SPDLOG_*`, `MA_NO_*`, `VULKAN_HPP_*`, `VK_USE_PLATFORM_*`, `NOMINMAX` or `NDEBUG`.
  - Add defines only through `Use<Lib>()`, copied from `VENDOR.md`.
  - Run `CheckBuildConfig.py` after any premake change.
- **NDEBUG ODR trap.** `NDEBUG` changes the layout of vulkan.hpp's dispatcher. Defining it in one project (or `#define NDEBUG` in code) produces Dist-only crashes in Vulkan calls. It is defined at workspace scope for Dist only, and nowhere else.
- **Floating-point flags.** Never add `/fp:fast`, `/Qfast_transcendentals`, `-ffast-math` or any of its components (`-ffinite-math-only`, `-fno-signed-zeros`, `-fno-trapping-math`, ...), `-ffp-contract=fast|on`, or `-ffp-model=precise`. The last one re-enables contraction and trips `-Woverriding-option` under `-Werror` on Clang. GCC and Clang use `-fno-fast-math -ffp-contract=off`; clang-cl takes the same options as `/clang:-fno-fast-math /clang:-ffp-contract=off`. This spelling deviates from Architecture §2.2 by `Docs/Decisions/0002-m0-deviations.md`. `CheckBuildConfig.py` enforces it.
- **Instruction set.** Never give a Jolt consumer other instruction-set options than JoltPhysics (`/arch:SSE4.2`, `-msse4.2 -mpopcnt`): Jolt derives `JPH_USE_AVX2` and friends from `__AVX2__`, and `VerifyJoltVersionID()` does not cover them. `CheckBuildConfig.py` compares them.
- **slangc version mismatch.** Install Vulkan SDK 1.4.350.0 and point `VULKAN_SDK` at it. Change the pin in `Toolchain.json` only as a deliberate toolchain upgrade, together with CI.
- **`python` on POSIX.** The shader build rule calls `python3` on Linux and macOS, so a machine without `python3` fails in the Shaders project.
- **Dist has no Tests, Editor or EditorCore.** Unit tests run in Debug and Release only, and those three projects cannot be built in Dist.
- **Linux Dist with LTO.** The archiver must understand LTO objects: `gcc-ar-N` for GCC, `llvm-ar-N` for Clang (packages `gcc-N`, `llvm-N`). Clang on Linux links with lld (`-fuse-ld=lld` from `Dependencies.lua`, package `lld-N`), which links LTO bitcode natively. `Build.py` (through `Scripts/Lib/toolchain.py`) selects them, or fails naming the missing package instead of falling back to plain `ar`; `CC`, `CXX` and `AR` from the environment still win.
- **macOS warnings** like `libtool: ... has no symbols` for libraries that hold only a PCH object are benign until real code lands. Treat any other warning as a defect.
- **Stale outputs after a toolchain change** (VS update, new SDK). Rebuild from clean: delete `bin/` and `bin-int/`, then re-run `Generate.py`. Never reuse build outputs across toolchains.
