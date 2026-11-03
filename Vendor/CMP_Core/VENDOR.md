# CMP_Core (Compressonator)

| | |
|---|---|
| Upstream | https://github.com/GPUOpen-Tools/compressonator (the `cmp_core` sources and the `cmp_math` helpers they link) |
| Version | `V4.6.0` (release of 2026-10-20). Its `cmp_core`, `applications/_libs/cmp_math` and `license` trees are identical to `V4.5.52`'s (2024-01-31): the same git tree objects `e67af1d9`, `94cbb146` and `d6ccafb5` |
| Commit | `095a143ad8cfbf9ed5afa4ea63c735bfd0fd04dd` (2026-10-19, "Compressonator v4.6") |
| Date vendored | 2026-10-29 (M23 contract C23.1; approved in `Docs/Decisions/0001-approvals.md`) |
| License | MIT (SPDX: `MIT`), Advanced Micro Devices, Inc. `LICENSE` is upstream's `license/license.txt` verbatim (byte-identical to `license/corelicense.txt`); every kept source file carries the same notice in its header. |
| Kind | C++ sources; static library, premake project `CMP_Core` (`premake5.lua`) |

## Contents

Kept (byte-identical to upstream at the commit above, at their upstream paths, LF line endings as stored upstream):

| File | SHA-256 |
|---|---|
| `applications/_libs/cmp_math/cmp_math_common.cpp` | `ed9dd1d35ba411af97e958104affedfe2e361cf1fff0313df53c2fad2a66df01` |
| `applications/_libs/cmp_math/cmp_math_common.h` | `20d3f7988c55a8e80e8615d2907e776d90f6dc36dc0e844a14ef5314413ac529` |
| `applications/_libs/cmp_math/cpu_extensions.cpp` | `4198d33320372291f58eb7f95c343cb6a5eca98a2111c61e14c2dd198b982785` |
| `applications/_libs/cmp_math/cpu_extensions.h` | `333959dd757045db2f2e91a2205eeaf746ef508ebbae278faee9f293ed36272f` |
| `cmp_core/shaders/bc1_cmp.h` | `598b367f25aa28a0371d8e67125e7f28c01c88df131f81dd083be93c40a9df9a` |
| `cmp_core/shaders/bc1_common_kernel.h` | `cf9baaad1a221c2458eeb0cee93cc5f04f22d8a30772dffb5693344e6489f028` |
| `cmp_core/shaders/bc1_encode_kernel.cpp` | `7a1ae8b8531f281f124bb435dcd99131703def5ca03039a29e36bb97d0fc2a2f` |
| `cmp_core/shaders/bc1_encode_kernel.h` | `157498c80baa98287923c311e7caf9102a207191a746ace91352078b7b8b3d3f` |
| `cmp_core/shaders/bc2_encode_kernel.cpp` | `29faa87a6a4bc871a8cd6a653a615e2e5428f86c245373f5f9ac206c38cd9dbe` |
| `cmp_core/shaders/bc2_encode_kernel.h` | `1abb48adf39a3da2bf281f04783b8932c55667b82c76d47cc8c296846a8f9d44` |
| `cmp_core/shaders/bc3_encode_kernel.cpp` | `2943a851608e3520c27dbb02107415c153f00873485313e34c21175bc0d3a234` |
| `cmp_core/shaders/bc3_encode_kernel.h` | `5c46ba260ad3b8641d5b6ccb6551b4100827cd1cb51b54430fc33b6665d238ce` |
| `cmp_core/shaders/bc4_encode_kernel.cpp` | `e31c38e27310dc35f6c6f516edebf67605fba117a705208ffb2c82aeedf846f0` |
| `cmp_core/shaders/bc4_encode_kernel.h` | `709b404e3a1aa6658107eeb1edbb1f290c180a43f64d739bdb2eda6f6326ae72` |
| `cmp_core/shaders/bc5_encode_kernel.cpp` | `132460d539bcf2c75b0c4b80ce239d03789bb8e420400073cf2c443ec644f46f` |
| `cmp_core/shaders/bc5_encode_kernel.h` | `a7a1258563339699c1ebee84c4c1c82c0347fa79b90584de2074eb9c95441f0e` |
| `cmp_core/shaders/bc6_common_encoder.h` | `26b3159139af05efb1847e9217a52dedbf7bd0f3b16f743e21565e6be97dce46` |
| `cmp_core/shaders/bc6_encode_kernel.cpp` | `a509fe57b014e52034c52d4992d2d128de53cd53b9b8268058d293e9b0a754d6` |
| `cmp_core/shaders/bc6_encode_kernel.h` | `5bbe62539f70bba30e56c575dd0787caf113d84e8c6465997acf578c41d5f103` |
| `cmp_core/shaders/bc7_cmpmsc.h` | `782b6c684aa1da9f359c0ef3f65c3f74d2939179b2baae86518e361edec23c83` |
| `cmp_core/shaders/bc7_common_encoder.h` | `36d1ff6373f85c4275d56b495843922c3498cd79c807e8771bcfa2be0fa93681` |
| `cmp_core/shaders/bc7_encode_kernel.cpp` | `13951b1a6fbe8cbf1aade70431931e2fec3f4e9da9ad63868c453fd5ec763879` |
| `cmp_core/shaders/bc7_encode_kernel.h` | `a405c12e8112dd71d42294c6ae8cf8f2740b9fa712e717cbcec214b90532043c` |
| `cmp_core/shaders/bcn_common_api.h` | `263f122e8e8852ef70cad4b7b8c1e4308a622385dadc5c26aa854d5780cbf8bc` |
| `cmp_core/shaders/bcn_common_kernel.h` | `df7309cb51d5d24dae4338f64f63b48e11a545fe025712e1b2db7b9daf09a224` |
| `cmp_core/shaders/common_def.h` | `0dabb4880d9c2bf42a4fd617774fade505a9ef286f74354d53f5fa913b241428` |
| `cmp_core/source/cmp_core.cpp` | `b772293a820ec07ad792bb617094f621774f675401893b7d91107f8659d7e199` |
| `cmp_core/source/cmp_core.h` | `0610fd531315b14df5af0cab6732f27776d2a2e6a183306a5083d9064ec1a88c` |
| `cmp_core/source/cmp_math_func.h` | `4a47c7575d3ee38a6192189c4f4da3f236ff716725c9c882bf07307a754e6dd0` |
| `cmp_core/source/cmp_math_vec4.h` | `62481f35f597e426ce6db0873474a8fb8b5078426d0bba4d0e3b446ea42e35b4` |
| `cmp_core/source/core_simd.h` | `83fa82fb21828367ff62a7544b6f67a40846d434c9a8a7f3f12226c94b27fa24` |
| `cmp_core/source/core_simd_avx.cpp` | `9d44f0d6f22bb84471c99d46e59ace79a5f2549352a39d3e5567b99a09ee4f6b` |
| `cmp_core/source/core_simd_avx512.cpp` | `869fcee6097aa7326d65612bc372b33a09052af4549b9fa6d9d7a16a7a60c68b` |
| `cmp_core/source/core_simd_sse.cpp` | `7c04e918c752d8962d455706a981dc314fe08febd95390c3859191fe501e97d5` |

