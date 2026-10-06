# JoltPhysics (vendored)

| | |
|---|---|
| Upstream | https://github.com/jrouwe/JoltPhysics |
| Version | 5.6.0 (tag `v5.6.0`, the latest stable release when this was vendored) |
| Commit | `e77f175595e64cb44218cc9d9d56fc365ad0e36a` (lightweight tag; commit dated 2026-07-11) |
| Date vendored | 2026-10-05 |
| License | MIT (SPDX: `MIT`), see `LICENSE` |
| Premake project | `JoltPhysics` (`kind "StaticLib"`, `language "C++"`, `cppdialect "C++17"`), defined in `premake5.lua` |

## What was kept

Every kept file is byte-identical to upstream at the commit above. Files use LF line endings, and this was checked by comparing git blob hashes.

- `LICENSE`
- `Jolt/`: the complete library source tree (`*.h`, `*.inl`, `*.cpp`, `Jolt.natvis`), with the upstream layout unchanged. The two exceptions are listed below.

Some files are kept but **not compiled**, because all compute backends are disabled (see the build configuration below):
- `Jolt/Compute/CPU/`, `Jolt/Compute/DX12/`, `Jolt/Compute/VK/`, `Jolt/Compute/MTL/` (`*.mm`)
- `Jolt/Shaders/*.hlsl`, `Jolt/Shaders/HairWrapper.cpp`, `Jolt/Shaders/TestComputeWrapper.cpp`

These files stay so that `Jolt/` remains a plain copy of upstream. Turning a backend on later then only needs a premake change. `Jolt/Shaders/HairStructs.h` and `Jolt/Shaders/ShaderCore.h` are still needed, because the always-built `Physics/Hair/*` code includes them.

## What was removed

- `Jolt/Jolt.cmake` (the CMake build script; its file list and options were mirrored in `premake5.lua`)
- `Jolt/Physics/Collision/Shape/TaperedCapsuleShape.gliffy` (a documentation diagram source)
- Everything outside `Jolt/`:
  - `Build/` (CMake, platform scripts, Android/iOS/macOS project files)
  - `UnitTests/`, `HelloWorld/`, `PerformanceTest/`, `Samples/`, `TestFramework/`, `JoltViewer/`, `Assets/`
  - `Docs/`, `Doxyfile`, `doxygen-awesome.css`, `run_doxygen.bat`, `README.md`, `ContributorAgreement.md`
  - `sonar-project.properties`, `.github/`, `.clang-format`, `.editorconfig`, `.gitattributes`, `.gitignore`

## Local modifications

None to the sources: every file under `Jolt/` is byte-identical to upstream. The build configuration deliberately differs
from upstream's defaults in one respect, cross-platform determinism; see "Determinism" below.

## Build configuration

`premake5.lua` mirrors upstream `Jolt/Jolt.cmake` and `Build/CMakeLists.txt` for a static library built with
`CROSS_PLATFORM_DETERMINISTIC=ON`. The source list is exactly upstream's `JOLT_PHYSICS_SRC_FILES` (130 `.cpp` files), plus the `ENABLE_OBJECT_STREAM` files (8 `.cpp` files), plus `Jolt.natvis` on Windows. That is 138 translation units on every platform. The generated gmake (Linux, macOS), ninja and xcode4 projects were cross-checked to contain all 138 objects.

### Upstream CMake options and the choice made here

