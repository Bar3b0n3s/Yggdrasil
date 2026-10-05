# glm (OpenGL Mathematics)

| | |
|---|---|
| Upstream | https://github.com/g-truc/glm |
| Version | 1.0.3 (tag `1.0.3`, latest stable release) |
| Commit | `8d1fd52e5ab5590e2c81768ace50c72bae28f2ed` (2025-12-31) |
| Date vendored | 2026-10-05 |
| License | MIT (SPDX: `MIT`). Upstream offers "The Happy Bunny License (Modified MIT)" **or** MIT; we use it under the MIT License. Full text: `copying.txt`. |
| Kind | Header-only (no premake project) |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings):

- `glm/**` - every `*.h`, `*.hpp`, `*.inl` (430 files: `glm/`, `glm/detail`, `glm/ext`, `glm/gtc`, `glm/gtx`, `glm/simd`)
- `copying.txt` - license

Removed:

- `glm/detail/glm.cpp` (explicit-instantiation TU used only when building glm as a compiled library)
- `glm/glm.cppm` (C++20 module interface unit; the engine uses headers)
- `glm/CMakeLists.txt`, `CMakeLists.txt`, `cmake/`
- `doc/` (20 MB), `manual.md`, `readme.md`, `test/`, `util/` (natvis/autoexp debugger files), `.github/`, `.gitignore`

Local modifications: **none**.

## Consumer usage

- Include directory: `Vendor/glm` (add as an *external* include dir, see Warnings)
- Include as: `#include <glm/glm.hpp>`, `<glm/gtc/matrix_transform.hpp>`, `<glm/gtc/quaternion.hpp>`, `<glm/gtc/type_ptr.hpp>`, `<glm/gtx/matrix_decompose.hpp>`, ...
- Link requirements: none on any platform (header-only).

### Defines (must be identical in every TU and every project - put them in the workspace/Engine `defines`, never in a source file)

| Define | Use? | Why |
|---|---|---|
| `GLM_FORCE_DEPTH_ZERO_TO_ONE` | **Yes** | Vulkan clip space depth is [0, 1]. Verified: `glm::perspective` maps near -> 0, far -> 1. |
| `GLM_ENABLE_EXPERIMENTAL` | **Yes** | Required by every `glm/gtx/*` header (e.g. `matrix_decompose.hpp`, `quaternion.hpp`), otherwise `#error`. |
| `GLM_FORCE_RADIANS` | Harmless no-op | glm 1.0.3 does not reference it anywhere; radians have been the only mode since 0.9.6. Defining it changes nothing. It may be kept for readability or dropped. |
| `GLM_FORCE_INTRINSICS` | **No** (recommended) | Verified on MSVC 14.51 x64: it compiles warning-free at /W4 and does not change type layout (`sizeof(vec3)=12`, `sizeof(vec4)=16`, `alignof(vec4)=4`, `sizeof(mat4)=64`), **but** glm only takes its SIMD code paths for `aligned_*` qualifiers, so default `vec*/mat*/quat` math gets no speed-up. It also switches glm to anonymous-struct unions and to per-architecture (SSE2 vs. NEON on macOS arm64) code paths, i.e. more cross-platform numeric variance for no gain. |
| `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES` | **Never** | Makes the default types aligned: `vec3` becomes 16 bytes and `vec4/mat4` 16-byte aligned. Breaks tightly packed vertex structs, glTF accessor copies, GPU buffer layouts and serialization. |
| `GLM_FORCE_CTOR_INIT` | Optional (lead decision) | Default constructors leave glm types uninitialized (`glm::vec3 v;` is garbage). With this define they zero/identity-initialize. Alternative: always write `glm::vec3 v{ 0.0f }` / `glm::mat4(1.0f)` and enforce it in code review. |

Notes:

- Projection matrices are right-handed (default). Vulkan's clip-space Y points down; flip it in the projection (`proj[1][1] *= -1.0f`) or use a negative-height viewport - pick one in the renderer and document it.
- `glm::decompose` (gtx) round-trips T * R * S correctly in 1.0.3 (verified, including orientation up to sign).
- glm matrices are column-major; `glm::value_ptr(m)[12..14]` is the translation (verified).

## Warnings (MSVC 14.51, C++23, /W4 /permissive- /Zc:preprocessor /utf-8)

- **No warnings** from glm headers, with or without `GLM_FORCE_INTRINSICS`, in Debug/Release/Dist, even when included as an ordinary (non-external) include directory.
- clang 22 (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`) preview: no warnings.
- Recommended anyway: add `Vendor/glm` via premake `externalincludedirs` (MSVC `/external:I` + `externalwarnings "Off"`, GCC/Clang `-isystem`) like all vendor code.

## Verification performed

Throwaway workspace (outside the repo) built Debug/Release/Dist with VS 2026 (v145) as C++23: perspective (ZO depth), lookAt, quaternion rotate/slerp, TRS compose + `glm::decompose`, `value_ptr` layout - all passed.

## Updating

1. `git ls-remote --tags https://github.com/g-truc/glm.git` and pick the newest release tag (check the GitHub releases page that it is not a pre-release).
2. `git clone --depth 1 --branch <tag> https://github.com/g-truc/glm.git <tmp>`
3. Delete `Vendor/glm/glm/` and copy `<tmp>/glm/**/*.{h,hpp,inl}` back preserving paths (skip `glm.cppm`, `detail/glm.cpp`, `CMakeLists.txt`); copy `copying.txt`.
4. Make sure files have LF line endings (clone with `-c core.autocrlf=false`).
5. Update this file (tag, SHA, date, file count), rebuild all configurations and run the unit tests.
