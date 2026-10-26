# stb (stb_image, stb_image_write, stb_image_resize2, stb_truetype, stb_vorbis)

| | |
|---|---|
| Upstream | https://github.com/nothings/stb |
| Version | No release tags exist upstream; pinned to `master`. Header versions: stb_image v2.30, stb_image_write v1.16, stb_image_resize2 v2.18, stb_truetype v1.26, stb_vorbis v1.22 |
| Commit | `2c980bb59875b0d32144a71867fbdebb2f77cd20` (master, 2026-08-01) |
| Date vendored | 2026-10-05 (stb_vorbis.c: 2026-10-08, M12; stb_truetype.h: 2026-10-20, M19; both at the same commit) |
| License | Dual: MIT **or** Public Domain (Unlicense), at the user's choice (SPDX: `MIT OR Unlicense`). Full text: `LICENSE` (also at the end of each header). |
| Kind | Single-header C libraries (no premake project); compile as C or C++. `stb_vorbis.c` is compiled only inside miniaudio's implementation translation unit (below) |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings):

- `stb_image.h` - image loading (PNG, JPEG, TGA, BMP, PSD, GIF, HDR, PIC, PNM)
- `stb_image_write.h` - PNG/BMP/TGA/JPEG/HDR writing
- `stb_image_resize2.h` - image resizing (SSE2/AVX/NEON SIMD)
- `stb_truetype.h` - TrueType and OpenType (CFF) font parsing, glyph outlines and distance fields (v1.26; SHA-256
  `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab`, 199,192 bytes; git blob
  `90a5c2e2b3fe563c98585294ca7a949876aec94c`, the one GitHub lists for the file at the commit), approved for M19 in
  `Docs/Decisions/0001-approvals.md` (ADR 0028 decision 7; `Docs/Decisions/0030-m19-contract.md` decision 23). Dear
  ImGui keeps its own copy of the same version, `Vendor/imgui/imstb_truetype.h`, for its font atlas: a snapshot with
  three `[DEAR IMGUI]` changes (a CID-keyed CFF glyph without a font dictionary reads no local subroutines, where this
  header asserts; a division guard in the bitmap rasterizer; one dropped initialization) that also predates this
  header's thresholds for near-singular segments in `stbtt_GetGlyphSDF` (its `eps`).
- `stb_vorbis.c` - Ogg Vorbis decoder (v1.22; SHA-256 `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`,
  192,790 bytes), approved for M12 in `Docs/Decisions/0001-approvals.md`. Byte-identical to miniaudio 0.11.25's
  `extras/stb_vorbis.c` (same SHA-256), the copy miniaudio documents for its Vorbis backend.
- `LICENSE`

Removed: every other stb library (`stb_ds.h`, `stb_rect_pack.h`, ...), `deprecated/`, `docs/`, `data/`, `tests/`, `tools/`, `stb_image_resize_test/`, `README.md`, `CONTRIBUTING.md`, `SECURITY.md`, `.NO_AI/`, `.travis.yml`, `.github/`. Add other stb headers here only when the engine needs them.

Local modifications: **none**. Do not rename `stb_image_resize2.h`: its implementation re-includes itself by name (`STBIR__HEADER_FILENAME`).

Upstream policy note: stb's `CONTRIBUTING.md` forbids issues and pull requests written with generative AI. AI agents working on this engine must not file issues/PRs against stb; a human must write any upstream report. (This does not restrict using the library.)

## Consumer usage

- Include directory: `Vendor/stb` (add as an *external* include dir - required for warning-free /WX builds on MSVC, see Warnings)
- Include as: `#include <stb_image.h>`, `<stb_image_write.h>`, `<stb_image_resize2.h>`, `<stb_truetype.h>`
- Link requirements: nothing beyond the C runtime and the math library. Windows/macOS: none. Linux: `libm` (`pow`, `ldexp`, ...), which `g++`/`clang++` already link for every C++ program; add `links { "m" }` only if the code ever ends up in a pure-C link.

### stb_vorbis (Ogg Vorbis, M12)