| Upstream option | Upstream default | Here | Effect |
|---|---|---|---|
| `DOUBLE_PRECISION` | OFF | OFF | no `JPH_DOUBLE_PRECISION` |
| `CROSS_PLATFORM_DETERMINISTIC` | OFF | **ON** | `JPH_CROSS_PLATFORM_DETERMINISTIC` in every configuration, plus the precise floating-point model without FMA contraction on every compiler and architecture (MSVC `/fp:precise`, GCC/Clang `-ffp-contract=off`). See "Determinism" below |
| `ENABLE_OBJECT_STREAM` | ON | ON | `JPH_OBJECT_STREAM` |
| `OBJECT_LAYER_BITS` | 16 | 16 | `JPH_OBJECT_LAYER_BITS=16` |
| `USE_ASSERTS` | OFF (asserts are implicit in Debug) | Debug and Release | `JPH_ENABLE_ASSERTS` |
| `DEBUG_RENDERER_IN_DEBUG_AND_RELEASE` | ON | ON | `JPH_DEBUG_RENDERER` in Debug and Release |
| `PROFILER_IN_DEBUG_AND_RELEASE` | ON | ON | `JPH_PROFILE_ENABLED` in Debug and Release (internal profiler, not `JPH_EXTERNAL_PROFILE`) |
| `FLOATING_POINT_EXCEPTIONS_ENABLED` | ON (MSVC only, Debug and Release) | MSVC only, **Debug only** | `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED` |
| `USE_SSE4_1` / `USE_SSE4_2` | ON | ON (x86_64) | `JPH_USE_SSE4_1`, `JPH_USE_SSE4_2` |
| `USE_AVX` / `USE_AVX2` | ON | **OFF** | see CPU features below |
| `USE_AVX512` | OFF | OFF | |
| `USE_LZCNT` / `USE_TZCNT` / `USE_F16C` / `USE_FMADD` | ON | **OFF** | see CPU features below. FMA is also incompatible with cross-platform determinism: `Jolt/Core/Core.h` never enables `JPH_USE_FMADD` when `JPH_CROSS_PLATFORM_DETERMINISTIC` is defined |
| `JPH_USE_DX12` / `JPH_USE_VK` / `JPH_USE_MTL` / `JPH_USE_CPU_COMPUTE` | ON | **OFF** | Compute backends are only used by `JPH::Hair` (GPU strand simulation, a fallback used mainly for debugging). The engine does not use hair. They would also pull in D3D12/Metal link dependencies, a second Vulkan device next to NVRHI, and an offline `dxc` shader step. With them off, `JPH::CreateComputeSystem()` returns an error result. |
| `DISABLE_CUSTOM_ALLOCATOR`, `USE_STD_VECTOR`, `TRACK_*_STATS`, `JPH_USE_EXTERNAL_PROFILE` | OFF | OFF | |
| `JPH_BUILD_SHARED_LIBS` | OFF | OFF | static library. Do not define `JPH_SHARED_LIBRARY`. |
| `CPP_RTTI_ENABLED` / `CPP_EXCEPTIONS_ENABLED` | OFF | **compiler default (on)** | see compiler flags below |
| `INTERPROCEDURAL_OPTIMIZATION` | ON | not enabled | LTO is a workspace-wide decision. A `/GL` or `-flto` static library forces LTO on every consumer. |
| `USE_STATIC_MSVC_RUNTIME_LIBRARY` | ON | OFF | `staticruntime "off"` (dynamic CRT), the same as every project in the workspace |
| `ENABLE_ALL_WARNINGS` | ON (`/Wall /WX`, `-Wall -Werror`) | OFF | Vendor code builds at the default warning level. It builds with 0 warnings on MSVC 14.51. |

### Public defines (identical for the library and every consumer)

Jolt encodes these in `JPH_VERSION_ID`, and the class layouts depend on them. `JPH::VerifyJoltVersionID()` fails if a consumer was compiled differently. A mismatch is **memory corruption**, not merely a link error.

| Scope | Defines |
|---|---|
| All configurations | `JPH_OBJECT_STREAM`, `JPH_OBJECT_LAYER_BITS=16`, `JPH_CROSS_PLATFORM_DETERMINISTIC` |
| `architecture:x86_64` | `JPH_USE_SSE4_1`, `JPH_USE_SSE4_2` |
| `configurations:Debug` | `JPH_DEBUG_RENDERER`, `JPH_PROFILE_ENABLED`, `JPH_ENABLE_ASSERTS` |
| `configurations:Debug` + `toolset:msc*` (Windows/MSVC) | `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED` |
| `configurations:Release` | `JPH_DEBUG_RENDERER`, `JPH_PROFILE_ENABLED`, `JPH_ENABLE_ASSERTS`, `JPH_NO_DEBUG` |
| `configurations:Dist` | `JPH_NO_DEBUG` |

