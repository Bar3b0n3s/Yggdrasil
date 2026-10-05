# spdlog (vendored)

| Field          | Value |
|----------------|-------|
| Upstream       | https://github.com/gabime/spdlog |
| Version / tag  | `v1.17.0` (latest stable 1.x release at vendoring time) |
| Commit         | `79524ddd08a4ec981b7fea76afd08ee05f83755d` (2026-01-04) |
| Date vendored  | 2026-10-05 |
| License        | `MIT` (see `LICENSE`) |
| Build          | Static library, premake project `spdlog` (`premake5.lua`), C++23 |
| Mode           | Compiled library (`SPDLOG_COMPILED_LIB`) formatting with `std::format` (`SPDLOG_USE_STD_FORMAT`) |

## What was kept / removed

Kept (byte-identical to upstream, upstream directory layout, LF line endings as stored upstream):

- `LICENSE`
- `include/spdlog/**` (all public headers, including every sink header for every platform)
- `src/spdlog.cpp`, `src/stdout_sinks.cpp`, `src/color_sinks.cpp`, `src/file_sinks.cpp`, `src/async.cpp`, `src/cfg.cpp`

Removed:

- `include/spdlog/fmt/bundled/` (bundled copy of the fmt library) and `src/bundled_fmtlib_format.cpp`.
  With `SPDLOG_USE_STD_FORMAT` neither is ever included or compiled (upstream's own CMake skips the
  source and its install rule excludes `fmt/bundled` in this mode). Removing them also turns a missing
  `SPDLOG_USE_STD_FORMAT` define in a consumer into a hard compile error
  (`Cannot open include file: 'spdlog/fmt/bundled/format.h'`) instead of a silent ODR/ABI mismatch
  between the library (std::format) and the consumer (fmt).
- `tests/`, `bench/`, `example/`, `cmake/`, `CMakeLists.txt`, `scripts/`, `logos/`, `.github/`,
  `appveyor.yml`, `README.md`, `INSTALL`, `.clang-format`, `.clang-tidy`, git metadata.

## Local modifications

None. `include/spdlog/tweakme.h` is unmodified; all configuration is done through defines.

## Build configuration (`premake5.lua`)

Mirrors upstream `CMakeLists.txt` with `SPDLOG_USE_STD_FORMAT=ON`, static library:

- Sources: the six `src/*.cpp` files listed above (identical on every platform; `color_sinks.cpp`
  selects `wincolor_sink` on Windows and `ansicolor_sink` elsewhere internally).
- Public defines (upstream `PUBLIC`): `SPDLOG_COMPILED_LIB`, `SPDLOG_USE_STD_FORMAT`.
- Private defines: `SPDLOG_FWRITE_UNLOCKED` on Windows (`_fwrite_nolock`) and Linux (glibc
  `fwrite_unlocked`). Not defined on macOS, whose libc has no `fwrite_unlocked` (upstream detects this
  with `check_symbol_exists`).
- MSVC options (`toolset:msc*`): `/utf-8` (upstream `PUBLIC`, `SPDLOG_MSVC_UTF8=ON` by default; two
  headers contain UTF-8 characters in comments) and `/Zc:__cplusplus` (upstream `PRIVATE`; makes the
  `__cplusplus` feature checks in the headers see C++23 instead of `199711L`).
- Linux: `pic "On"`.

## How consumers use it

Every project that includes spdlog headers (directly or through `Engine/Core/Log.h`, i.e. Engine,
Editor, Runtime, Tests) must use:

```lua
includedirs { "%{wks.location}/Vendor/spdlog/include" }   -- or externalincludedirs
defines { "SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT" }
links { "spdlog" }                                        -- in the project that links the final binary
filter "toolset:msc*"
    buildoptions { "/utf-8", "/Zc:__cplusplus" }          -- recommended workspace-wide
filter "system:linux"
    links { "pthread" }                                   -- upstream: Threads::Threads
filter {}
```

Consumers must compile with the same C++ dialect (C++23): with `SPDLOG_USE_STD_FORMAT`, the
`spdlog::format_string_t` type depends on the standard library's `__cpp_lib_format` level.

Link requirements per platform:

| Platform | Link |
|----------|------|
| Windows  | nothing beyond the CRT |
| Linux    | `pthread` (merged into glibc >= 2.34 but harmless and portable) |
| macOS    | nothing (libSystem). Deployment target must be macOS 13.3+ because Apple libc++ gates floating-point `std::to_chars`, which `std::format` uses, behind 13.3. |

std::format specifics (differences from the fmt-based spdlog):

- Format strings are checked at compile time (`std::format_string<Args...>`), so they must be
  constant expressions. For runtime format strings, format first and log the result:
  `logger->info(std::vformat(fmt, std::make_format_args(args...)));`
  (C++26 `std::runtime_format` is not available in C++23 mode.) Logging a plain string with no
  arguments (`logger->info(str)`) is always fine.
- Custom types are made loggable by specializing `std::formatter<T>` (not `fmt::formatter`).
  `spdlog/fmt/ostr.h`, `ranges.h`, `chrono.h`, `std.h`, `compile.h` and `xchar.h` are empty in this
  mode; `operator<<` is not picked up automatically.
- The `SPDLOG_ACTIVE_LEVEL` compile-time filter only affects the `SPDLOG_TRACE(...)`-style macros.

Verified on Windows (MSVC 14.51 / v145, C++23, Debug/Release/Dist, `/W4` consumer with no warnings):
colored stdout sink with a pattern, `std::format`-style arguments including a custom
`std::formatter` specialization, exact output comparison through an ostream sink, basic file sink,
registry, async logger and thread pool.

## Updating

1. Find the latest release: `git ls-remote --tags https://github.com/gabime/spdlog`.
2. `git clone --depth 1 --branch <tag> https://github.com/gabime/spdlog <tmp>` and export with the
   original line endings: `git -C <tmp> -c core.autocrlf=false archive HEAD | tar -x -C <tmp2>`.
3. Replace `LICENSE`, `include/` and the six `src/*.cpp` files; delete `include/spdlog/fmt/bundled/`.
4. Diff upstream `CMakeLists.txt` against the previous tag: check `SPDLOG_SRCS`, the definitions
   loop (`SPDLOG_USE_STD_FORMAT` etc.), the MSVC options and the `SPDLOG_FWRITE_UNLOCKED` check, and
   update `premake5.lua` to match.
5. Rebuild Debug/Release/Dist, run the engine's logging tests, and update this file (tag, commit, date).

To go back to the bundled fmt instead of `std::format`: restore `include/spdlog/fmt/bundled/` and
`src/bundled_fmtlib_format.cpp` from the same tag, add the source to `files`, and remove
`SPDLOG_USE_STD_FORMAT` from both this project and every consumer.