`stb_vorbis.c` is not used by first-party code. The miniaudio project compiles it inside its local implementation
translation unit `Vendor/miniaudio/miniaudio_vorbis.c` (with `Vendor/stb` on that project's include path): the
declarations (`STB_VORBIS_HEADER_ONLY`) before miniaudio's implementation, which then registers its Vorbis decoding
backend, and the decoder's implementation after it (`Vendor/miniaudio/VENDOR.md`, "Local additions"). Engine code decodes
Ogg Vorbis only through miniaudio (`Engine/Audio`). No defines are set: the stdio paths stay in, since miniaudio's
backend uses `stb_vorbis_open_filename` for file paths. It adds no MSVC warning at the default warning level of the
miniaudio project (checked with MSVC 14.51 when it was vendored). clang-cl (the portability stage's build, which tolerates
vendored warnings) reports one: `-Wtautological-compare` at line 1404 (`f->stream_start + loc < f->stream_start`, an
overflow check of `set_file_offset` that the compiler may fold because pointer overflow is undefined); it is upstream's
code and harmless for the in-memory streams miniaudio hands it, whose offsets come from the decoder itself.

### stb_truetype (fonts, M19)

`stb_truetype.h` is compiled only by `FontImporter`'s rasterizer,
`Engine/Source/Engine/AssetPipeline/ThirdParty/TrueTypeRasterizer.cpp`, its one implementation translation unit (no PCH:
the `ThirdParty` filter of `Engine/premake5.lua`):

```cpp
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_sqrt(x) std::sqrt(x)
#define STBTT_pow(x, y) ::Engine::DetMath::Pow(x, y)
#define STBTT_cos(x) ::Engine::DetMath::Cos(x)
#define STBTT_acos(x) ::Engine::DetMath::ACos(x)
#include <stb_truetype.h>
```

- `STBTT_STATIC` gives every function internal linkage, so this copy never clashes with Dear ImGui's, which
  `imgui_draw.cpp` also compiles with `STBTT_STATIC`. Both copies define the same `stbtt_*` types (`stbtt_fontinfo`,
  `stbtt_vertex`, ...) in the global namespace, so their definitions must stay identical (the one-definition rule): they
  are, at this commit and Dear ImGui 1.92.9b (see Updating). The rasterizer's unit includes no stb_rect_pack, so the
  header also defines its minimal stand-ins for `stbrp_context`, `stbrp_node` and `stbrp_rect` (for the packing API,
  which the engine never calls), while `imgui_draw.cpp` includes Dear ImGui's `imstb_rectpack.h` first and gets that
  library's definitions under the same names. The two units have differed this way since M6, when the rasterizer
  compiled Dear ImGui's copy; no function with external linkage takes those types.
- The `pow`, `cos` and `acos` of the distance-field solver go through DetMath (Architecture §4.12), so a glyph's field
  does not depend on the C runtime; `sqrt`, `fabs`, `fmod`, `floor` and `ceil` are IEEE-exact and keep their defaults.
- `STBTT_assert` stays `assert`, active in Debug and Release. The library reads fonts without bounds checks and asserts
  on some malformed structures, so the engine validates a font before it opens one (`ValidateTrueTypeTables`,
  `AssetPipeline/Private/TrueTypeTables.h`); a change to what the engine calls, or a newer header, is checked against
  that validation.
- The rasterizer computes the distance field with the body of this header's `stbtt_GetGlyphSDF`, after replacing the
  cubic segments of CFF outlines by quadratics (ADR 0022 "Integration"); for a TrueType outline the field equals
  `stbtt_GetGlyphSDF`'s byte for byte.

### Implementation TU (exactly one in the whole program)

```cpp
// e.g. Engine/Source/Engine/Asset/StbImplementation.cpp - must not use the PCH
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
```

Keep these in their own TU (excluded from the PCH via `flags { "NoPCH" }` on a `files:` filter): the stb_image_resize2 implementation sets `#pragma fp_contract(off)` (MSVC) / `#pragma GCC optimize("fp-contract=off")` (GCC) for the rest of the TU to keep its results deterministic across x64/arm64/SIMD/scalar, which must not leak into engine code.

### Defines (place them in the implementation TU before the includes unless noted)

None required. Relevant options:

| Define | Notes |
|---|---|
| `STBI_WINDOWS_UTF8`, `STBIW_WINDOWS_UTF8` | Make `stbi_load(filename)` / `stbi_write_*(filename)` treat paths as UTF-8 on Windows. Recommended instead: the engine reads/writes files through its own file system and uses the `*_from_memory` / `*_to_func` APIs (verified), so these are not needed. |
| `STBI_ASSERT(x)`, `STBIW_ASSERT(x)`, `STBIR_ASSERT(x)` | Route internal asserts to `ENGINE_ASSERT` if desired. |
| `STBI_MALLOC/STBI_REALLOC/STBI_FREE` (and `STBIW_*`, `STBIR_MALLOC/STBIR_FREE`) | Allocator hooks. |
| `STBI_NO_*` / `STBI_ONLY_*` | Strip formats to reduce code size and attack surface (e.g. `STBI_NO_PSD STBI_NO_PIC STBI_NO_PNM`). stb_image is not hardened against hostile input (see upstream SECURITY.md); load only trusted game assets. |
| `STBI_MAX_DIMENSIONS` | Reject images larger than N x N (default 1 << 24). |
| `STBIR_AVX` / `STBIR_AVX2` | Optional wider SIMD; only together with `/arch:AVX(2)` / `-mavx(2)`. SSE2 (x64) and NEON (arm64) are enabled automatically. |

Notes: `stbi_set_flip_vertically_on_load` is process-global (Vulkan's image origin is top-left, so no flip is normally needed). For HDRIs use `stbi_loadf*` (Radiance .hdr round trip verified).

## Warnings (MSVC 14.51, C++23, /W4 /permissive- /Zc:preprocessor /utf-8)

With `Vendor/stb` as an **ordinary** include dir, the implementation TU produces exactly one warning, identical in Debug/Release/Dist:

- `stb_image_write.h(776)`: **C4996** `'sprintf'` (CRT deprecation). stb_image_write defines `_CRT_SECURE_NO_WARNINGS` itself, but that only works if no CRT header was included earlier in the TU; `stb_image.h` (included first above) already pulls in `<stdio.h>`.

stb_image.h and stb_image_resize2.h produce no warnings at /W4. TUs that only include the declarations produce no warnings.

stb_truetype.h's implementation with `STBTT_STATIC` (checked with MSVC 14.51 and Visual Studio's clang-cl 22.1.3 at `/W4`,
2026-10-20) reports only unused functions with internal linkage (MSVC **C4505**, clang `-Wunused-function`), one at the
definition of each API function the translation unit does not call. As an external include directory (below) it reports
none, with `/WX`.

Suppression (either one; the first was verified with `/WX`):

1. **Recommended:** add `Vendor/stb` via premake `externalincludedirs` + `externalwarnings "Off"` (MSVC `/external:W0`). Verified: zero warnings with `/W4 /WX`.
2. Define `_CRT_SECURE_NO_WARNINGS` for the implementation file only (`filter "files:**/StbImplementation.cpp"` + `defines { "_CRT_SECURE_NO_WARNINGS" }`) - never project-wide.

Linux/macOS (not buildable here), clang 22 preview with `-Wall -Wextra`: `stb_image_write.h` reports 8 x `-Wmissing-field-initializers` (lines 514, 522, 613, 621, 789, 796, 1609, 1618 - `stbi__write_context s = { 0 };` compiled as C++), and with `-Wpedantic` on the MSVC target `stb_image_resize2.h(396)` `-Wlanguage-extension-token` (`unsigned __int64`). Use `externalincludedirs` (`-isystem`) on all platforms.

## Verification performed

Throwaway workspace built Debug/Release/Dist (VS 2026 v145, compiled as C++23): `stbi_write_png_to_func` -> `stbi_info_from_memory` -> `stbi_load_from_memory` bit-exact round trip, `stbi_write_hdr_to_func` -> `stbi_is_hdr_from_memory` -> `stbi_loadf_from_memory` within RGBE precision, garbage input rejected with `stbi_failure_reason`, `stbir_resize_uint8_srgb` (RGBA) and `stbir_resize_float_linear` (RGB) - all passed.

`stb_truetype.h` (M19): downloaded from
`https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h`; its git blob
hash equals the one the GitHub contents API lists for that path at the commit. The `FontImporter` unit tests pass in Debug
and Release: the Inter TrueType and CFF atlases at the default settings hash as with Dear ImGui's copy (so the Text golden
is unchanged), and a temporary comparison confirmed that the rasterizer's field equals this header's `stbtt_GetGlyphSDF`
for every TrueType glyph the unit suite imports.