Notes:
- **`JPH_NO_DEBUG`**: `Jolt/Core/Core.h` defines `JPH_DEBUG` whenever `NDEBUG` is not defined. `JPH_DEBUG` silently forces `JPH_ENABLE_ASSERTS` on, which changes layouts and the version ID. Upstream avoids this by exporting a public `NDEBUG` in Release and Distribution. Here, the Jolt-specific `JPH_NO_DEBUG` is used instead. Jolt's state is then fixed by its own defines, whatever the workspace does with `NDEBUG` (Jolt itself never uses `assert()`).
- **Floating point exceptions** are MSVC-only, as upstream does it. Upstream ignores the option for Clang and GCC, because Clang may vectorize `Float2` and raise spurious exceptions. Linux and macOS Debug builds therefore do **not** define `JPH_FLOATING_POINT_EXCEPTIONS_ENABLED`. They are Debug-only here (upstream also enables them in Release) so that Release behaves like a shipping build.
- On ARM64 (macOS), Jolt enables NEON by itself from compiler macros. No defines are needed. Without
  `JPH_CROSS_PLATFORM_DETERMINISTIC` it would also enable FMA (`__ARM_FEATURE_FMA`); with it, it does not.
- **`JPH_CROSS_PLATFORM_DETERMINISTIC`** is feature bit 2 of `JPH_VERSION_ID`, so a consumer built without it fails
  `JPH::VerifyJoltVersionID()`. The engine checks this at startup (fatal) and in the unit test
  `"Jolt: version ID includes JPH_CROSS_PLATFORM_DETERMINISTIC"`.

### CPU features (x86_64): x86-64-v2 baseline

Jolt is built with **SSE4.1 + SSE4.2 (+ POPCNT)**: the x86-64-v2 level, which Windows 11 24H2 also requires. **AVX, AVX2, AVX-512, LZCNT, TZCNT, F16C and FMA3 are not enabled.**

Reasoning:
- LZCNT, TZCNT (BMI1), F16C and FMA3 are all x86-64-v3 features. They arrived with Haswell (Intel, 2013) and Piledriver (AMD, 2012). Many Vulkan-1.3-capable machines still lack them: Intel Atom-class Pentium Silver and Celeron (Goldmont, Goldmont Plus and Tremont, sold until about 2021), Core-based Pentium and Celeron parts with AVX fused off, and pre-Haswell desktops fitted with newer GPUs.
- These failures are serious. F16C and FMA are VEX-encoded and crash with an illegal-instruction fault. `LZCNT` is worse: on CPUs without it, it decodes as `BSR` and **silently returns wrong results**.
- The gain is modest. FMA saves an instruction in some vector multiply-adds. LZCNT and TZCNT replace a `BSR`/`BSF` plus a zero check. Without F16C, half-float AABB-tree node decoding (used by `MeshShape` and `StaticCompoundShape`) falls back to software. Jolt's broad-phase and solver hot paths stay SIMD-vectorized with SSE4.1 either way.
- For a general-purpose engine that ships exported games to unknown hardware, a guaranteed launch outweighs a modest speedup. This can be revisited later with an opt-in x86-64-v3 build (add the defines, `/arch:AVX2` and `-mavx2 -mbmi -mlzcnt -mf16c -mfma` to the library and all consumers together).

Matching compiler flags:
- MSVC (x86_64): `/arch:SSE4.2`. This is supported by MSVC 14.51, accepted without warnings, and lets the optimizer use the same ISA level as the intrinsics. Like upstream, Jolt's own `JPH_USE_*` defines drive the intrinsics.
- GCC/Clang (x86_64): `-msse4.2 -mpopcnt -mfpmath=sse`, as upstream does for `USE_SSE4_2`.
- clang-cl (x86_64, the portability build of Docs/Architecture.md §15.8): `-msse4.2 -mpopcnt`, which clang-cl
  accepts as they are. It rejects `/arch:SSE4.2` (its 64-bit `/arch:` values start at AVX).
- ARM64: no flags. Upstream adds none either.

### Compiler flags

