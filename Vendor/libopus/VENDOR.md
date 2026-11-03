# libopus (vendored)

| | |
|---|---|
| Upstream | https://github.com/xiph/opus |
| Version | `v1.6.1` (tag) |
| Commit | `22244de5a79bd1d6d623c32e72bf1954b56235be` (2026-01-13) |
| Archive | `https://github.com/xiph/opus/archive/refs/tags/v1.6.1.tar.gz`, SHA-256 `bf0b97ec7a65890b8db90ef94c4d6c18de12584c3085031953a10986f5917745` (4,888,071 bytes), verified when vendored |
| Date vendored | 2026-10-29 (M23, W23.1 stream O; pinned by `Docs/Decisions/0034-m23-contract.md` decision 16, approved in `Docs/Decisions/0001-approvals.md`) |
| License | BSD-3-Clause (SPDX: `BSD-3-Clause`), Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo, Mozilla and Amazon: `COPYING`, upstream's, verbatim (it also names the royalty-free patent licences Opus is subject to) |
| Kind | C sources; static library, premake project `opus` (`premake5.lua`) |

## Contents

Kept: 199 files, byte-identical to the archive's files below `opus-1.6.1/`, at their upstream paths (LF line endings as
stored upstream):

- `COPYING`;
- `include/`: the six public headers (`opus.h`, `opus_custom.h`, `opus_defines.h`, `opus_multistream.h`,
  `opus_projection.h`, `opus_types.h`);
- `celt/`: every header of the directory (`celt_headers.mk`'s generic ones, the fixed-point, float and ARM NE10 static
  mode tables and the TI C5x/C6x fixed-point headers, which nothing compiled here includes) and the `CELT_SOURCES` of
  `celt_sources.mk`;
- `silk/`: every header of the directory and the `SILK_SOURCES` of `silk_sources.mk`;
- `silk/fixed/`: `main_FIX.h`, `structs_FIX.h` and the `SILK_SOURCES_FIXED` of `silk_sources.mk`;
- `src/`: every header of the directory and the `OPUS_SOURCES` and `OPUS_SOURCES_FLOAT` of `opus_sources.mk`.

The kept set's digest: SHA-256 `384fc3f04e45a2dc2c546252d54778b6d58578f10b2f7fc88abae1aed211a526` over one line
`<SHA-256 of the file>  <path>` (two spaces, the path relative to this directory with `/` separators) per kept file,
each ending in a line feed, sorted by path byte-wise; `premake5.lua`, this file and generated project files are not
kept files.

Removed (everything else of the archive): the x86, ARM and MIPS sources and headers (`celt/x86`, `celt/arm`,
`celt/mips`, `silk/x86`, `silk/arm`, `silk/mips`, `silk/fixed/x86`, `silk/fixed/arm`, `silk/fixed/mips`,
`silk/xtensa`), the floating-point SILK (`silk/float`), the deep-learning code (`dnn/`, with no model data in the tag
archive), `celt/mini_kfft.c` (QEXT only), the demos and comparison tools (`celt/opus_custom_demo.c`, `celt/dump_modes`,
`src/opus_demo.c`, `src/opus_compare.c`, `src/qext_compare.c`, `src/repacketizer_demo.c`), the tests (`tests/`,
`celt/tests`, `silk/tests`), `training/`, `doc/`, `scripts/`, the CMake, meson, autotools and make build files, and the
other top-level files (`AUTHORS`, `ChangeLog`, `LICENSE_PLEASE_READ.txt`, `NEWS`, `README`, `README.draft`,
`releases.sha2`, `tar_list.txt`, `create_opus_data.sh`, `*.pc.in`, `opus.m4`).

Local modifications: **none**. Local additions: `premake5.lua` and this file.

## What it is for

The Opus decoder under opusfile (`Vendor/opusfile/VENDOR.md`), which miniaudio's libopus decoding backend
(`Vendor/miniaudio/VENDOR.md`) reads Ogg Opus files with: M23's Ogg Opus import and playback (A-OPUS,
`Docs/Decisions/0034-m23-contract.md` decision 16). No first-party code includes its headers. The encoder is compiled,
as upstream compiles it, but nothing calls it.

## Build configuration (`premake5.lua`)

Upstream's CMake build (`CMakeLists.txt`, `cmake/OpusConfig.cmake`, `cmake/OpusSources.cmake`) in its fixed-point
configuration:

- Files: `CELT_SOURCES`, `SILK_SOURCES`, `SILK_SOURCES_FIXED`, `OPUS_SOURCES` and `OPUS_SOURCES_FLOAT` (the encoder's
  analysis, which upstream always compiles) with their headers. Include directories `include` (public), `.`, `celt`,
  `silk` and `silk/fixed` (private), as upstream's.
