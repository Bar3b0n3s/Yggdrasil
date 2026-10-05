# EnTT

| | |
|---|---|
| Upstream | https://github.com/skypjack/entt |
| Version | v4.0.0 (tag `v4.0.0`, latest stable release, marked "Latest" on GitHub) |
| Commit | `85c6bba014049b5de8fad49d25424df2f1f6a8c1` (2026-07-22) |
| Date vendored | 2026-10-05 |
| License | MIT (SPDX: `MIT`). Full text: `LICENSE`. |
| Kind | Header-only (no premake project). Requires C++20 or newer (engine uses C++23). |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings):

- `src/entt/**` - every `*.hpp` and `src/entt/config/*.h` (98 headers: config, container, core, entity, graph, locator, meta, poly, process, resource, signal, stl, tools, `entt.hpp`, `fwd.hpp`, `tools.hpp`)
- `LICENSE`

Removed:

- `src/entt/natvis/*.natvis` (Visual Studio debugger visualizers - optional; can be vendored later and added to the Engine project with `files { ".../natvis/*.natvis" }` if wanted)
- `src/BUILD.bazel`, `single_include/` (generated 3.6 MB amalgamation of the same code), `test/`, `testbed/`, `docs/`, `build/`, `cmake/`, `conan/`, `conanfile.py`, `scripts/`, `bazel/`, `*.bazel*`, `CMakeLists.txt`, `AUTHORS`, `CONTRIBUTING.md`, `README.md`, `TODO`, `entt.imp`, `.clang-format`, `.clang-tidy`, `.github/`

Local modifications: **none**.

### Why `src/` and not `single_include/`

Same code, but the modular headers let engine headers include only what they need (`<entt/entity/fwd.hpp>` in public headers, `<entt/entity/registry.hpp>` where the registry is used) instead of a 3.6 MB single header in every TU, and keep the upstream layout for easy updates. `#include <entt/entt.hpp>` still works and pulls in everything.

## Consumer usage

- Include directory: `Vendor/entt/src` (add as an *external* include dir, see Warnings)
- Include as: `#include <entt/entt.hpp>` or finer-grained, e.g. `<entt/entity/registry.hpp>`, `<entt/entity/fwd.hpp>`, `<entt/core/hashed_string.hpp>`, `<entt/signal/sigh.hpp>`
- Link requirements: none on any platform (header-only).

### Defines (optional; if used they must be identical in every TU - set them in premake, not in source files)

No define is required. Relevant knobs (see upstream `docs/md/config.md`):

| Define | Notes |
|---|---|
| `ENTT_ASSERT(condition, msg)` | Default is `assert(((condition) && (msg)))`, i.e. active unless `NDEBUG` is defined. Premake's `optimize` does **not** define `NDEBUG`, so EnTT asserts stay on in Release (matches the "asserts on in Release" policy) and must be turned off for Dist by defining `NDEBUG` or `ENTT_DISABLE_ASSERT` there. To route EnTT asserts into `ENGINE_ASSERT`, EnTT supports a user header `<entt/ext/config.h>` found via `__has_include` - the engine could provide one in its own include path instead of a command-line define. |
| `ENTT_DISABLE_ASSERT` | Turns all EnTT asserts off (takes precedence over a redefined `ENTT_ASSERT`). |
| `ENTT_ID_TYPE` | Entity identifier type, default `std::uint32_t`. Keep default unless more than ~1M live entities/versions are needed. |
| `ENTT_USE_ATOMIC` | Atomic type-index counters; only needed if types are registered concurrently from several threads. |
| `ENTT_NO_EXCEPTION` | Builds without exceptions. |

MSVC: EnTT emits `#pragma detect_mismatch` for its version, exception mode, id type and "nonstd" mode, so inconsistent configuration across TUs/libraries is a link error rather than silent ODR breakage.

### Important for serialization

`entt::type_id<T>()` / `entt::type_hash<T>` values are derived from `__FUNCSIG__` / `__PRETTY_FUNCTION__`, so they **differ between compilers** (MSVC vs. GCC/Clang). Never write them to scene/project files; serialize components under explicit, engine-owned names (an `entt::hashed_string` of an explicit string is stable: it is FNV-1a of the string).

## Warnings (MSVC 14.51, C++23, /W4 /permissive- /Zc:preprocessor /utf-8)

- **No warnings** from EnTT headers in Debug/Release/Dist, even as an ordinary (non-external) include directory.
- clang 22 (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`) preview: no warnings.
- Recommended anyway: add via premake `externalincludedirs` like all vendor code.

## Verification performed

Throwaway workspace built Debug/Release/Dist (VS 2026 v145, C++23): `"Transform"_hs` / `entt::hashed_string` compile-time vs runtime, registry create/emplace/`on_construct` signal, `view<A, const B>().each`, `view<A>(entt::exclude<B>)`, `try_get`, destroy + `valid`, entity recycling, `clear` - all passed.

## Updating

1. `git ls-remote --tags https://github.com/skypjack/entt.git`, pick the newest release tag (confirm on the releases page it is not a pre-release; read the release notes - v4 had breaking changes vs v3).
2. `git clone --depth 1 --branch <tag> https://github.com/skypjack/entt.git <tmp>`
3. Delete `Vendor/entt/src/` and copy `<tmp>/src/entt/**/*.{h,hpp}` back preserving paths; copy `LICENSE`.
4. Ensure LF line endings (clone with `-c core.autocrlf=false`).
5. Update this file (tag, SHA, date, header count), rebuild all configurations and run the unit tests.
