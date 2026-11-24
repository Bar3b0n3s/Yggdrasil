# meshoptimizer

| | |
|---|---|
| Upstream | https://github.com/zeux/meshoptimizer |
| Version | v1.3 (the latest release tag on 2026-11-24) |
| Commit | `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` (tag `v1.3`, 2026-09-25, "Merge pull request #1097 from zeux/nextver") |
| Date vendored | 2026-11-24 (M46 W46.1; approved in `Docs/Decisions/0001-approvals.md`, ADR 0060 decision 5) |
| License | MIT (SPDX: `MIT`). `LICENSE.md` is upstream's, verbatim. |
| Kind | C++ sources and one header; static library, premake project `meshoptimizer` (`premake5.lua`), C++17 |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings as stored upstream): upstream's `src/` directory
whole, and `LICENSE.md`.

| File | SHA-256 |
|---|---|
| `LICENSE.md` | `f03037ca7bad1e3eb7f4a63fa6084a8baabd5ba30d3c239a9a7f35705d873e26` |
| `src/allocator.cpp` | `d2cc48691fe2f4c6d097bf7a766389cfb7f83ca14a5f747e3944332656d02254` |
| `src/clusterizer.cpp` | `a2775edf77d86a9a5f0ca82f4635ce6ce741bbda79fd9a652ffc8c5dd61d3b0e` |
| `src/indexanalyzer.cpp` | `bff37aecb10cefa33f3f6a217413c6ac98899c23e6de5282420c60e6baf786ec` |
| `src/indexcodec.cpp` | `2b98ede67ab4731e8ab54da794c46b735d659ea360666fe66bc5725c027fff6e` |
| `src/indexgenerator.cpp` | `f362cdcd8af10016333bf699347a9364335987f413f061e5b7e8cafe3b498948` |
| `src/meshletcodec.cpp` | `f7bad517cb77a646ba0beef916ac42715c2fe18002f29e12c6cd4ccd5835e678` |
| `src/meshletutils.cpp` | `4813a0c9e8648c6de53f317322fa17c9486ff024f186aa9d631e7c60ba96c33e` |
| `src/meshoptimizer.h` | `e3688ef553f603391747895a5c221ef1528574cc7f8173782417a96cb531f61b` |
| `src/opacitymap.cpp` | `81037b530d10aaac80a43197294aef5b27b3ccf8d1e323c5ccdc0945fc4c5d69` |
| `src/overdrawoptimizer.cpp` | `a70412562f991133ce52b2833adee9de0a9029dc45312ada1eb5c2452dc4f3ee` |
| `src/partition.cpp` | `cf34f00dd7a550c7b18b31c35d683ac2efb9e1d711bcec411649d311477d4b2d` |
| `src/quantization.cpp` | `28282f555d42a118a6735f161b1ba78fcbe0d1810a6a337a399bd8471f79105e` |
| `src/rasterizer.cpp` | `2172e0ac82359a19f2880763e2088f51b7ec361b23fb54f743b618691967dbe5` |
| `src/remesher.cpp` | `51a06f9d348709b99e2f750710cc607793c8a08c5486193d8622ca2a06f67fb2` |
| `src/simplifier.cpp` | `719303b6970398631e30724e8eebc6cd2e7e08b67fe8ab7cbf7eecafdaa08616` |
| `src/spatialorder.cpp` | `9005ee0069ed8c56797606de80c0cd951ab55ffb608bc3af83c4e5b2517371b1` |
| `src/stripifier.cpp` | `e653e6be3a9b821016a1f6a90b756b971422f1246ebd03e60b478251a288bfd4` |
| `src/tangentspace.cpp` | `1ba72a59793887e870e5cc2c31b9b3d38296b436ee2fcdfb2c9e409b480e4079` |
| `src/vcacheoptimizer.cpp` | `618429ef4db8ab9b16fde4e73dcf425d89452acbf103085478bc8b1761fa6689` |
| `src/vertexcodec.cpp` | `6af26a85ed38daffb97d988e9704ffc257c690af05048304044bfc786699550f` |
| `src/vertexfilter.cpp` | `780e7f95024b577b8adcb8ee2a2e442294ebd62a1b6aaf37627fc1ff3cac4ff3` |
| `src/vfetchoptimizer.cpp` | `aa534bb8150ca27ae229c58a0dc51f85f9acccbc5d4214f4c82edd91fc08e478` |

Added: `premake5.lua`, this file.

Removed: `gltf/` (gltfpack), `js/`, `demo/`, `tools/`, `extern/`, `CMakeLists.txt`, `Makefile`, `README.md` and
`CONTRIBUTING.md`, and upstream's dotfiles (`.github/`, `.clang-format`, `.editorconfig`, `.gitignore`).

Local modifications: **none**.

## What it is for

