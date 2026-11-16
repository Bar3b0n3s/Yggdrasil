# Recast

| | |
|---|---|
| Upstream | https://github.com/recastnavigation/recastnavigation |
| Version | `v1.6.0` (an annotated tag, object `b4554541b658630816dba41466eb1cefb624519e`; the latest release when this was vendored) |
| Commit | `6dc1667f580357e8a2154c28b7867bea7e8ad3a7` (commit dated 2023-05-21) |
| Date vendored | 2026-11-15 (M30 W30.1; approved in `Docs/Decisions/0001-approvals.md` (ADR 0042, "The owner's answers" 3), pinned by `Docs/Decisions/0045-m30-navigation.md` decision 2) |
| License | zlib (SPDX: `Zlib`), Mikko Mononen, see `License.txt` (upstream's `License.txt`, the same file in `Vendor/Recast/` and `Vendor/Detour/`) |
| Kind | C++ static library, premake project `Recast` (`premake5.lua`), the `Recast/` directory of the recastnavigation repository |
| Release archive | `https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz`, 1,280,501 bytes, SHA-256 `d48ca0121962fa0639502c0f56c4e3ae72f98e55d88727225444f500775c0074` (verified on the downloaded bytes) |

## Contents

Kept, byte-identical to the release archive (line endings as stored upstream: LF, but CRLF in `License.txt`, which
`.gitattributes` keeps byte for byte; each file's git blob hash also equals the tag's), at their upstream paths below `Recast/`:

| File | SHA-256 |
|---|---|
| `Include/Recast.h` | `dea734c3ae3b0f436c7734ea2c911e2f5d9e333243d541204b9181d054cf90d2` |
| `Include/RecastAlloc.h` | `df5c71e121903ed6ab38db8efdf7690f7a97e50aa103ab205a85e44c4d90c813` |
| `Include/RecastAssert.h` | `9d1b781803215b276f7e044fa7ff4012fa0b27cdbd2a9d0ad70fe32bb503f285` |
| `Source/Recast.cpp` | `865595763451cde0aad5b99088bfab48d4ea35050fefd67f3d4f95885ba38d66` |
| `Source/RecastAlloc.cpp` | `e77600ee0a1f4b36e9e0700480fcfccf8a62e31eb9e16aa7cb8eab5e93511a2e` |
| `Source/RecastArea.cpp` | `6e60b6eaf7b43f22c268315f55489c30c5061f3d4b5a875ff43fb763dfa0cbca` |
| `Source/RecastAssert.cpp` | `25e35567a16d98950aa887c9e361d6f82f56dc6c9486049ace80662bc2d09392` |
| `Source/RecastContour.cpp` | `f5ad218147949dccc71885dfd1bc4bf2e303a35571b6da37d3696f8460220a39` |
| `Source/RecastFilter.cpp` | `c75ef35233dbd5d7db768f9375e9f648c51c3832c14f97bf47cb3cb4905cc51e` |
| `Source/RecastLayers.cpp` | `f01d325fd096feee2971f71f9b03b1d37a6d0a4777bb9d529ce698253b484820` |
| `Source/RecastMesh.cpp` | `c1dab1383688cf37398abad47e6897a29e17ae2a00b1d20dc8f95126b1a8725a` |
| `Source/RecastMeshDetail.cpp` | `b1fe99540334134a6307ab3c013dcc5c6b5f9ed41309137b7a8635ebcea1ed20` |
| `Source/RecastRasterization.cpp` | `d01d16045c0b0c9fca6201b66574ccbcbc31ad071ea2360f36fdb5d12a074f2e` |
| `Source/RecastRegion.cpp` | `430f1a1d91317b1e28020272a8bd48ac2f5c031a5d38f08deafbf53ea0e74e42` |
| `License.txt` | `ffad87821cce7cfd3be00b63ffadd01b04e83a4c6a0bcc68f06362ffd42d705e` |

Added: `premake5.lua` and this file.

Removed (everything else of the archive): `Recast/CMakeLists.txt` (its file list is the glob `Source/*.cpp`, mirrored in
`premake5.lua`); outside `Recast/`, Detour (vendored separately in `Vendor/Detour/`), `DetourCrowd/`, `DetourTileCache/`,
`DebugUtils/`, `RecastDemo/`, `Tests/`, `Docs/`, the CMake and pkg-config files, `Doxyfile` and the repository's documents.

Local modifications: **none**.

## What it is for

The editor's navigation bake (M30, `Docs/Decisions/0045-m30-navigation.md`): `navigation.bake` and the editor's Bake
Navigation action voxelize a scene's static collision geometry (the bake input, `Scene/NavigationBakeInput.h`) and build
each agent type's polygon and detail meshes, which the NavMesh asset stores as a first-party payload
(`Asset/NavMeshData.h`). Only one first-party translation unit includes its headers:
`Editor/Source/EditorCore/Navigation/NavMeshBuilder.cpp` (`Scripts/ModuleRules.json`). EditorCore exists only in the
editor and the Tests, so no exported game links Recast.

The builder calls Recast's steps one by one (`rcCreateHeightfield`, `rcRasterizeTriangles`, the span filters,
`rcBuildCompactHeightfield`, `rcErodeWalkableArea`, `rcMarkConvexPolyArea`, `rcBuildDistanceField`, `rcBuildRegions`,
`rcBuildContours`, `rcBuildPolyMesh`, `rcBuildPolyMeshDetail`) and never `rcMarkWalkableTriangles` or
`rcClearUnwalkableTriangles`, whose slope test calls `cosf`: the walkable slope is marked first-party with `DetMath`
(decision 7), so a bake never depends on the C runtime's transcendental functions.

