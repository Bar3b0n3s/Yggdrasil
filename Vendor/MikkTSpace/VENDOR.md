# MikkTSpace

| | |
|---|---|
| Upstream | https://github.com/mmikk/MikkTSpace |
| Version | No release tags exist upstream; pinned to `master` |
| Commit | `3e895b49d05ea07e4c2133156cfa94369e19e409` (master, 2020-03-25, "add a url with more info") |
| Date vendored | 2026-10-07 (M6 contract; approved in `Docs/Decisions/0001-approvals.md`) |
| License | zlib (SPDX: `Zlib`). The notice is at the top of both source files; `LICENSE` reproduces it verbatim. |
| Kind | One C file and its header; static library, premake project `MikkTSpace` (`premake5.lua`), language C |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings as stored upstream):

| File | SHA-256 |
|---|---|
| `mikktspace.c` | `de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288` |
| `mikktspace.h` | `17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0` |

Added: `LICENSE` (the zlib notice of the source files' header comment, verbatim; SHA-256
`9ae731b25215baa2b2d6047bf537fb2312f2003a6a05d3d4552010455b1e9fb0`), `premake5.lua`, this file.

Removed: `README.md` (two lines pointing at http://www.mikktspace.com/).

Local modifications: **none**.

## What it is for

The glTF importer generates tangents with it for primitives that have normals and `TEXCOORD_0` but no `TANGENT`
(Architecture §7.4; the approved alternative to the Lengyel fallback of Appendix C). MikkTSpace is the tangent space the
glTF specification and normal-map bakers assume, so normal maps baked against it shade correctly. Only one first-party
translation unit includes the header: `Engine/Source/Engine/AssetPipeline/Private/TangentGenerator.cpp`
(`Scripts/ModuleRules.json` restricts it), and the Runtime never links any code that calls it (AssetPipeline is
editor-only, Architecture §14.4).

## Build configuration (`premake5.lua`)

- Files: `mikktspace.c`, `mikktspace.h`; include directory `.`.
- C dialect: compiler default; no defines.
- **Floating point: precise, without contraction**, like JoltPhysics and every first-party project (Architecture §2.2):
  MSVC `/fp:precise` (premake `floatingpoint "Default"`); GCC and Clang `-fno-fast-math -ffp-contract=off`; clang-cl the
  same through `/clang:`. Generated tangents are cooked into meshes, and cache keys do not include the build
  configuration (§7.5), so a Debug and a Release editor must produce identical bytes; FMA contraction that depends on
  the optimization level would break that. `Scripts/CheckBuildConfig.py` checks the model (`PRECISE_VENDOR_PROJECTS`)
  and its fixture `Tests/Data/BuildConfig/FastMathMikkTSpace` proves the check.
- Debug: `symbols "on"`; Release: `optimize "on"`, `symbols "on"`; Dist: `optimize "full"`, `symbols "off"`.
- Warnings: the compiler's default level; the file compiles without warnings under MSVC 14.51 (verified by the Debug,
  Release and Dist builds of the M6 contract, which fail on any MSBuild warning).

The C runtime calls it makes are `malloc`/`free`, `sqrtf`, `fabsf` and `acosf` (the angle weights). `acosf` is a CRT
transcendental function: importers are not on the simulation path (Architecture §4.12), and their output is compared
within one toolchain and C runtime ("Importers: importing twice gives identical hashes"); cross-C-runtime equality of
cooked meshes is not required (§1.2: no bit-exact cross-OS or cross-compiler determinism).

`assert` is active in Debug and Release (premake does not define `NDEBUG` outside Dist, Architecture §2.2) and checks
only MikkTSpace's internal invariants.

## Consumer usage

- `UseMikkTSpace()` in `Dependencies.lua`: `externalincludedirs { Vendor/MikkTSpace }` and, for linking projects,
  `links { "MikkTSpace" }`. No defines (`Scripts/CheckBuildConfig.py` compares only `NDEBUG` for it).
- Include as `#include <mikktspace.h>` (the header has its own `extern "C"` guard).
- Call `genTangSpaceDefault(const SMikkTSpaceContext*)` with an interface that reads positions, normals and texture
  coordinates of a triangle list and receives one tangent and sign per corner (`m_setTSpaceBasic`). The function
  returns 0 only when an allocation fails.

## Verification performed

- Cloned `https://github.com/mmikk/MikkTSpace.git` and checked out the commit above; the file hashes above were computed
  on the checked-out files.
- The M6 contract builds the library in Debug, Release and Dist with MSVC 14.51 without warnings;
  `Scripts/CheckBuildConfig.py` passes on the workspace and fails on `FastMathMikkTSpace`.
- Stream C's tests of the tangent generator (`Tests/Source/Engine/AssetPipeline/Importers/GltfImporterTests.cpp`) check
  generated tangents against the normal-mapped fixtures with file tangents.

## Updating

1. `git ls-remote https://github.com/mmikk/MikkTSpace.git refs/heads/master`
2. `git clone -c core.autocrlf=false https://github.com/mmikk/MikkTSpace.git <tmp>` and check out the new commit.
3. Replace `mikktspace.c` and `mikktspace.h`; update `LICENSE` if the notice changed.
4. Update this file (commit, date, hashes), rebuild every configuration, run `python Scripts/CheckBuildConfig.py` and the
   glTF importer tests. Bump `GltfImporter::Version` when generated tangents change, so cached meshes are rebuilt.