| Flag | Where | Why |
|---|---|---|
| PCH `Jolt/Jolt.h` (created from `Jolt/RegisterTypes.cpp`) | MSVC only | Upstream builds with `Jolt.h` as the precompiled header, and every Jolt `.cpp` includes it first. It is MSVC-only so that the Linux and macOS generators, which cannot be tested on the vendoring machine, keep the simplest possible setup. |
| `floatingpoint "Default"` (`/fp:precise`) | MSVC only | Upstream's MSVC setting for `CROSS_PLATFORM_DETERMINISTIC=ON`. Replaces upstream's default, non-deterministic `/fp:fast`. Since VS 2022, `/fp:precise` never contracts into FMA. premake emits no `FloatingPointModel` element for "Default", so MSVC's default `/fp:precise` applies. |
| `multiprocessorcompile "On"` (`/MP`) | MSVC only | Upstream sets `/MP`. 138 translation units. |
| `-fno-fast-math` | Linux, macOS | Upstream never allows fast-math. premake's **xcode4** generator adds `-ffast-math` for `optimize "Full"` (the Dist configuration). This flag comes after it and turns it back off. |
| `-ffp-contract=off` | Linux, macOS, every architecture | Upstream's GCC/Clang setting for `CROSS_PLATFORM_DETERMINISTIC=ON`. Replaces upstream's default ARM64 `-ffp-contract=on`, which lets the compiler fuse multiply-adds and so changes results between architectures and optimization levels. Together with Clang's default precise model this is `-ffp-model=precise` without contraction. The literal spelling `-ffp-model=precise -ffp-contract=off` is not used: current Clang reports it as `-Woverriding-option` (verified with Clang 22.1.3), an error under `-Werror`, and `-ffp-model=precise` alone sets `-ffp-contract=on`. |
| `-pthread` | Linux, macOS | As upstream (public there) |
| `-faligned-allocation` | macOS | As upstream for AppleClang |
| `-Wno-stringop-overflow -Wno-psabi` | GCC | As upstream (false positives and ABI notes) |
| `pic "On"` | Linux | The workspace convention for static libraries |
| `/clang:-fno-fast-math /clang:-ffp-contract=off` | clang-cl (`filter { "action:vs*", "toolset:clang" }`) | The same precise model as GCC/Clang for the clang-cl portability build (Docs/Architecture.md §15.8). clang-cl does not accept these GCC-style options directly (`-Wunknown-argument`); `/clang:` passes them to the clang driver. |

Upstream flags that were deliberately **not** mirrored:
- `/GR-` + no `/EHsc`, `-fno-rtti -fno-exceptions`: Jolt does not need RTTI or exceptions, but it is compiled with the compiler defaults, the same as the engine. Two reasons. First, premake's `exceptionhandling "Off"` also defines `_HAS_EXCEPTIONS=0`, which would build the MSVC STL inline code in Jolt differently from every consumer. Second, mixing `/GR` and `/GR-` gives inline vtables in Jolt headers two incompatible COMDAT variants. On x64, the cost of leaving them enabled is negligible.
- `/GS-`, `/Oi /Ot /Gy` details, LTO/IPO: the workspace template's optimization settings are used instead.

### Determinism (build-configuration change from upstream defaults)

This is the one place where this build intentionally departs from upstream's default options (Docs/Architecture.md
§2.2 and §9.1, Roadmap M0). The engine requires identical simulation results in Debug, Release and Dist, so recorded
replays and expected state hashes are valid in every configuration.

| | Upstream default | Here (= upstream with `CROSS_PLATFORM_DETERMINISTIC=ON`) |
|---|---|---|
| Public define | none | `JPH_CROSS_PLATFORM_DETERMINISTIC` in every configuration, for the library and every consumer |
| MSVC floating point | `/fp:fast` | `/fp:precise` (premake `floatingpoint "Default"`) |
| GCC/Clang x86_64 | `-ffp-contract=off` (FMA is not enabled in this build) | `-ffp-contract=off` |
| GCC/Clang ARM64 | `-ffp-contract=on` | `-ffp-contract=off` |
| FMA (`JPH_USE_FMADD`) | on where the CPU target has it | never (Jolt disables it under `JPH_CROSS_PLATFORM_DETERMINISTIC`) |

