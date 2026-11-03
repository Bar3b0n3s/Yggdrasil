# HarfBuzz (vendored)

| Field | Value |
|---|---|
| Upstream | https://github.com/harfbuzz/harfbuzz |
| Version / tag | `15.0.0` (the latest release at vendoring time, published 2026-10-22) |
| Commit | `35f656a1a2bab548ea7f54a6dd3fba2b09680d14` (the tag `15.0.0`, an annotated tag object `66946f52d6c27f635c0b9c23d55fa1df6b76a5fc`) |
| Archive | `https://github.com/harfbuzz/harfbuzz/releases/download/15.0.0/harfbuzz-15.0.0.tar.xz`, 20,396,372 bytes, SHA-256 `968536e9abd782f07f7954044c0c4020199fcdd39b41d221ec42d2a3be4a44df` (the digest GitHub publishes for the release asset; verified on the downloaded bytes) |
| Date vendored | 2026-11-03 (M22's contract C22.3, Docs/Decisions/0033-m22-contract.md decision 41) |
| License | `MIT-Modern-Variant` ("Old MIT") - `COPYING` |
| Local modifications | None. Every kept file is byte-identical to the archive's (all of them use LF line endings already). |
| Approval | `Docs/Decisions/0001-approvals.md` (HarfBuzz and SheenBidi, ADR 0028 decision 7) |

## What was kept

`COPYING` and the 365 files of `src/` that the amalgamated translation unit `src/harfbuzz-subset.cc` and the public
headers `hb.h`, `hb-ot.h` and `hb-subset.h` include, directly or through other kept files (every `#include` followed
whatever its condition, so a few backend headers such as `hb-ft.h` that only a disabled backend's sources include are
kept with them, and the two string tables `hb-ot-cff1-std-str.hh` and `hb-ot-post-macroman.hh`, which
`hb-string-array.hh` includes through the macro `HB_STRING_ARRAY_LIST`), in upstream's layout (`src/`, `src/OT/`,
`src/graph/`): 366 files, 5,587,532 bytes.

The kept tree's digest is SHA-256 `358ba9febf6a59f5c763b809b20898403afb9f85d34779ac46c8247f302c3c90`: the SHA-256 of
the UTF-8 text made of one line `<file SHA-256>  <path>\n` per kept file (lower-case hex, two spaces, the path relative
to this directory with `/` separators), sorted by path byte-wise. `COPYING`'s SHA-256 is
`ba8f810f2455c2f08e2d56bb49b72f37fcf68f1f4fade38977cfd7372050ad64`.

## What was removed