The glTF importer's generated mesh levels (M46, `Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md` decision
3B; Architecture §7.4): with the import setting `Lods` at `Generate`, `meshopt_simplify` reduces each submesh of level 0,
over level 0's positions, to each level's share of its triangles within `LodMaxError` (relative to the mesh's extent),
with `meshopt_SimplifyLockBorder` so seams between submeshes stay closed. A generated level is index-only: it indexes
level 0's vertices. Only one first-party translation unit includes the header:
`Engine/Source/Engine/AssetPipeline/Private/MeshSimplifier.cpp` (`Scripts/ModuleRules.json` restricts it), and the
Runtime never links any code that calls it (AssetPipeline is editor-only, Architecture §14.4).

## Build configuration (`premake5.lua`)

- Files: `src/*.h`, `src/*.cpp`; include directory `src`.
- C++17; no defines (`MESHOPTIMIZER_API`, `MESHOPTIMIZER_EXPERIMENTAL` and `MESHOPTIMIZER_ALLOC_CALLCONV` keep their
  defaults, and `MESHOPTIMIZER_NO_WRAPPERS`, `MESHOPTIMIZER_ALLOC_EXPORT` and `MESHOPTIMIZER_NO_SIMD` stay undefined:
  the SIMD paths are the vertex and meshlet codecs', the vertex filters' and the clusterizer's, which the engine does not
  call).
- **Floating point: precise, without contraction**, like MikkTSpace and every first-party project (Architecture §2.2):
  MSVC `/fp:precise` (premake `floatingpoint "Default"`); GCC and Clang `-fno-fast-math -ffp-contract=off`; clang-cl the
  same through `/clang:`. Generated levels are cooked into meshes, and cache keys do not include the build configuration
  (§7.5), so a Debug and a Release editor must produce identical bytes; FMA contraction that depends on the
  optimization level would break that. `Scripts/CheckBuildConfig.py` checks the model (`PRECISE_VENDOR_PROJECTS`), its
  fixture `Tests/Data/BuildConfig/FastMathMeshOptimizer` proves the check, and the unit case "GltfImporter: generated
  levels share level 0's vertices and match their committed checksum" pins a generated level's bytes in the Debug and
  the Release unit runs.
- Debug: `symbols "on"`; Release: `optimize "on"`, `symbols "on"`; Dist: `optimize "full"`, `symbols "off"`.
- Warnings: the compiler's default level; the sources compile without warnings under MSVC 14.51 (the Debug and
  Release builds of W46.1, which fail on any MSBuild warning).

The simplifier's C runtime calls are `sqrtf` and `fabsf`, which are correctly rounded or exact, and `memset`/`memcpy`;
importers are not on the simulation path (Architecture §4.12), and their output is compared within one toolchain and C
runtime ("Importers: importing twice gives identical hashes"); cross-C-runtime equality of cooked meshes is not required
(§1.2).

`assert` is active in Debug and Release (premake does not define `NDEBUG` outside Dist, Architecture §2.2) and checks
only meshoptimizer's own preconditions, which `MeshSimplifier.cpp` meets before every call (an index count a multiple of
three, indices below the vertex count, a position stride of 12 bytes).

## Consumer usage

- `UseMeshOptimizer()` in `Dependencies.lua`: `externalincludedirs { Vendor/meshoptimizer/src }` and, for linking
  projects, `links { "meshoptimizer" }`. No defines (`Scripts/CheckBuildConfig.py` compares the `MESHOPTIMIZER_` family,
  empty everywhere, and `NDEBUG`). Engine, EditorCore, Editor and Tests call it; `UseRuntimeDependencies()` does not.
- Include as `#include <meshoptimizer.h>` (the header has its own `extern "C"` guard).
- Call `meshopt_simplify(destination, indices, indexCount, positions, vertexCount, 12, targetIndexCount, targetError,
  meshopt_SimplifyLockBorder, &resultError)`; it returns the new index count, at most `indexCount`, and writes that many
  indices into `destination`, which holds `indexCount` entries.

## Verification performed

- `git ls-remote --tags https://github.com/zeux/meshoptimizer.git` named `v1.3` the latest release tag, at the commit
  above; a shallow clone of the tag (`git clone -c core.autocrlf=false --depth 1 --branch v1.3`) gave that commit, and
  the file hashes above were computed on the checked-out files.
- W46.1 builds the library in Debug and Release with MSVC 14.51 without warnings; `Scripts/CheckBuildConfig.py` passes
  on the workspace and fails on `FastMathMeshOptimizer`.
- The glTF importer's unit cases check generated levels (`Tests/Source/Engine/AssetPipeline/Importers/GltfImporterTests.cpp`).

## Updating

1. `git ls-remote --tags https://github.com/zeux/meshoptimizer.git`
2. `git clone -c core.autocrlf=false --depth 1 --branch <tag> https://github.com/zeux/meshoptimizer.git <tmp>`.
3. Replace `src/` and `LICENSE.md`.
4. Update this file (tag, commit, date, hashes), rebuild every configuration, run `python Scripts/CheckBuildConfig.py`
   and the glTF importer tests. Bump `GltfImporter::Version` when generated levels change (their checksum case fails),
   so cached meshes are rebuilt.

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).
Shipped is `yes` by the notices' mechanical rule: the generated Engine project references this directory (its include
path), so the library ships attributed, although no exported game links it (Projects is `-`: no project links it into the
Runtime), as tinyexr's attribution over-approximates.

| Field | Value |
|---|---|
| Name | meshoptimizer |
| Upstream | https://github.com/zeux/meshoptimizer |
| Version | v1.3 9e1f07b159d3cb777f1c67ed31fc11fd117986f4 |
| License | MIT |
| Shipped | yes |
| Projects | - |
| License files | LICENSE.md |
