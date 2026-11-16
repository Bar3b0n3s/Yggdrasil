# Detour

| | |
|---|---|
| Upstream | https://github.com/recastnavigation/recastnavigation |
| Version | `v1.6.0` (an annotated tag, object `b4554541b658630816dba41466eb1cefb624519e`; the latest release when this was vendored) |
| Commit | `6dc1667f580357e8a2154c28b7867bea7e8ad3a7` (commit dated 2023-05-21) |
| Date vendored | 2026-11-15 (M30 W30.1; approved in `Docs/Decisions/0001-approvals.md` (ADR 0042, "The owner's answers" 3), pinned by `Docs/Decisions/0045-m30-navigation.md` decision 2) |
| License | zlib (SPDX: `Zlib`), Mikko Mononen, see `License.txt` (upstream's `License.txt`, the same file in `Vendor/Recast/` and `Vendor/Detour/`) |
| Kind | C++ static library, premake project `Detour` (`premake5.lua`), the `Detour/` directory of the recastnavigation repository |
| Release archive | `https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz`, 1,280,501 bytes, SHA-256 `d48ca0121962fa0639502c0f56c4e3ae72f98e55d88727225444f500775c0074` (verified on the downloaded bytes) |

## Contents

Kept, byte-identical to the release archive (line endings as stored upstream: LF, but CRLF in `License.txt`, which
`.gitattributes` keeps byte for byte; each file's git blob hash also equals the tag's), at their upstream paths below `Detour/`:

| File | SHA-256 |
|---|---|
| `Include/DetourAlloc.h` | `5f617c7f162069c395d7edfc59f0e06a9b6afaf6698de8fd674c80f00056ab71` |
| `Include/DetourAssert.h` | `5c9ba991780904dba813c43d2f21b021b3149c460385c5aedc27a480b61de311` |
| `Include/DetourCommon.h` | `24d69468f73782718855b3691ae348e50437febc7594635b37a940f1c0c8db46` |
| `Include/DetourMath.h` | `f8c4e9d51a526ec5c38c01e48a035e50c6acaa1d0dd69ffa66209cb092d4cac7` |
| `Include/DetourNavMesh.h` | `09e9bf590eea65048eb4646f5561023e2894d53cb774a869f175d17e96a735cb` |
| `Include/DetourNavMeshBuilder.h` | `39fde320de4931e85949215ca9395e1027980572afe4962dfbc7d02804131798` |
| `Include/DetourNavMeshQuery.h` | `7fec77b2d0f0ed8d588fb271a01da41943d4cd9b33d0809e761083950e2d625d` |
| `Include/DetourNode.h` | `7c295c61f09dff0de42bdcc873a5d698d687bb2052aca8b293425c0ef0444a9e` |
| `Include/DetourStatus.h` | `7255e33173c4bdb26f4224514d098fdc5c31e19cd80263a4e56475627602ade9` |
| `Source/DetourAlloc.cpp` | `ddf4d9857e9e1714daa9dd9b687ac8139085cc602359db0dba4f1dedbedc69c1` |
| `Source/DetourAssert.cpp` | `d7e05a59d390b02c3d7b2c3bbdf5c243f11ea36962243600c5acb26cdb80f796` |
| `Source/DetourCommon.cpp` | `c28eb2d5fd251e1b2e24a7c03a679ff82684e51a0ce95db3cf457fda60362008` |
| `Source/DetourNavMesh.cpp` | `e82c9a13cf01bea8ef0c6bcc50cef41f7604ce7343a70ef04a8bdc48be21a26a` |
| `Source/DetourNavMeshBuilder.cpp` | `bbe5adf461cae282291e498db50550eb3a12706e352dd83021165d38950119f3` |
| `Source/DetourNavMeshQuery.cpp` | `fb3f040b35633ad01dfec2a18e1dd1433b10ede69780dddf65bd0a016af0183b` |
| `Source/DetourNode.cpp` | `067ffb9a81f7315f2253ff217263acb1c01590d130e01d4de4500308d756132b` |
| `License.txt` | `ffad87821cce7cfd3be00b63ffadd01b04e83a4c6a0bcc68f06362ffd42d705e` |

Added: `premake5.lua` and this file.

Removed (everything else of the archive): `Detour/CMakeLists.txt` (its file list is the glob `Source/*.cpp`, mirrored in
`premake5.lua`); outside `Detour/`, Recast (vendored separately in `Vendor/Recast/`), `DetourCrowd/` (crowds and local
avoidance are out of scope, ADR 0042 decision 1), `DetourTileCache/` (no run-time rebakes), `DebugUtils/`, `RecastDemo/`,
`Tests/`, `Docs/`, the CMake and pkg-config files, `Doxyfile` and the repository's documents.

Local modifications: **none**.

## What it is for

The run-time navigation mesh and its queries (M30, `Docs/Decisions/0045-m30-navigation.md`): the Navigation module
(`Engine/Source/Engine/Navigation/`) builds Detour's mesh from a NavMesh asset's validated first-party payload
(`Asset/NavMeshData.h`) with `dtCreateNavMeshData`, never from bytes it trusts, and answers the `Navigation` script
namespace's path, closest-point and surface-ray queries in the fixed step. Only the Navigation module's private files
include its headers (`Scripts/ModuleRules.json`); every executable links it, the Runtime included.

### Audit of the C runtime's mathematical functions (ADR 0042 decision 3)

Detour's sources call `sqrtf`, `floorf`, `ceilf` and `fabsf` (`DetourMath.h`'s `dtMathSqrtf`, `dtMathFloorf`,
`dtMathCeilf`, `dtMathFabsf`), which IEEE 754 rounds exactly in every configuration. `DetourMath.h` also defines
`dtMathCosf`, `dtMathSinf` and `dtMathAtan2f`, which no file of `Detour/` calls (DetourCrowd, not vendored, does). The only
randomness is the `frand` callback of `dtNavMeshQuery::findRandomPoint` and `findRandomPointAroundCircle`, a function
pointer without user data that a seeded generator could reach only through a global: the engine never calls either, and
picks a random point first-party from a seeded generator (decision 10).