- **Cost.** Jolt documents cross-platform determinism as roughly 8 % slower. This is accepted: in exchange, the same
  inputs give bit-identical results regardless of compiler, build configuration, OS and architecture (Jolt's own
  guarantee). The engine itself claims only "same toolchain and platform, any configuration"; see Architecture §1.2.
- **Sources** stay byte-identical to upstream; only `premake5.lua` changed.
- **Checks.** `Scripts/CheckBuildConfig.py` fails if the define is missing from this project or any consumer, if a
  consumer's instruction-set options differ from this project's, or if this project uses a non-precise floating-point
  model (`/fp:fast`, `/Qfast_transcendentals`, `FloatingPointModel=Fast`, `-ffast-math` or any of its components, or
  any `-ffp-contract` other than `off`). At runtime `JPH::VerifyJoltVersionID()` rejects a consumer without the define.

## Consumer usage

- Include path: `Vendor/JoltPhysics`, or `%{wks.location}/Vendor/JoltPhysics` in the root premake. Include as `#include <Jolt/Jolt.h>` (always first), then the specific headers, e.g. `#include <Jolt/Physics/PhysicsSystem.h>`.
- Link: `links { "JoltPhysics" }`
- Every project whose sources include Jolt headers (Engine, and Editor/Runtime/Tests if they touch Jolt types) **must** use the same defines and x86 flags. `Dependencies.lua` implements this as `UseJoltPhysics()`, which also links `JoltPhysics` (plus `pthread` on Linux) into executables. This is its body (`LinkingKinds` is `"kind:ConsoleApp or WindowedApp or SharedLib"`, and `RepositoryRoot` the absolute repository path):

```lua
IncludeDir["JoltPhysics"] = RepositoryRoot .. "/Vendor/JoltPhysics"

-- Jolt Physics (Vendor/JoltPhysics/VENDOR.md). Must stay identical to Vendor/JoltPhysics/premake5.lua; checked at
-- runtime by JPH::VerifyJoltVersionID() and at generation time by Scripts/CheckBuildConfig.py.
function UseJoltPhysics()
	externalincludedirs { IncludeDir.JoltPhysics }
	defines { "JPH_OBJECT_STREAM", "JPH_OBJECT_LAYER_BITS=16", "JPH_CROSS_PLATFORM_DETERMINISTIC" }

	filter "architecture:x86_64"
		defines { "JPH_USE_SSE4_1", "JPH_USE_SSE4_2" }

	filter { "toolset:msc*", "architecture:x86_64" }
		buildoptions { "/arch:SSE4.2" }

	-- Required: Jolt's inline SSE4 intrinsics do not compile without them. GCC, Clang and clang-cl all accept them.
	filter { "toolset:gcc or clang", "architecture:x86_64" }
		buildoptions { "-msse4.2", "-mpopcnt" }

	filter "configurations:Debug"
		defines { "JPH_DEBUG_RENDERER", "JPH_PROFILE_ENABLED", "JPH_ENABLE_ASSERTS" }

	filter { "configurations:Debug", "toolset:msc*" }
		defines { "JPH_FLOATING_POINT_EXCEPTIONS_ENABLED" }

	filter "configurations:Release"
		defines { "JPH_DEBUG_RENDERER", "JPH_PROFILE_ENABLED", "JPH_ENABLE_ASSERTS", "JPH_NO_DEBUG" }

	filter "configurations:Dist"
		defines { "JPH_NO_DEBUG" }

	filter { LinkingKinds }
		links { "JoltPhysics" }

	filter { LinkingKinds, "system:linux" }
		links { "pthread" }

	filter {}
end
```