- Defines: `OPUS_BUILD`; `FIXED_POINT=1` (`OPUS_FIXED_POINT`); `USE_ALLOCA` (stack arrays through `alloca`: upstream's
  choice where the C compiler has no variable-length arrays, MSVC, and used on every platform here so every platform runs
  the same code; never `NONTHREADSAFE_PSEUDOSTACK`, which import workers would race on); `ENABLE_HARDENING`
  (`OPUS_HARDENING`, on by default upstream: its checks end the process instead of continuing with a broken invariant);
  `HAVE_LRINT`, `HAVE_LRINTF` (the C99 functions upstream's checks find on every supported compiler); Linux and macOS
  also `HAVE_ALLOCA_H`. `HAVE_CONFIG_H` and its `config.h` are not used: CMake's would only define `PACKAGE_VERSION`,
  which `celt/celt.c` defaults to `"unknown"` (`opus_get_version_string` then reports `libopus unknown-fixed`).
- **Left undefined, for bit-exact decoding** (the contract's determinism condition): `OPUS_HAVE_RTCD`, `CPU_INFO_BY_*`
  and every `OPUS_X86_*` and `OPUS_ARM_*` macro (no run-time CPU detection, no intrinsics: their sources are not even
  vendored), `ENABLE_DEEP_PLC`, `ENABLE_DRED`, `ENABLE_OSCE` (off by default upstream), `ENABLE_QEXT`, `ENABLE_RES24`
  (an autotools-only default; CMake and meson leave it off), `CUSTOM_MODES`, `DISABLE_FLOAT_API` (the float API stays,
  as upstream builds it; opusfile's fixed-point build never calls it), `FUZZING`, `ENABLE_ASSERTIONS` and
  `FIXED_DEBUG`. The decoder is therefore integer arithmetic throughout, the same in Debug, Release and Dist and on
  every CPU: "AudioDecoder: an Ogg Opus file decodes bit-exact to its committed digest in every configuration"
  (`Tests/Source/Engine/Audio/AudioDecoderTests.cpp`) holds the samples' digest, checked in Debug and Release, and
  FeatureTest's `AssetFormats` suite checks it in the exported Release and Dist runs.
- C dialect: C11 with Visual Studio (MSVC and clang-cl), C99 elsewhere (upstream's `CMAKE_C_STANDARD`).
- Floating point: the compiler's default model (MSVC `/fp:precise`); the fixed-point decoder computes no floating-point
  value (the float sources are the unused encoder's analysis and the unused float API's conversions).
- **Warnings.** Two MSVC warnings are disabled, each only for the translation units that show it at the workspace's
  `/W3` for vendored code, as `Docs/ReviewChecklist.md` requires of a vendored suppression:
  - C4244 ("conversion ..., possible loss of data"; seven at `/W3`) in `celt/quant_bands.c`, `celt/vq.c`,
    `src/extensions.c` and `src/mapping_matrix.c`: the fixed-point code narrows 64-bit intermediate sums (a band
    energy's prediction, an inner product after its shift, a matrix product after its rounding shift) and pointer
    differences (an extension's length within its packet) to the 32-bit types its results have by design; upstream's
    own CMake moves C4244 to level 4 for every MSVC build (`cmake/OpusConfig.cmake`: "move cosmetic warnings to level
    4"). `filter { "toolset:msc*", "files:celt/quant_bands.c or ..." } disablewarnings { "4244" }`.
  - C4293 ("shift count negative or too big") at `src/opus_multistream_encoder.c` line 216: it is on the untaken branch
    of `celt/fixed_generic.h`'s `VSHR32(a, shift)`, `((shift)>0) ? SHR32(a, shift) : SHL32(a, -(shift))`, called with
    the positive constant `DB_SHIFT-16` (`DB_SHIFT` is 24, `celt/arch.h`), so only `SHR32(..., 8)` runs; the file is the
    multistream encoder, which the engine never calls.
    `filter { "toolset:msc*", "files:src/opus_multistream_encoder.c" } disablewarnings { "4293" }`.

  The library then compiles without warnings under MSVC 14.51 in Debug, Release and Dist. Re-check after every update:
  remove a suppression upstream's sources no longer need, and never widen one to other warnings or files.
- Linux: `pic "On"`. Debug: `symbols "on"`; Release: `optimize "on"`, `symbols "on"`; Dist: `optimize "full"`,
  `symbols "off"`.
- The Linux and macOS builds are generated by the portability stage and never compiled (ADR 0028 decision 10).

## Consumer usage

```lua
-- Dependencies.lua: UseOpus(), called by UseMiniaudio() after UseOpusFile(): links only.
filter { "kind:ConsoleApp or WindowedApp or SharedLib" }
    links { "opus" }
filter {}
```

Vendored consumers (`Vendor/opusfile`, `Vendor/miniaudio`) add `../libopus/include` to their include directories and
include `<opus_multistream.h>` (through `opusfile.h`) with no defines. The public headers read `OPUS_BUILD` (the
library's own translation units: symbol export from a shared library and the custom-mode declarations), `DLL_EXPORT`,
`CUSTOM_MODES`, `ENABLE_OPUS_CUSTOM_API` and `DISABLE_PTR_CHECK`; a consumer defines none of them.
`Scripts/CheckBuildConfig.py`'s family for the project is `OPUS_*` with those names, `OPUS_BUILD` excepted as the
library's own (`VENDOR_PRIVATE_DEFINES`), and `Scripts/Lint.py` bans defining `OPUS_*` in first-party source.

## Updating

1. Download the release tag's archive from `https://github.com/xiph/opus/archive/refs/tags/<tag>.tar.gz`, record its
   SHA-256 and size, and extract it into an empty directory.
2. Replace the kept files from the new `*_sources.mk` and `*_headers.mk` lists, recompute the kept set's digest, and diff
   `CMakeLists.txt` and `cmake/*.cmake` (sources, definitions, options and their defaults, the MSVC warning options)
   against `premake5.lua`; keep every macro of the determinism list above undefined.
3. Rebuild Debug, Release and Dist (no warning), run the `AudioDecoder*` and `AudioImporter*` unit cases in Debug and
   Release: the Opus digest changes only if the decoder's output changed, which the update records.

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).

| Field | Value |
|---|---|
| Name | libopus |
| Upstream | https://github.com/xiph/opus |
| Version | 1.6.1 |
| License | BSD-3-Clause |
| Shipped | yes |
| Projects | opus |
| License files | COPYING |