Added: `LICENSE` (upstream's `license/license.txt`, verbatim; SHA-256
`a408ebd610b0e0390aaa4be2139808e84f1968a54cd8ef40de8a3bed43b38906`), `premake5.lua`, this file.

Removed (everything else of the repository): the CMake and Visual Studio build files (`cmp_core/CMakeLists.txt`,
`cmp_core/cmp_core.def`), the GPU shader sources of the codecs (`cmp_core/shaders/*.hlsl`) and their scripts
(`cmp.bat`, `compilebcn_shaders.cmd`, `compilefx_shaders.cmd`, `copyfiles.bat`), the FidelityFX and Brotli-G sources
(`ffx_*.h`, `cmp_fsr1.h`, `cas_main.hlsl`, `fsr_pass.hlsl`, `brlg_encode_kernel.*`, off upstream too), the other
`cmp_math` files (the Tootle and JML math, never linked by CMP_Core), and the applications, framework, SDK, GUI, tests,
documentation and third-party licences of the other Compressonator components.

Local modifications: **none**.

## What it is for

The BC1, BC4, BC5, BC6H and BC7 block codecs of texture compression (M23: R-TEXC-BC and R-TEXC-HDR,
`Docs/Decisions/0034-m23-contract.md` decisions 13 to 15; BC2 and BC3 are compiled with the library but never used, because
BC3 failed the determinism check below). Only one first-party translation unit includes the header:
`Engine/Source/Engine/Asset/TextureCompression.cpp` (`Scripts/ModuleRules.json` restricts it), whose encoders the
editor's texture and environment importers use and whose decoders the GpuResourceCache uses on a device without BC
support, in the editor and in exported games alike.

## Build configuration (`premake5.lua`)