Everything else of the archive: `test/`, `perf/`, `util/`, `docs/`, `subprojects/`, the build files (`meson.build`,
`meson_options.txt`, `CMakeLists.txt`, `replace-enum-strings.cmake`, the `*.pc.in` and `*.cmake.in` templates), the
other amalgamations (`src/harfbuzz.cc`, which adds the platform backends, and `src/harfbuzz-world.cc`), the platform
backends' sources (FreeType, ICU, GLib, GObject, Cairo, CoreText, DirectWrite, GDI, Uniscribe, Graphite2), the
WebAssembly (`src/wasm/`), Rust (`src/rust/`) and `kbts` shapers, the raster, vector and GPU libraries, `src/ms-use/`
(the Universal Shaping Engine's data sources, with their own `COPYING`), upstream's tests and tools in `src/`
(`main.cc`, `test-*.cc`, the `gen-*.py` and `check-*.py` scripts and their `*.txt` inputs), and the top-level documents
(`README*`, `NEWS`, `AUTHORS`, `THANKS`, `BUILD.md`, `CONFIG.md`, `TESTING.md`, `RELEASING.md`, `SECURITY.md`,
`AGENTS.md`, `CLAUDE.md`, `CODE_OF_AI_CONDUCT.md`, `AVAR2-INSTANCING.md`, `harfbuzz.doap`, the images).

## Premake project (`premake5.lua`)

| Project | Contents | Depends on |
|---|---|---|
| `HarfBuzz` | `src/harfbuzz-subset.cc` (the shaping library and the subsetter in one translation unit) | - |

Build settings:

- C++17, a static library, one translation unit: upstream's amalgamated `harfbuzz-subset.cc`, which includes the
  OpenType shaper with its built-in Unicode data (`hb-ucd.cc`, so no ICU or GLib), the face, font and buffer objects and
  the subsetter (`hb-subset*.cc`), and no platform backend. The other `.cc` files are kept beside it only because it
  includes them.
- Defines (private to the library, never read by a public header): `HB_NO_GETENV` (no reads of the `HB_OPTIONS`,
  `HB_SHAPER_LIST`, `HB_FACE_LOADER` and `HB_FONT_FUNCS` environment variables) and `HB_NO_SETLOCALE` (no read of the C
  locale in `hb_language_get_default`), so nothing in the process's environment changes how text is shaped. Listed in
  `Scripts/CheckBuildConfig.py`'s `VENDOR_PRIVATE_DEFINES`; the `HB_` family is otherwise empty in every project
  (`VENDOR_ABI_FAMILIES`, and `Scripts/Lint.py`'s `banned-abi-macro` keeps first-party sources from defining one).
- Process-level state (Architecture §3 rule 5's rule for vendored libraries, ADR 0033 decision 41): the library keeps
  write-once statics created by thread-safe lazy initialization (the Unicode and OpenType function tables in `hb-ucd.cc`
  and `hb-ot-font.cc`, the shaper list in `hb-shaper.cc`, the face-loader and font-functions lists in `hb-face.cc` and
  `hb-font.cc`, and the interned language list in `hb-common.cc`, whose entries never change once added), the atomic hint
  indices of its language-tag lookups in `hb-ot-tag.cc` (checked before every use) and `_hb_CrapPool`, the write sink for
  writes to its null objects on error paths, which nothing reads for a result. None of it changes a result, the engine
  never initializes or tears any of it down, and without upstream's `config.h` (`HAVE_ATEXIT`) it registers no `atexit`
  handler; its atomic reference counts, those of the shared function tables included, decide no result.
- MSVC (and clang-cl): `/bigobj` and `/utf-8`, upstream's own options (its `CMakeLists.txt` and `meson.build`): the
  amalgamated subsetter exceeds the object format's default section limit, and the sources' comments are UTF-8.
- MSVC: warnings C4244 and C4267 disabled for this project, as upstream's own build disables them (its
  `CMakeLists.txt`: "lossy type conversion (e.g. double/size_t -> int)"): its sources make such conversions on purpose,
  86 C4244 and 28 C4267 reports at MSVC's default level in the vendored translation unit (MSVC 14.51), and they stay
  unmodified. No other warning is disabled, and consumers' translation units keep every warning (the include directory
  is external).
- Windows: `_CRT_SECURE_NO_WARNINGS` and `_CRT_NONSTDC_NO_WARNINGS`, upstream's own definitions (its `CMakeLists.txt`),
  so the CRT does not mark the C functions the sources call (`strncpy`, `strcpy`, `fopen`, `mbstowcs`) deprecated
  (C4996). They change no declaration's layout and only this project's translation unit sees them.
- Floating point: the precise model without contraction (`floatingpoint "Default"`, and `-fno-fast-math
  -ffp-contract=off` for GCC and Clang, through `/clang:` for clang-cl), as for every first-party project
  (Architecture §2.2): shaped positions reach the UI's layout and the cooked fonts, which must be identical in every
  configuration. `Scripts/CheckBuildConfig.py` checks it (`PRECISE_VENDOR_PROJECTS`; the fixture
  `Tests/Data/BuildConfig/FastMathHarfBuzz` must fail).
- Debug, Release and Dist follow the other vendored libraries (symbols in Debug and Release, `optimize "full"` in Dist).

## Consumer usage

`Dependencies.lua`'s `UseHarfBuzz()`: the external include directory `Vendor/HarfBuzz/src` (consumers include `<hb.h>`,
`<hb-ot.h>` and `<hb-subset.h>`), no defines, and the link of the `HarfBuzz` library in every executable (the Engine,
EditorCore, Editor, Tests, RuntimeCore and Runtime projects call it; `UseRuntimeDependencies()` holds it, so a game's
native module and its Runtime link it too). `Scripts/ModuleRules.json` gives it to the Renderer (the text-measurement
layer's shaping) and AssetPipeline (the font importer's shaping glyphs and subset) modules, privately: no public header
exposes a HarfBuzz type (Architecture §3).

How the engine calls it is ADR 0033's (decisions 43 and 44): a face and font per font of the chain from the cooked
font's shaping data, its scale the font's units per em, so positions are integers in font units; the buffer's language
set explicitly (never `hb_language_get_default`), and the script and direction from the bidirectional runs.

## Verification

- The archive's SHA-256 matches the digest GitHub publishes for the release asset; the kept files match the archive's
  byte for byte (the tree digest above).
- `python Scripts/CheckBuildConfig.py` passes on the workspace and fails on `FastMathHarfBuzz`.
- The library builds without warnings in Debug and Release with MSVC (14.51, the workspace build) and in Release and Dist
  with clang-cl (the portability stage's build of Architecture §15.8). Its GCC, Clang and Apple Clang builds are never
  compiled (`Docs/Decisions/0028-backlog-program.md` decision 10).

## Updating

1. Download the release's `harfbuzz-<version>.tar.xz` from the GitHub release and compare its SHA-256 with the digest
   the release page lists.
2. Recompute the include closure of `src/harfbuzz-subset.cc`, `hb.h`, `hb-ot.h` and `hb-subset.h`, replace `src/` with
   it and `COPYING` with the archive's.
3. Update this file (version, commit, archive, digests, the kept count), rebuild every configuration and the clang-cl
   portability build, run `python Scripts/CheckBuildConfig.py` and the text-measurement and font-importer tests, and bump
   `FontImporter::Version` when cooked fonts change.

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).

| Field | Value |
|---|---|
| Name | HarfBuzz |
| Upstream | https://github.com/harfbuzz/harfbuzz |
| Version | 15.0.0 |
| License | MIT-Modern-Variant |
| Shipped | yes |
| Projects | HarfBuzz |
| License files | COPYING |