- The workspace must set `architecture` (`"x86_64"` on Windows and Linux, `"ARM64"` on macOS), because the SSE settings are filtered on it.
- The instruction-set options must also be identical: `Jolt/Core/Core.h` derives `JPH_USE_AVX2`, `JPH_USE_AVX`, `JPH_USE_F16C`, `JPH_USE_LZCNT`, `JPH_USE_TZCNT` and others from the compiler's `__AVX2__`, `__AVX__`, ... macros, and those bits are not part of `JPH_VERSION_ID`. A consumer built with `/arch:AVX2` or `-mavx2` would compile Jolt's inline vector code differently from the library. `Scripts/CheckBuildConfig.py` compares them.
- The workspace adds the include directory with `externalincludedirs`, as Architecture §2.2 requires for every vendor include directory, so Jolt headers are compiled with vendor warning settings. They were also verified to compile cleanly in a C++23 consumer at `/W4 /WX` with plain `includedirs`.

### Link requirements per platform

The final executable needs these. A static library does not carry link options.

| Platform | Needs |
|---|---|
| Windows | nothing beyond the CRT and the default kernel32 |
| Linux | `links { "pthread" }` or `linkoptions { "-pthread" }` |
| macOS | nothing (pthread is part of libSystem) |

### Runtime contract (for the engine's physics module)

1. Call `JPH::RegisterDefaultAllocator()`, or `JPH::Allocate`/`Free`/`Reallocate`/`AlignedAllocate`/`AlignedFree` to supply your own. Do this before any Jolt allocation.
2. Set `JPH::Trace`, and with `JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = ...;)` route asserts into `ENGINE_CORE_*` logging and `ENGINE_ASSERT`.
3. Call `JPH::VerifyJoltVersionID()` and treat `false` as fatal. It catches any define mismatch.
4. Run `JPH::Factory::sInstance = new JPH::Factory(); JPH::RegisterTypes();`. Shutdown is the reverse: `JPH::UnregisterTypes(); delete JPH::Factory::sInstance; JPH::Factory::sInstance = nullptr;`.
5. Floating point exceptions in Windows Debug builds: during `PhysicsSystem::Update`, Jolt unmasks divide-by-zero, invalid and overflow exceptions on the worker threads. Engine callbacks that run inside the update (contact listeners, body activation listeners, character/vehicle callbacks, and so on) must therefore not produce NaN/Inf, or they will trap. That trap is the purpose of the feature.
6. Profiler (`JPH_PROFILE_ENABLED` in Debug and Release): it is inert until `JPH_PROFILE_START(name)` is called. `JPH_PROFILE_NEXTFRAME()` and `JPH_PROFILE_DUMP()` dereference `JPH::Profiler::sInstance`, so call them only after `JPH_PROFILE_START`.
7. Resting contacts penetrate by up to `PhysicsSettings::mPenetrationSlop` (0.02 m by default). A sphere of radius 0.5 settles at y = 0.48 on a floor at y = 0. Tests must account for this.
8. Never derive engine logic from `JPH_DEBUG`. It is off in Release and Dist by design.

## Verification performed (Windows, VS 2026 / MSVC 14.51, x64)

### clang-cl portability build (2026-10-05, Roadmap M0 integration)

- `Scripts/CI.py`'s portability stage generates the workspace with `--cc=clang` (vs2026, ClangCL platform toolset,
  clang-cl 22.1.3) and builds `Tests` in Release, which compiles this project with the clang-cl branches above. Jolt
  and every first-party project compile; first-party code builds with `/WX`.

### Cross-platform deterministic build (2026-10-05, Roadmap M0)

- A throwaway workspace outside the repository included this `premake5.lua` and a C++23 `/W4 /WX` console app using
  `UseJoltPhysics()` from `Dependencies.lua`, generated with `premake5 vs2026`. Debug, Release and Dist built with
  0 compiler warnings.
- The app calls `RegisterDefaultAllocator`, `VerifyJoltVersionID` (passes) and `Factory`/`RegisterTypes`, drops a
  dynamic 0.5 m sphere with an initial velocity of (0.3, -1, 0.2) m/s onto a static box floor and steps 120 times at
  1/60 s on a 4-thread `JobSystemThreadPool`. The final sphere position is **bit-identical in all three
  configurations**: (0.439786, 0.479987, 0.293199), IEEE bits `3ee12bab 3ef5c0de 3e961e39`.
