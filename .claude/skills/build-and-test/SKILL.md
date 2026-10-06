---
name: build-and-test
description: Use this skill to generate project files, build, run unit tests or the CI pipeline, run a single doctest case, or work out why a build, shader compile, test or build-configuration check failed. It covers MSBuild/make/xcodebuild errors, slangc/spirv-val errors, CheckBuildConfig findings, ABI define mismatches, the NDEBUG ODR trap, MSB8029 and missing files after adding sources.
---

# Build and test

This skill gives the commands, configurations, output locations and failure diagnosis for this repository. The authoritative sources are Architecture §2 (build), §15 (testing, CI matrix) and `AGENTS.md`. Run everything from the repository root. Use `python` on Windows and `python3` on Linux and macOS. Every script has `--help` and `--json`, and exits non-zero on failure.

## Toolchain

| | Windows | Linux | macOS |
|---|---|---|---|
| Compiler | VS 2026, toolset v145, MSVC 14.51 | GCC 14 (`CC=gcc-14 CXX=g++-14`) or Clang 18+ | Xcode 26+ (Apple Clang), arm64 |
| Generator | `vs2026` (`Yggdrasil.slnx`) | `gmake` (or `ninja`) | `xcode4` |
| Packages | Vulkan SDK 1.4.350.0 | `xorg-dev` (GLFW X11), Vulkan SDK 1.4.350.0 | Vulkan SDK 1.4.350.0 (MoltenVK) |

- `VULKAN_SDK` must point at SDK 1.4.350.0, which provides `slangc` and `spirv-val` for the `Shaders` project.
- `Scripts/Lib/Toolchain.json` pins the SDK version and slangc 2026.8. Any other slangc version makes `CompileShaders.py` exit 3.
- premake 5.0.0 lives in `Vendor/premake/bin/`. `Setup.py` downloads it, SHA-256 verified, when it is missing.

## Commands

```
python Scripts/Setup.py                                    # toolchain checks, premake download
python Scripts/Generate.py                                 # vs2026 | gmake | xcode4 (+ compile-commands); --action to override
python Scripts/Build.py --config Debug                     # Release, Dist; --project Tests builds one project
python Scripts/Test.py --suite unit --config Debug --junit # Release too; Dist has no Tests project
python Scripts/CompileShaders.py --config Debug            # --program P, --force, --verbose
python Scripts/CheckBuildConfig.py                         # ABI defines, JPH_CROSS_PLATFORM_DETERMINISTIC, Jolt ISA, FP model, Dist solution
python Scripts/Format.py --check                           # without --check it rewrites files
python Scripts/Lint.py                                     # --self-test: every seeded Tests/Data/Lint fixture fails as expected; --mode clang|regex
python Scripts/PreCommit.py                                # the commit gate
python Scripts/CI.py                                       # all stages; --stages build,unit for a subset
```

**CI stages** run in order and fail fast: `setup → generate → lint → build → unit → portability`. Later milestones add bake, gpu, golden, feature, automation, export, determinism and games. The configurations follow the §15.8 matrix:
- lint: `CheckBuildConfig.py` on the workspace and on each fixture workspace under `Tests/Data/BuildConfig/` (each must fail with its own defect), `Lint.py`, `Lint.py --self-test` and `Format.py --check`. `PreCommit.py` runs exactly this list too (`Scripts/Lib/scripts.py`);
- build: Debug, Release and Dist, plus, after Debug, `"Shaders: slang-only change is not skipped by the up-to-date check"` (touching only a `.slang` file re-runs the shader rule; the next build skips it);
- unit: Debug and Release;
- portability: generates Linux gmake/ninja and macOS xcode4 projects (checked against the vs2026 reference; the xcode4 and gmake precompiled-header paths must resolve and the xcode4 Dist projects must enable LTO), then builds Tests with clang-cl in Release when the VS Clang component is installed (`--no-clang-cl` leaves it out, `--require-clang-cl` makes a missing component a failure). Only warnings located under `Vendor/` are tolerated in that build.

**GitHub Actions** (`.github/workflows/ci.yml`) runs `CI.py` everywhere, with the stages a GPU-less hosted runner supports (setup, generate, lint, build, unit, portability):
- Windows: one `CI.py` run, with `--require-clang-cl`.
- Linux (GCC 14 and Clang 18) and macOS: one `CI.py` run per step (setup and generate, lint, Debug, Release, Dist, portability), so every configuration is reported even when another one failed. Each run writes `bin/TestResults/CI-<step>.xml`.
- JUnit results are uploaded as artifacts named `test-results-windows`, `test-results-linux-gcc`, `test-results-linux-clang` and `test-results-macos`.

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
- **Engine-specific modes** are added to the Tests main as their milestones land (M1–M5, Architecture §15.2): `--death-test=<name>` runs one death-test body and `--windowed-child=<name>` runs a windowed child case; `--require-gpu` turns GPU-suite skips into failures. These are for tests that spawn child processes. Run the parent test case instead of calling them by hand.
- **Exit codes** (§4.1): 0 success, 1 test failure, 2 usage error, 3 init failed, 4 crash or assert (exit 4 is what the parent of a death test expects), 5 timeout.

## Reading failures

- **Build errors.** On MSVC the format is `file(line,col): error C####: ...`; on GCC and Clang it is `file:line:col: error: ...`.
  - Warnings are errors in first-party code. Fix the cause and never suppress the warning.
  - Fix the first error first; later errors are often consequences of it.
- **Shader errors** come from `CompileShaders.py`. For each failing variant it prints slangc's diagnostic followed by a canonical `<file>: error: shader <Program>/<Entry> (<KEY>=<VALUE>, ...) failed to compile` line (without the parenthesis for a program without permutations).
  - When a compile fails, the previous `.spv` files and the stamp are kept.
  - Exit 1 is a compile, validation or tool failure (a slangc or spirv-val run over 300 s is terminated and counts as one). Exit 2 is a usage or configuration error, such as an unknown `--program` or an invalid `Shaders.json`. Exit 3 means slangc or spirv-val is missing or slangc is not the pinned version. Neither 2 nor 3 is a shader bug.
- **doctest failures** look like `file(line): ERROR: CHECK( a == b ) is NOT correct!` followed by `values: CHECK( 1 == 2 )`. Re-run the single case with `-s` to see the passing assertions around the failure.
- **CheckBuildConfig findings** look like `[windows] Debug: Tests includes JoltPhysics headers but lacks JPH_PROFILE_ENABLED ...`; the bracket names the target (`windows`, `windows-clang`, `linux`, `linux-clang`, `macosx`). The exit codes are 0 clean, 1 findings or a generation failure, 2 usage error, 3 premake missing or not the pinned 5.0.0, 5 timeout. `--keep` keeps the generated projects so you can inspect them.
- **Exit code 3 from any script** means a required tool or file is missing or has the wrong version (premake, clang-format, clang-tidy or clang-query in `--mode clang`, slangc, a compiler, an archiver or lld). Run `python Scripts/Setup.py`, which names what to install.
- **CI.py** prints a summary table with one row per stage and configuration. Fix the first failing stage; later stages did not run.

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
