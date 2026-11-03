# tinyexr

| | |
|---|---|
| Upstream | https://github.com/syoyo/tinyexr |
| Version | `v3.2.0` (a lightweight tag) |
| Commit | `6f470c9ab24bf3992bc512ce07e8ecb00d9bf105` |
| Date vendored | 2026-11-02 (M23 W23.2 stream Y; approved in `Docs/Decisions/0001-approvals.md`, pinned by `Docs/Decisions/0034-m23-contract.md` decision 14) |
| License | BSD-3-Clause (SPDX: `BSD-3-Clause`), Syoyo Fujita and contributors, see `LICENSE`; `tinyexr.h` also carries the 3-clause BSD notice of Industrial Light & Magic for the OpenEXR code it contains (its header, and the PIZ Huffman and wavelet code), see `OpenEXR_LICENSE.txt` |
| Kind | A single C++ header with an implementation section; static library, premake project `tinyexr` (`premake5.lua`) compiling upstream's implementation unit `tinyexr.cc` |
| Release archive | `https://github.com/syoyo/tinyexr/archive/refs/tags/v3.2.0.tar.gz`, 25,444,587 bytes, SHA-256 `df2bd61124a35d8138f8b0bc22418a1d4fe33622c818e0e022f5522afc0821b0` (verified on the downloaded bytes) |

## Contents

Kept, byte-identical to the release archive (LF line endings as stored upstream), at their upstream paths:

| File | SHA-256 |
|---|---|
| `tinyexr.h` | `b1d33ca1350d64e2b9a199614991cffd45d7982ff3d68f107020a1230bde38ec` |
| `tinyexr.cc` | `88bf210298beb68b886cd2218e972a72a5cc6f339281a2ee8c2cbf971085d30d` |
| `exr_reader.hh` | `9e0bfef0487b9c3082165b2a637c75ef91d9fcc4aa55a5f32eb97f77db19f99b` |
| `streamreader.hh` | `81f2d1502930ca9ca9957e7607c2a7924d3ef2fe4b08b1a4001245dc6d3fefb6` |
| `LICENSE` | `b90388aaadc2b6f77a5c01278697a2a0539eab471a3befc6a081728becdc3390` |
| `NOTICE` | `c26b775b31bf5852e988f1cda2226f0c78e25dc7ab391b928aa766382a53823d` |

