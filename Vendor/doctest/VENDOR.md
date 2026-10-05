# doctest

| | |
|---|---|
| Upstream | https://github.com/doctest/doctest |
| Version | 2.5.3 (tag `v2.5.3`, latest stable release) |
| Commit | `2d0a9359a60c51affe2a9bebb1be1dca47868151` (2026-07-07) |
| Date vendored | 2026-10-05 |
| License | MIT (SPDX: `MIT`). Full text: `LICENSE.txt`. |
| Kind | Header-only (no premake project). Used only by the `Tests` executable. |

## Contents

Kept (byte-identical to upstream at the commit above, LF line endings):

- `doctest/doctest.h` - the amalgamated single header (declarations + implementation)
- `LICENSE.txt`

Removed:

- `doctest/parts/` (the un-amalgamated sources `doctest.h` is generated from), `doctest/doctest.cpp` (tiny helper TU that only defines `DOCTEST_CONFIG_IMPLEMENT`), `doctest/extensions/` (MPI reporter and utilities)
- `examples/`, `tests/`, `doc/`, `scripts/`, `CMakeLists.txt`, `meson.build`, `doctest.pc.in`, `*.bazel*`, `BUILD.bazel`, `MODULE.bazel`, `WORKSPACE.bazel`, `CHANGELOG.md`, `CONTRIBUTING.md`, `README.md`, `.clang-format`, `.clang-tidy`, `.editorconfig`, `.pre-commit-config.yaml`, `.github/`

Local modifications: **none**.

## Consumer usage

- Include directory: `Vendor/doctest` (external include dir recommended for consistency, although doctest is warning-clean)
- Include as: `#include <doctest/doctest.h>`
- Exactly **one** TU of the test executable defines the implementation before including the header:
  - `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` - doctest supplies `main()`, or
  - `#define DOCTEST_CONFIG_IMPLEMENT` + a custom `main()` that creates a `doctest::Context`, applies command-line options and calls `context.run()` - recommended for the engine's `Tests` project so the runner can initialize engine logging, set default options (e.g. `--no-breaks` on CI) and return `context.run()` as the exit code.
- All other test TUs just `#include <doctest/doctest.h>`.
- Link requirements: none on any platform.

### Defines

None required. Optional (must be identical in every TU of the test executable):

| Define | Notes |
|---|---|
| `DOCTEST_CONFIG_SUPER_FAST_ASSERTS` | Faster-compiling assert macros (no try/catch per assert); only matters for very large suites. |
| `DOCTEST_CONFIG_NO_SHORT_MACRO_NAMES` | Only if `CHECK`/`REQUIRE` collide with other macros (they do not in this engine today). |
| `DOCTEST_CONFIG_DISABLE` | Strips tests compiled into production code. Not needed: tests live in the separate `Tests` executable. |

## Warnings (MSVC 14.51, C++23, /W4 /permissive- /Zc:preprocessor /utf-8)

- **No warnings**: the doctest smoke executable was built with doctest as an *ordinary* include dir at /W4 **and /WX** in Debug/Release/Dist.
- clang 22 (`-Wall -Wextra -Wpedantic`) preview: no warnings.

## Verification performed

Throwaway workspace: C++23 console app with `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and 2 test cases (plain `CHECK`s, `REQUIRE`, `SUBCASE`s, `CHECK_FALSE`, `doctest::Approx`), built and run in Debug/Release/Dist: `2 passed | 0 failed`, 12/12 assertions, exit code 0.

## Updating

1. `git ls-remote --tags https://github.com/doctest/doctest.git`, pick the highest release tag by version number (confirm on the releases page that it is not a pre-release).
2. `git clone --depth 1 --branch <tag> https://github.com/doctest/doctest.git <tmp>`
3. Replace `doctest/doctest.h` and `LICENSE.txt`.
4. Ensure LF line endings (clone with `-c core.autocrlf=false`).
5. Update this file (tag, SHA, date), rebuild the `Tests` project in all configurations and run it.