- `GetConfigurationString()`:
  - Debug: `SSE2 SSE4.1 SSE4.2 (Cross Platform Deterministic) (FP Exceptions) (Debug Renderer) (Profile) (16-bit ObjectLayer) (Assertions) (ObjectStream) (Debug) (C++ RTTI) (C++ Exceptions)`
  - Release: `SSE2 SSE4.1 SSE4.2 (Cross Platform Deterministic) (Debug Renderer) (Profile) (16-bit ObjectLayer) (Assertions) (ObjectStream) (C++ RTTI) (C++ Exceptions)`
  - Dist: `SSE2 SSE4.1 SSE4.2 (Cross Platform Deterministic) (16-bit ObjectLayer) (ObjectStream) (C++ RTTI) (C++ Exceptions)`
- In the engine workspace, `Tests` passes `"Jolt: version ID includes JPH_CROSS_PLATFORM_DETERMINISTIC"` in Debug and
  Release, and `Scripts/CheckBuildConfig.py` passes on the workspace and fails on its three fixture workspaces.
- Linux and macOS: the generated gmake, ninja and xcode4 projects were checked for `-ffp-contract=off` and the absence
  of `-ffp-contract=on` and `-ffast-math`; GitHub Actions compiles them.

### Original vendoring (2026-10-05, upstream default configuration)

- A throwaway workspace includes this `premake5.lua` plus a C++23 `Smoke` console app with the consumer settings above, generated with `premake5 vs2026`. Debug, Release and Dist each built with 0 warnings and 0 errors.
- The Smoke app calls `RegisterDefaultAllocator`, `VerifyJoltVersionID` and `Factory`/`RegisterTypes`. It builds a `PhysicsSystem` with minimal layer interfaces and filters, a static floor box and a dynamic sphere, and steps 120 times at 1/60 s with `JobSystemThreadPool` and `TempAllocatorImpl`. The sphere ends at (0, 0.48, 0), asleep, in all three configurations. Cleanup is complete.
- `GetConfigurationString()`:
  - Debug: `SSE2 SSE4.1 SSE4.2 (FP Exceptions) (Debug Renderer) (Profile) (16-bit ObjectLayer) (Assertions) (ObjectStream) (Debug)`
  - Release: `SSE2 SSE4.1 SSE4.2 (Debug Renderer) (Profile) (16-bit ObjectLayer) (Assertions) (ObjectStream)`
  - Dist: `SSE2 SSE4.1 SSE4.2 (16-bit ObjectLayer) (ObjectStream)`
- Negative test: dropping `JPH_PROFILE_ENABLED` from the consumer makes `VerifyJoltVersionID()` fail, as intended.
- The Smoke app was rebuilt at `/W4 /WX` (C++23): Jolt headers are warning-free.
- Linux (gmake, ninja) and macOS (gmake, ninja, xcode4) projects were generated with `--os=` and inspected: 138 objects, the expected defines and flags, and no PCH. They could not be compiled on this machine.

## Updating

1. Find the latest release: `git ls-remote --tags https://github.com/jrouwe/JoltPhysics`.
2. Clone it with LF endings: `git -c core.autocrlf=false -c core.eol=lf clone --depth 1 --branch vX.Y.Z https://github.com/jrouwe/JoltPhysics src`.
3. Replace `Vendor/JoltPhysics/Jolt/` with `src/Jolt/` and `LICENSE` with `src/LICENSE`. Then delete `Jolt/Jolt.cmake` and `Jolt/Physics/Collision/Shape/TaperedCapsuleShape.gliffy` again.
4. Diff the new `Jolt/Jolt.cmake` against the previous version:
   - Regenerate the `files` list in `premake5.lua` from `JOLT_PHYSICS_SRC_FILES` (main list, minus `Jolt.cmake`/`.gliffy`) plus the `ENABLE_OBJECT_STREAM` block.
   - Check every listed file exists.
   - Review new options, defines and compiler flags in `Jolt.cmake` and `Build/CMakeLists.txt`.
5. Check `Jolt/Core/Core.h` for new `JPH_VERSION_FEATURE_BIT_*` entries, which are new ABI-relevant defines. Update the consumer define list (`Dependencies.lua`) in the same change.
6. Rebuild all configurations, run the engine's physics unit tests and the FeatureTest scene, and update this file (version, commit, date).
