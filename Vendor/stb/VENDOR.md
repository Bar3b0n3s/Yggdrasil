# stb (stb_image, stb_image_write, stb_image_resize2)

| | |
|---|---|
| Upstream | https://github.com/nothings/stb |
| Version | No release tags exist upstream; pinned to `master`. Header versions: stb_image v2.30, stb_image_write v1.16, stb_image_resize2 v2.18 |
| Commit | `2c980bb59875b0d32144a71867fbdebb2f77cd20` (master, 2026-08-01) |
| Date vendored | 2026-10-05 |
| License | Dual: MIT **or** Public Domain (Unlicense), at the user's choice (SPDX: `MIT OR Unlicense`). Full text: `LICENSE` (also at the end of each header). |
| Kind | Single-header C libraries (no premake project); compile as C or C++ |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings):

- `stb_image.h` - image loading (PNG, JPEG, TGA, BMP, PSD, GIF, HDR, PIC, PNM)
- `stb_image_write.h` - PNG/BMP/TGA/JPEG/HDR writing
- `stb_image_resize2.h` - image resizing (SSE2/AVX/NEON SIMD)
- `LICENSE`

Removed: every other stb library (`stb_truetype.h`, `stb_vorbis.c`, `stb_ds.h`, ...), `deprecated/`, `docs/`, `data/`, `tests/`, `tools/`, `stb_image_resize_test/`, `README.md`, `CONTRIBUTING.md`, `SECURITY.md`, `.NO_AI/`, `.travis.yml`, `.github/`. Add other stb headers here only when the engine needs them.

Local modifications: **none**. Do not rename `stb_image_resize2.h`: its implementation re-includes itself by name (`STBIR__HEADER_FILENAME`).

Upstream policy note: stb's `CONTRIBUTING.md` forbids issues and pull requests written with generative AI. AI agents working on this engine must not file issues/PRs against stb; a human must write any upstream report. (This does not restrict using the library.)

## Consumer usage

- Include directory: `Vendor/stb` (add as an *external* include dir - required for warning-free /WX builds on MSVC, see Warnings)
- Include as: `#include <stb_image.h>`, `<stb_image_write.h>`, `<stb_image_resize2.h>`
- Link requirements: nothing beyond the C runtime and the math library. Windows/macOS: none. Linux: `libm` (`pow`, `ldexp`, ...), which `g++`/`clang++` already link for every C++ program; add `links { "m" }` only if the code ever ends up in a pure-C link.

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

Suppression (either one; the first was verified with `/WX`):

1. **Recommended:** add `Vendor/stb` via premake `externalincludedirs` + `externalwarnings "Off"` (MSVC `/external:W0`). Verified: zero warnings with `/W4 /WX`.
2. Define `_CRT_SECURE_NO_WARNINGS` for the implementation file only (`filter "files:**/StbImplementation.cpp"` + `defines { "_CRT_SECURE_NO_WARNINGS" }`) - never project-wide.

Linux/macOS (not buildable here), clang 22 preview with `-Wall -Wextra`: `stb_image_write.h` reports 8 x `-Wmissing-field-initializers` (lines 514, 522, 613, 621, 789, 796, 1609, 1618 - `stbi__write_context s = { 0 };` compiled as C++), and with `-Wpedantic` on the MSVC target `stb_image_resize2.h(396)` `-Wlanguage-extension-token` (`unsigned __int64`). Use `externalincludedirs` (`-isystem`) on all platforms.

## Verification performed

Throwaway workspace built Debug/Release/Dist (VS 2026 v145, compiled as C++23): `stbi_write_png_to_func` -> `stbi_info_from_memory` -> `stbi_load_from_memory` bit-exact round trip, `stbi_write_hdr_to_func` -> `stbi_is_hdr_from_memory` -> `stbi_loadf_from_memory` within RGBE precision, garbage input rejected with `stbi_failure_reason`, `stbir_resize_uint8_srgb` (RGBA) and `stbir_resize_float_linear` (RGB) - all passed.

## Updating

stb has no releases; pin a `master` commit.

1. `git ls-remote https://github.com/nothings/stb.git refs/heads/master`
2. `git clone --depth 1 https://github.com/nothings/stb.git <tmp>` (verify `git rev-parse HEAD`)
3. Replace `stb_image.h`, `stb_image_write.h`, `stb_image_resize2.h`, `LICENSE`.
4. Ensure LF line endings (clone with `-c core.autocrlf=false`); note the new header version numbers from the first line of each header.
5. Update this file (commit, versions, date, warning line numbers), rebuild all configurations and run the unit tests (image import tests).