## Build configuration (`premake5.lua`)

- Files: `Source/*.cpp` (with `Include/*.h` listed for the IDE); include directory `Include`. C++17, like the other vendored
  C++ libraries.
- Defines: none. `DT_POLYREF64` (64-bit polygon references) and `DT_VIRTUAL_QUERYFILTER` (a virtual `dtQueryFilter`) change
  the layout of `dtPolyRef`, `dtNode` and `dtQueryFilter`, so they are left undefined for the library and every consumer:
  one tile per agent type needs no more than 32-bit references. `NDEBUG` follows the workspace (Dist only), where
  `dtAssert` compiles to nothing and `dtAssertFailSetCustom` is not declared.
- **Floating point: precise, without contraction**, like every first-party project (Architecture §2.2): MSVC
  `/fp:precise` (premake `floatingpoint "Default"`); GCC and Clang `-fno-fast-math -ffp-contract=off`; clang-cl the same
  through `/clang:`. Queries run on the simulation path, so a path must be identical in Debug, Release and Dist (§9.1).
  `Scripts/CheckBuildConfig.py` checks the model (`PRECISE_VENDOR_PROJECTS`), and its fixture
  `Tests/Data/BuildConfig/FastMathDetour` proves the check.
- Debug: `symbols "on"`; Release: `optimize "on"`, `symbols "on"`; Dist: `optimize "full"`, `symbols "off"`.
- Warnings: the compiler's default level; the library compiles without warnings under MSVC 14.51.

## Consumer usage

- `UseDetour()` in `Dependencies.lua`: `externalincludedirs { Vendor/Detour/Include }` and, for linking projects,
  `links { "Detour" }`. Engine, EditorCore, Editor and Tests call it, and the Runtime's projects through
  `UseRuntimeDependencies()`. `Scripts/CheckBuildConfig.py` compares the `DT_` family (its `VENDOR_ABI_FAMILIES` entry,
  empty for the library and every consumer) and `NDEBUG`, and `Scripts/Lint.py` bans defining a `DT_` macro in first-party
  source (`banned-abi-macro`).
- Include as `#include <DetourNavMesh.h>` (and the other headers of `Include/`).
- Process-level hooks (Architecture §3 rule 5; decision 2): the ProcessContext's Navigation step calls
  `InitializeNavigationRuntime` (`Navigation/NavigationRuntime.h`), which sets `dtAllocSetCustom` (the C runtime's heap,
  counted, so the step's teardown verifies that no Detour allocation is live) and, outside Dist, `dtAssertFailSetCustom`
  (the engine's assert handler).

## Verification performed

- Downloaded the release archive above, recorded its size and SHA-256, and compared every kept file's git blob hash with
  the tag's tree (`git ls-tree` of `v1.6.0` in a fresh clone): all equal; every file LF but `License.txt` (CRLF).
- Built in Debug and Release with MSVC 14.51 (`Build.py`, which fails on any MSBuild warning); `Scripts/CheckBuildConfig.py`
  passes on the workspace and fails on `FastMathDetour`.
- The NavigationRuntime cases (`Tests/Source/Engine/Navigation/NavigationRuntimeTests.cpp`): the hooks.

## Updating

As `Vendor/Recast/VENDOR.md` (one release for both). A change in Detour's query results changes the committed paths of
FeatureTest's navigation suite and replay: record them again with `Scaffold.py`.

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).

| Field | Value |
|---|---|
| Name | Detour |
| Upstream | https://github.com/recastnavigation/recastnavigation |
| Version | v1.6.0 6dc1667f580357e8a2154c28b7867bea7e8ad3a7 |
| License | Zlib |
| Shipped | yes |
| Projects | Detour |
| License files | License.txt |