### Audit of the C runtime's mathematical functions (ADR 0042 decision 3)

Recast's sources call `sqrtf` (`Recast.cpp`'s `rcSqrt`, `RecastMeshDetail.cpp`), `floorf`, `ceilf` and `fabsf`, which IEEE
754 rounds exactly in every configuration, and `cosf` in exactly two functions, `rcMarkWalkableTriangles` and
`rcClearUnwalkableTriangles` (`Recast.cpp`), which the builder never calls. No other transcendental function (`sinf`,
`tanf`, `atan2f`, `acosf`, `expf`, `logf`, `powf`) and no random number appears in `Include/` or `Source/`.

## Build configuration (`premake5.lua`)

- Files: `Source/*.cpp` (with `Include/*.h` listed for the IDE); include directory `Include`. C++17, like the other vendored
  C++ libraries.
- Defines: none. Recast's headers read no configuration macro; `RC_MAX_LAYERS_DEF` and `RC_MAX_NEIS_DEF`, which only
  `RecastLayers.cpp` reads (the heightfield layers, which the bake does not use), keep their defaults. `NDEBUG` follows the
  workspace (Dist only), where `rcAssert` compiles to nothing; the editor has no Dist build, so its asserts are always
  compiled.
- **Floating point: precise, without contraction**, like every first-party project (Architecture §2.2): MSVC
  `/fp:precise` (premake `floatingpoint "Default"`); GCC and Clang `-fno-fast-math -ffp-contract=off`; clang-cl the same
  through `/clang:`. Two bakes of one scene must write byte-identical files from the Debug and Release editors.
  `Scripts/CheckBuildConfig.py` checks the model (`PRECISE_VENDOR_PROJECTS`), and its fixture
  `Tests/Data/BuildConfig/FastMathRecast` proves the check.
- Debug: `symbols "on"`; Release: `optimize "on"`, `symbols "on"`; Dist: `optimize "full"`, `symbols "off"`.
- Warnings: the compiler's default level; the library compiles without warnings under MSVC 14.51.

## Consumer usage

- `UseRecast()` in `Dependencies.lua`: `externalincludedirs { Vendor/Recast/Include }` and, for linking projects,
  `links { "Recast" }`. EditorCore, Editor and Tests call it, never the Runtime's projects (`UseRuntimeDependencies`).
  `Scripts/CheckBuildConfig.py` compares the `RC_` family (its `VENDOR_ABI_FAMILIES` entry, empty for the library and every
  consumer) and `NDEBUG`, and `Scripts/Lint.py` bans defining an `RC_` macro in first-party source (`banned-abi-macro`).
- Include as `#include <Recast.h>` (and `RecastAlloc.h`, `RecastAssert.h`).
- Process-level hooks (Architecture §3 rule 5; decision 2): `InstallNavMeshBuildHooks`
  (`EditorCore/Navigation/NavMeshBuilder.h`) sets `rcAllocSetCustom` (the C runtime's heap, counted, so the removal
  verifies that no allocation outlived a bake) and `rcAssertFailSetCustom` (the engine's assert handler); the editor and
  the Tests main hand it to the ProcessContext's Navigation step (`ProcessLibraryHooks`), so the Runtime, which never
  links Recast, sets nothing.
- Every Recast object the builder allocates (`rcHeightfield`, `rcCompactHeightfield`, `rcContourSet`, `rcPolyMesh`,
  `rcPolyMeshDetail`) is released through its free function (`rcFreeHeightField` and the others) by an RAII guard.

## Verification performed

- Downloaded the release archive above, recorded its size and SHA-256, and compared every kept file's git blob hash with
  the tag's tree (`git ls-tree` of `v1.6.0` in a fresh clone): all equal; every file LF but `License.txt` (CRLF).
- Built in Debug and Release with MSVC 14.51 (`Build.py`, which fails on any MSBuild warning); `Scripts/CheckBuildConfig.py`
  passes on the workspace and fails on `FastMathRecast`.
- The NavMeshBuilder cases (`Tests/Source/EditorCore/Navigation/NavMeshBuilderTests.cpp`): the hooks, and the bakes of the
  test arenas.

## Updating

1. `git ls-remote --tags https://github.com/recastnavigation/recastnavigation.git`
2. Download the new tag's archive, verify it, and replace `Include/`, `Source/` and `License.txt` here and in
   `Vendor/Detour/` (one release for both); check the audit above again (a new `cosf` caller, a random number).
3. Update this file (version, commit, date, hashes), rebuild every configuration, run `python Scripts/CheckBuildConfig.py`
   and the NavMeshBuilder and navigation.bake cases. A change in what Recast builds changes every committed `.navmesh`
   file's bytes: raise `NavMeshData::FormatVersion` only if the payload's layout changes; otherwise bake the committed scenes
   again (FeatureTest through `Scaffold.py`).

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).
Shipped is `no`: neither the generated Runtime project nor the Engine project references this directory (only EditorCore,
Editor and Tests call `UseRecast()`), so no exported game contains Recast's code.

| Field | Value |
|---|---|
| Name | Recast |
| Upstream | https://github.com/recastnavigation/recastnavigation |
| Version | v1.6.0 6dc1667f580357e8a2154c28b7867bea7e8ad3a7 |
| License | Zlib |
| Shipped | no |
| Projects | - |
| License files | License.txt |