`exr_reader.hh` and its `streamreader.hh` (BSD-3-Clause, the same authors) are the bounds-checked memory reader that the
implementation section of `tinyexr.h` includes (`#include "exr_reader.hh"`), so the implementation unit does not compile
without them (decision 14 lists them since W23.2's integration, decision 21). `NOTICE` attributes the fpnge and fpng code of the version 3 C library under
`src/`, which is not vendored; it is kept with the license as upstream ships it.

Added: `premake5.lua`, this file, and `OpenEXR_LICENSE.txt`: Industrial Light & Magic's 3-clause BSD notice for the
OpenEXR code `tinyexr.h` contains, its text verbatim from `tinyexr.h`'s header (the comment block between "TinyEXR
contains some OpenEXR code" and "End of OpenEXR license") without the comment markers, so the notices file reproduces it
(the PIZ code's "Copyright (c) 2004, Industrial Light & Magic" line refers to the same license). SHA-256
`c3bcb95dd4b737de1420774d17f7fd2eb1fa7e5398ac999ea92f93b5e5f4647d`.

Removed (everything else of the archive): the version 3 C library (`src/`, `include/`), `deps/` (miniz, nanozlib, ZFP
and the others: no miniz is needed, below), the CMake, make, premake 4 and Visual Studio build files, `test/`,
`test_tinyexr.cc`, `examples/`, `experimental/`, `sandbox/`, `benchmark/`, `tools/`, `web/`, `jni/`, `attic/`, `doc/`,
`data/`, the sample images (`asakusa.exr`, `asakusa.png`), `third_party/` and the repository's documents and scripts.

Local modifications: **none**.

## What it is for

OpenEXR environments (M23: A-EXR, `Docs/Decisions/0034-m23-contract.md` decision 14). `EnvironmentImporter` version 2
reads single-part scanline and tiled images (NONE, RLE, ZIPS, ZIP, PIZ and PXR24 compression) through the version 1
API of `tinyexr.h` (`ParseEXRVersionFromMemory`, `ParseEXRHeaderFromMemory`, `LoadEXRImageFromMemory`, the free
functions), before the same checks and GPU bake as Radiance `.hdr`. Only one first-party translation unit includes the
header: `Engine/Source/Engine/AssetPipeline/Importers/EnvironmentImporter.cpp` (`Scripts/ModuleRules.json` restricts
it). AssetPipeline is editor-only, so the Runtime never links the library.

The importer validates what tinyexr would otherwise trust before it decodes a pixel: the size and aspect of the data
window, the channels, a decode budget, and every chunk the offset table names (each scanline block's first line, each
tile's coordinates and level), so a file whose chunks repeat or leave out a part of the image, which tinyexr would
decode into memory it never wrote, or a tile at a negative coordinate, which it would write outside its tile buffer, is
refused instead (`EnvironmentImporter.cpp`'s `CheckExrChunks`). The fuzz target `EnvironmentImporterExr`
("EnvironmentImporter: 200 seeded mutations of an OpenEXR file never crash") covers the reader.

## Build configuration (`premake5.lua`)

- Files: `tinyexr.cc` (with the headers listed for the IDE). C++17, like the other vendored C++ libraries.
- Defines, set alike for the library and its consumers (`UseTinyExr()`):
  - `TINYEXR_USE_MINIZ=0` and `TINYEXR_USE_STB_ZLIB=1`: ZIP and PXR24 data inflate through stb_image's zlib decoder
    (`stbi_zlib_decode_buffer`), and the writer, which the engine never calls, deflates through stb_image_write's
    (`stbi_zlib_compress`). tinyexr declares both functions itself with C linkage; stb declares them with C linkage
    too, and their implementation is the Engine library's (`Engine/Source/Engine/Graphics/ThirdParty/StbImageImplementation.cpp`,
    `Docs/Decisions/0009-m5-decisions.md` decision 11), so every executable that links `tinyexr` links `Engine`, and
    no zlib or miniz is vendored.
  - `TINYEXR_USE_THREAD=0` and `TINYEXR_USE_OPENMP=0`: no worker threads (the importer runs on the main thread,
    Architecture §4.11), and the decoded image does not depend on a thread count.
  - Left at their defaults: `TINYEXR_USE_PIZ` (1, PIZ decoding), `TINYEXR_USE_ZFP` and `TINYEXR_USE_NANOZLIB` (0).
- `tinyexr.cc` defines `NOMINMAX` for itself before the implementation includes `<windows.h>` (which it uses only for
  its file functions, never called by the engine); no consumer includes the implementation section.
- **Floating point: precise, without contraction**, like every first-party project (Architecture §2.2): MSVC
  `/fp:precise` (premake `floatingpoint "Default"`); GCC and Clang `-fno-fast-math -ffp-contract=off`; clang-cl the same
  through `/clang:`. The decoded texels (the software half-to-float conversion among them, whose denormal path
  subtracts floats) are baked into a cooked environment, whose cache key does not include the build configuration
  (§7.5). `Scripts/CheckBuildConfig.py` checks the model (`PRECISE_VENDOR_PROJECTS`, since W23.2's review), and its
  fixture `Tests/Data/BuildConfig/FastMathTinyExr` proves the check.
- Debug: `symbols "on"`; Release: `optimize "on"`, `symbols "on"`; Dist: `optimize "full"`, `symbols "off"`.
- Warnings: the compiler's default level; the library compiles without warnings under MSVC 14.51.
- Link order outside Visual Studio (never compiled, ADR 0028 decision 10): the library references stb's zlib functions
  in `Engine`, so a GNU ld link would need `Engine` after `tinyexr` as well as before it (or a link group).

## Consumer usage

- `UseTinyExr()` in `Dependencies.lua`: `externalincludedirs { Vendor/tinyexr }`, the four defines above, and, for
  linking projects, `links { "tinyexr" }`. It is called by Engine (the include directory and defines for
  `EnvironmentImporter.cpp`), EditorCore, Editor and Tests, never by the Runtime's projects (`UseRuntimeDependencies`).
  `Scripts/CheckBuildConfig.py` compares the `TINYEXR_` family (its `VENDOR_ABI_FAMILIES` entry) and `NDEBUG`, and
  `Scripts/Lint.py` bans defining a `TINYEXR_` macro in first-party source (`banned-abi-macro`).
- Include as `#include <tinyexr.h>` without `TINYEXR_IMPLEMENTATION`.
- Every allocation tinyexr returns is released through its free functions (`FreeEXRHeader`, `FreeEXRImage`,
  `FreeEXRErrorMessage`); the importer holds them in RAII guards.

## Verification performed

- Downloaded the release archive above and checked its size and SHA-256 against the contract's pin, then the SHA-256 of
  `tinyexr.h`, `tinyexr.cc`, `LICENSE` and `NOTICE` against the contract's, and of the two reader headers, on the
  extracted files.
- Built in Debug and Release with MSVC 14.51 (stream Y's `Build.py` runs, which fail on any MSBuild warning);
  `Scripts/CheckBuildConfig.py` passes on the workspace.
- Built in Release and Dist with clang-cl in the portability workspace (`Generate.py --toolset clang`, then
  `Build.py --workspace-dir bin-int/Portability/windows-vs2026-clang --allow-vendor-warnings`, as the portability stage
  builds it; its warnings, such as `-Wunused-function` for the writer's PXR24 and B44 encoders, are tolerated as the
  stage tolerates vendored code's), at W23.2's review.
- The EnvironmentImporter cases: the generated `Gradient.exr` (half-float scanlines, uncompressed) and
  `GradientZip.exr` (ZIP) decode to exactly the texels `Gradient.hdr` decodes to, as do in-memory float, RLE, tiled and
  mipmapped variants, and the generated `GradientBlocks*.exr` images, several scanline blocks of each of NONE, RLE,
  ZIPS, ZIP, PIZ and PXR24, decode to the same texels repeated over 4 x 4 texels; tiles of one level, mipmap and ripmap
  levels, rounded down and up, decode their top level; the unsupported cases are refused naming what is unsupported,
  a wrong `chunkCount` among the broken ones; and the fuzz case.

## Updating

1. `git ls-remote --tags https://github.com/syoyo/tinyexr.git`
2. Download the new tag's archive, verify it, and replace the kept files (check whether the implementation section of
   `tinyexr.h` includes new local headers); update `LICENSE` and `NOTICE` if they changed.
3. Update this file (version, commit, date, hashes), rebuild every configuration, run `python Scripts/CheckBuildConfig.py`
   and the EnvironmentImporter cases, the fuzz case included. A decoder change that changes decoded texels needs
   `EnvironmentImporter::Version` raised, so cached environments are rebuilt.

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).
Shipped is `yes` by the notices' mechanical rule: the generated Engine project references this directory (its include
path), so the library ships attributed, although no exported game links it (Projects is `-`: no project links it into the
Runtime), as nativefiledialog-extended's attribution over-approximates.

| Field | Value |
|---|---|
| Name | tinyexr |
| Upstream | https://github.com/syoyo/tinyexr |
| Version | v3.2.0 6f470c9ab24bf3992bc512ce07e8ecb00d9bf105 |
| License | BSD-3-Clause |
| Shipped | yes |
| Projects | - |
| License files | LICENSE, OpenEXR_LICENSE.txt |