- Files: upstream's `CMP_Core` sources (`cmp_core/CMakeLists.txt`: the BC1 to BC7 kernels, `cmp_core.cpp` and the two
  `cmp_math` sources) plus the sources of the three SIMD libraries `CMP_Core_SSE`, `CMP_Core_AVX` and `CMP_Core_AVX512`
  that upstream links into it, on x86-64 only. Include directories `cmp_core/shaders`, `cmp_core/source` and
  `applications/_libs/cmp_math`, as upstream's.
- Defines: none on Windows; `_LINUX` and `ASPM_GPU` on Linux and macOS, as upstream's CMake sets them on UNIX (the
  portability stage only generates those builds; they are never compiled, ADR 0028 decision 10).
- Instruction sets: the SIMD sources get upstream's per-library options: `core_simd_avx.cpp` `/arch:AVX2`,
  `core_simd_avx512.cpp` `/arch:AVX512` (upstream writes `/arch:AVX-512`; MSVC's option is spelled `/arch:AVX512`);
  for GCC and Clang `-msse4.2`, `-mavx2 -mfma` and `-mavx512f -mavx512cd` (upstream's `-march=nehalem`, `-march=haswell`
  and `-march=knl`, whose instruction sets these name; GCC 15 removed `-march=knl`). The engine never runs those paths
  (below), but the BC1 kernel references their functions, so they are compiled as upstream compiles them. clang-cl (the
  portability stage's `vs2026` build with toolset `clang`) takes the AVX options in MSVC's spelling and `-msse4.2` for
  every source: under `_M_X64`, which clang-cl defines as MSVC does, the kernels' shared `cmp_floor`
  (`cmp_core/shaders/bcn_common_api.h`) uses the SSE4.1 intrinsic `_mm_floor_ss`, and `core_simd_sse.cpp` uses
  `_mm_blendv_ps` and `_mm_floor_ps`; MSVC compiles SSE4.1 intrinsics without an option, but Clang refuses them in a
  function without the `sse4.1` target feature, as for Jolt's (`UseJoltPhysics`). GCC and Clang outside Visual Studio
  keep upstream's per-file options (never compiled, ADR 0028 decision 10). The engine needs SSE4.2 already (Jolt's
  `/arch:SSE4.2`).
- **Floating point: precise, without contraction**, like JoltPhysics, MikkTSpace and every first-party project
  (Architecture §2.2): MSVC `/fp:precise` (premake `floatingpoint "Default"`); GCC and Clang `-fno-fast-math
  -ffp-contract=off`; clang-cl the same through `/clang:`. Compressed blocks are cooked into the asset cache, whose keys
  do not include the build configuration (§7.5), so a Debug and a Release editor must produce identical bytes.
  `Scripts/CheckBuildConfig.py` checks the model (`PRECISE_VENDOR_PROJECTS`), and its fixture
  `Tests/Data/BuildConfig/FastMathCMPCore` proves the check.
- Debug: `optimize "Size"` (`/O1`), `runtimechecks "Off"`, `symbols "on"`, with the Debug runtime; Release:
  `optimize "on"` (`/Ox`: premake writes `Optimization` `Full` for it in Visual Studio projects), `symbols "on"`; Dist:
  `optimize "full"` (`/Ox` too, with `NDEBUG`, which compiles out the BC6H and BC7 kernels' `assert`s), `symbols "off"`.
  Debug is optimized since M23's W23.2 integration (Docs/Decisions/0034-m23-contract.md decision 21): unoptimized, the
  BC6H encoder made an environment import about six times slower ("EnvironmentImporter: bakes and cooks the Studio HDRI
  deterministically", two imports of the built-in Studio HDRI, took 51.2 s in Debug unoptimized, 8.4 s at `/Ox` and
  8.6 s at `/O1`, about 4.3 s an import, the bake unchanged). MSVC refuses `/RTC1`, which a project with the Debug
  runtime gets by default, together with optimization, so premake writes `BasicRuntimeChecks` `Default` (no run-time
  checks).
- **Why the configurations compile it differently** (W23.2's review, the lead's decision): the precise floating-point
  model makes the optimization level change no result of correct code, but a read of uninitialized memory, like the one
  that keeps BC3 out (above; decision 13), leaves different stack contents under different code generation. Debug at
  `/O1` with the Debug runtime and Release at `/Ox` therefore stay different builds of the library, so the determinism
  check, which compares the cooked bytes of the Debug and the Release editor (the configurations that cook), can still
  expose such a read; at the integration's `optimize "on"` Debug compiled exactly like Release and could not.
- Warnings: the compiler's default level; the library compiles without warnings under MSVC 14.51 (the M23 contract's
  Debug and Release builds, which fail on any MSBuild warning).

## Determinism and process-level state

- **Scalar paths only.** CMP_Core picks the BC1 endpoint search's SIMD variant from the CPU on the first BC1 block,
  unless told otherwise, and writes that choice, and the BC7 codec's ramp tables on their first use, into
  process-level variables without synchronization. `InitializeTextureCodecs` (`Asset/TextureCompression.h`), the
  ProcessContext's `TextureCodecs` step, calls `DisableSIMD()` and builds the BC7 tables once, before any import worker
  exists, so every compression runs the scalar paths and no two threads write those variables. Architecture §3 rule 5
  lists that state.
- **The check.** "TextureCompression: the fixture set compresses to the committed bytes in every configuration"
  (`Tests/Source/Engine/Asset/TextureCompressionTests.cpp`) compresses the committed PNG textures and procedural LDR and
  HDR levels with every codec the engine uses and compares the XXH64 of the blocks with one committed digest, in Debug
  and in Release. It passed in both at the contract (decision 13), with Debug unoptimized, which is what adopting
  CMP_Core waited for (ADR 0028 decision 7, plan §6 "M23"), and passes with Debug at `/O1` since W23.2's review. Dist
  builds no test executable, and no Dist program encodes (the Runtime only decodes, through the fallback), so Debug and
  Release are the configurations whose bytes must agree. Bytes are never compared across compilers.
- **Thread safety.** After that step every codec call builds its own options (`CreateOptionsBC*`), so concurrent calls
  share no mutable state.

## Consumer usage

- `UseCMPCore()` in `Dependencies.lua`: `externalincludedirs { Vendor/CMP_Core/cmp_core/source }` and, for linking
  projects, `links { "CMP_Core" }`. No defines; `Scripts/CheckBuildConfig.py` compares the `CMP_` family (its
  `VENDOR_ABI_FAMILIES` entry) and `NDEBUG`, and `Scripts/Lint.py` bans defining a `CMP_` macro in first-party source
  (`banned-abi-macro`): `CMP_CDECL` and `CMP_DEFAULTNULL` select the calling convention and the default arguments.
- Include as `#include <cmp_core.h>`; the API returns 0 on success (`CGU_CORE_OK` of `common_def.h`, which the public
  header does not include).
- Sources: RGBA8 rows for BC1, BC2, BC3 and BC7; one 8-bit channel for BC4 and two for BC5; three binary16 channels a
  texel (RGB, stride in shorts) for BC6H. Decoders write 4 x 4 blocks in the same forms.

## Verification performed

- Partial clone of `https://github.com/GPUOpen-Tools/compressonator.git` at tag `V4.6.0` (sparse checkout of
  `cmp_core`, `applications/_libs/cmp_math` and `license`, `core.autocrlf=false`); the file hashes above were computed on
  the checked-out files, and `git ls-tree` showed the three trees identical at `V4.5.52`.
- The M23 contract builds the library in Debug and Release with MSVC 14.51 without warnings, and in Release and Dist with
  clang-cl in the portability workspace (`Generate.py --toolset clang`; vendored code's warnings tolerated, as the
  portability stage tolerates them); `Scripts/CheckBuildConfig.py` passes on the workspace and fails on
  `FastMathCMPCore`; the determinism check above passes in Debug and Release. W23.2's review built it again in Debug
  (`/O1`), Release and Dist with MSVC 14.51 and in Release and Dist with clang-cl, and the determinism check passed in
  Debug and Release.

## Updating

1. `git ls-remote --tags https://github.com/GPUOpen-Tools/compressonator.git`
2. Clone the new tag as above and compare the three trees; replace the kept files, add any new source that upstream's
   `cmp_core/CMakeLists.txt` lists, and update `LICENSE` if the notice changed.
3. Update this file (version, commit, date, hashes), rebuild every configuration, run `python Scripts/CheckBuildConfig.py`
   and the TextureCompression tests in Debug and Release. Keep the configurations' optimization levels different (Debug
   `/O1`, Release `/Ox`, above): the determinism check compares the two builds' bytes, so a read of uninitialized memory
   in a new pin, or in a codec the engine starts using, shows as a digest that differs between them. A codec change
   changes the committed digest: update it only together with `TextureImporter::Version` and
   `EnvironmentImporter::Version`, so cached textures are rebuilt.

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).

| Field | Value |
|---|---|
| Name | CMP_Core (Compressonator) |
| Upstream | https://github.com/GPUOpen-Tools/compressonator |
| Version | V4.6.0 095a143ad8cfbf9ed5afa4ea63c735bfd0fd04dd |
| License | MIT |
| Shipped | yes |
| Projects | CMP_Core |
| License files | LICENSE |
