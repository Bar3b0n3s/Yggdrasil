# cgltf

| | |
|---|---|
| Upstream | https://github.com/jkuhlmann/cgltf |
| Version | 1.15 (tag `v1.15`, latest stable release) |
| Commit | `360db1a95480fe102ae9c69b27c5d101167ff5ba` (2025-02-09) |
| Date vendored | 2026-10-05 |
| License | MIT (SPDX: `MIT`). Full text: `LICENSE` (also repeated at the end of each header). Embeds jsmn (MIT). |
| Kind | Single-header C99 library (no premake project); compiles as C or C++ |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings):

- `cgltf.h` - glTF 2.0 / GLB parser (+ embedded jsmn JSON tokenizer)
- `cgltf_write.h` - glTF writer (includes `cgltf.h`)
- `LICENSE`

Removed: `fuzz/`, `test/`, `README.md`, `.github/`.

Local modifications: **none**.

## Consumer usage

- Include directory: `Vendor/cgltf` (add as an *external* include dir - required for warning-free /WX builds on MSVC, see Warnings)
- Include as: `#include <cgltf.h>` (and `<cgltf_write.h>` where writing is needed)
- Link requirements: none on any platform (only the C runtime).

### Implementation TU (exactly one in the whole program)

```cpp
// e.g. Engine/Source/Engine/Asset/CgltfImplementation.cpp - must not use the PCH
#define CGLTF_IMPLEMENTATION
#define CGLTF_WRITE_IMPLEMENTATION
#include <cgltf_write.h>   // includes cgltf.h - do NOT also include cgltf.h here
```

Pitfall (verified): `#define CGLTF_IMPLEMENTATION` + `#include <cgltf.h>` followed by `#define CGLTF_WRITE_IMPLEMENTATION` + `#include <cgltf_write.h>` compiles the parser implementation **twice** (its implementation section is not include-guarded) and fails with C2084/C2371 redefinition errors. Use the pattern above (it is the one documented upstream). If the writer is not needed: `#define CGLTF_IMPLEMENTATION` + `#include <cgltf.h>`.

Exclude the implementation file from the precompiled header (premake: `filter "files:**/CgltfImplementation.cpp"` + `flags { "NoPCH" }`) so the implementation macros are seen before any other inclusion.

### Defines

None required. All configuration is runtime via `cgltf_options`:

- `options.file.read` / `options.file.release`: **recommended** - route file access through the engine's file system. The default reader uses narrow `fopen`, which on Windows cannot open non-ASCII (UTF-8) paths. Alternatively read the .gltf/.glb into memory yourself, call `cgltf_parse`, then `cgltf_load_buffers(&options, data, gltfPath)` with a custom `file.read` for external `.bin` files.
- `options.memory.alloc_func` / `free_func`: optional allocator hooks.

Always call `cgltf_validate(data)` after `cgltf_load_buffers`. cgltf does not decode images (use stb_image) and does not decompress Draco/meshopt data (it only parses the extension metadata).

## Warnings (MSVC 14.51, C++23, /W4 /permissive- /Zc:preprocessor /utf-8)

With `Vendor/cgltf` as an **ordinary** include dir, the implementation TU produces 14 x **C4996** (CRT "unsafe function" deprecation), identical in Debug/Release/Dist:

- `cgltf.h(1045)`: `fopen`
- `cgltf.h(1284, 1819, 1824, 2716, 2726, 2737, 2816, 2974, 2996, 3007)`: `strncpy`
- `cgltf.h(1285, 1289)`: `strcpy`
- `cgltf_write.h(1253)`: `fopen`

TUs that only include the declarations (no `CGLTF_IMPLEMENTATION`) produce no warnings.

Suppression (either one; the first was verified with `/WX`):

1. **Recommended:** add `Vendor/cgltf` via premake `externalincludedirs` and set `externalwarnings "Off"` (MSVC `/external:I ... /external:W0`). Verified: zero warnings with `/W4 /WX`.
2. Define `_CRT_SECURE_NO_WARNINGS` for the implementation file only (`filter "files:**/CgltfImplementation.cpp"` + `defines { "_CRT_SECURE_NO_WARNINGS" }`) - never project-wide.

Linux/macOS (not buildable here): glibc/libc++ do not deprecate these functions; clang 22 preview additionally reports `-Wlanguage-extension-token` (`__int64` under `_MSC_VER`) only with `-Wpedantic` on the MSVC target. Use `externalincludedirs` (`-isystem`) on all platforms.

## Verification performed

Throwaway workspace built Debug/Release/Dist (VS 2026 v145, compiled as C++23): `cgltf_parse` on an embedded minimal glTF (one triangle, base64 data URI buffer), `cgltf_load_buffers`, `cgltf_validate`, `cgltf_accessor_read_float`, `cgltf_accessor_read_index`, `cgltf_node_transform_world`, `cgltf_write` to memory, `cgltf_free` - all passed.

## Updating

1. `git ls-remote --tags https://github.com/jkuhlmann/cgltf.git`, pick the newest release tag `vX.Y`.
2. `git clone --depth 1 --branch <tag> https://github.com/jkuhlmann/cgltf.git <tmp>`
3. Replace `cgltf.h`, `cgltf_write.h`, `LICENSE`.
4. Ensure LF line endings (clone with `-c core.autocrlf=false`).
5. Update this file (tag, SHA, date, warning line numbers), rebuild all configurations and run the unit tests (glTF import tests).