## Updating

stb has no releases; pin a `master` commit.

1. `git ls-remote https://github.com/nothings/stb.git refs/heads/master`
2. `git clone --depth 1 https://github.com/nothings/stb.git <tmp>` (verify `git rev-parse HEAD`)
3. Replace `stb_image.h`, `stb_image_write.h`, `stb_image_resize2.h`, `stb_truetype.h`, `stb_vorbis.c`, `LICENSE`. Compare
   `stb_vorbis.c` with the `extras/stb_vorbis.c` of the vendored miniaudio release and record any difference here.
   Compare `stb_truetype.h` with the vendored Dear ImGui's `imstb_truetype.h`, whose type definitions it must keep (see
   stb_truetype above), and follow any change to what the font validation guards (`TrueTypeTables.cpp`: the glyph
   outlines, the CFF structures and the charstring interpreter) or to `stbtt_GetGlyphSDF` in the validation and the
   rasterizer.
4. Ensure LF line endings (clone with `-c core.autocrlf=false`); note the new header version numbers from the first line of each header.
5. Update this file (commit, versions, SHA-256 of `stb_vorbis.c` and `stb_truetype.h`, date, warning line numbers), rebuild all configurations and run the unit tests (image import tests, the audio decoder and importer tests for stb_vorbis, and the `FontImporter` tests for stb_truetype, whose pinned atlas hashes show whether a newer header changes the atlases, and so the Text golden).

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).

| Field | Value |
|---|---|
| Name | stb |
| Upstream | https://github.com/nothings/stb |
| Version | master 2c980bb59875b0d32144a71867fbdebb2f77cd20 |
| License | MIT OR Unlicense |
| Shipped | yes |
| Projects | - |
| License files | LICENSE |
