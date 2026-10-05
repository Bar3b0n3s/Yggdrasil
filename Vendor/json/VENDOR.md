# nlohmann/json (JSON for Modern C++)

| | |
|---|---|
| Upstream | https://github.com/nlohmann/json |
| Version | 3.12.0 (tag `v3.12.0`, latest stable release) |
| Commit | `55f93686c01528224f448c19128836e7df245f72` (2025-04-11) |
| Date vendored | 2026-10-05 |
| License | MIT (SPDX: `MIT`). Full text: `LICENSE.MIT`. Every section of the amalgamated header carries `SPDX-License-Identifier: MIT` (copyright holders: Niels Lohmann; embedded portions by Evan Nemerson (Hedley), Florian Loitsch (Grisu2), Bjoern Hoehrmann (UTF-8 decoder), The Abseil Authors). |
| Kind | Header-only (no premake project) |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings):

- `single_include/nlohmann/json.hpp` - the amalgamated library
- `single_include/nlohmann/json_fwd.hpp` - forward declarations (use in engine headers to keep compile times down)
- `LICENSE.MIT`

Removed:

- `include/` (multi-file version of the same code), `nlohmann_json.natvis` (debugger visualizer, optional)
- `LICENSES/` (REUSE license texts that apply to upstream tests/tools/docs, not to the vendored header), `.reuse/`
- `tests/`, `docs/`, `tools/`, `cmake/`, `CMakeLists.txt`, `Makefile`, `meson.build`, `BUILD.bazel`, `MODULE.bazel`, `Package.swift`, `CITATION.cff`, `ChangeLog.md`, `FILES.md`, `README.md`, `.cirrus.yml`, `.clang-tidy`, `.github/`

Local modifications: **none**.

## Consumer usage

- Include directory: `Vendor/json/single_include` (add as an *external* include dir)
- Include as: `#include <nlohmann/json.hpp>` (or `<nlohmann/json_fwd.hpp>` in headers that only need the type names)
- Link requirements: none on any platform (header-only).

### Defines (if used they must be identical in every TU - set them in premake)

| Define | Recommendation |
|---|---|
| `JSON_USE_IMPLICIT_CONVERSIONS=0` | **Recommended.** Disables implicit `json -> T` conversions (they cause ambiguous-overload and silent-conversion bugs); values are read with `j.get<T>()` / `j.get_to(x)`. This will be the default in json 4.0. The smoke test was built and passed with it. |
| `JSON_DIAGNOSTICS=1` | Optional. Exceptions then include the JSON pointer of the offending element (much better scene/project file error messages) at the cost of a parent pointer per value. It is part of the library's ABI-tagged inline namespace, so mixing settings across TUs is link-safe, but keep it consistent per configuration. |
| `JSON_NOEXCEPTION` | Not recommended unless the engine is built without exceptions. For untrusted input use the non-throwing overload `nlohmann::json::parse(text, nullptr, false)` + `is_discarded()` (verified). |

Language detection (`JSON_HAS_CPP_20`, etc.) is automatic and uses `_MSVC_LANG` on MSVC, so it works with or without `/Zc:__cplusplus`.

Note: `json.hpp` contains one non-ASCII (UTF-8) character in a comment; compile with `/utf-8` on MSVC (premake: `buildoptions { "/utf-8" }`) to avoid C4819 on machines with a non-UTF-8 system code page.

## Warnings (MSVC 14.51, C++23, /W4 /permissive- /Zc:preprocessor /utf-8)

- **No warnings** from json headers in Debug/Release/Dist, even as an ordinary (non-external) include directory.
- clang 22 (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`) preview: no warnings.
- Recommended anyway: add via premake `externalincludedirs` like all vendor code.

## Verification performed

Throwaway workspace built Debug/Release/Dist (VS 2026 v145, C++23): parse (objects, arrays, `é` escapes, `uint64` max, null), `NLOHMANN_DEFINE_TYPE_INTRUSIVE` struct mapping both directions, `dump(4)` -> `parse` lossless round trip, non-throwing parse of invalid input, `parse_error` exception - all passed.

## Updating

1. `git ls-remote --tags https://github.com/nlohmann/json.git`, pick the newest release tag `vX.Y.Z`.
2. `git clone --depth 1 --branch <tag> https://github.com/nlohmann/json.git <tmp>`
3. Replace `single_include/nlohmann/json.hpp`, `single_include/nlohmann/json_fwd.hpp`, `LICENSE.MIT`.
4. Ensure LF line endings (clone with `-c core.autocrlf=false`); re-check SPDX identifiers in the header (`grep SPDX-License-Identifier`).
5. Update this file (tag, SHA, date), rebuild all configurations and run the unit tests.
