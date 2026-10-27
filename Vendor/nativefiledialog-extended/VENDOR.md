# nativefiledialog-extended

| | |
|---|---|
| Upstream | https://github.com/btzy/nativefiledialog-extended |
| Version | 1.4.1 (tag `v1.4.1`, the latest release when vendored; 1.4.0 added custom dialog titles, which `FileDialogRequest::Title` needs) |
| Commit | `65b2f070a0fcfee0d453a64e770a217ab0b6ff4f` ("Release: v1.4.1", 2026-10-01; annotated tag object `73ed723be61cfc5a94f7d16112541fd2ac39eee4`) |
| Date vendored | 2026-10-20 (M18 W18.1 stream G; approved in `Docs/Decisions/0001-approvals.md`, pinned by `Docs/Decisions/0029-m18-contract.md` decision 21) |
| License | zlib (SPDX: `Zlib`), see `LICENSE` |
| Kind | The public C header and the Windows backend (C++); static library, premake project `nfd` (`premake5.lua`), the name of upstream's CMake target |
| Release archive | `https://github.com/btzy/nativefiledialog-extended/archive/refs/tags/v1.4.1.tar.gz`, 438,898 bytes, SHA-256 `4a01eafa921169f009f3e83bea8e287d9e5d0b94ca9064cf7f0a546652d7c884` |

## Contents

Kept, byte-identical to upstream at the commit above (LF line endings as stored upstream), at their upstream paths:

| File | SHA-256 |
|---|---|
| `LICENSE` | `b332d6f507142c66a0e904838b6437e6273734589c22a16b57392b89bfdccb2a` |
| `README.md` | `5568653f492b3d3446ef77b6670b4e14f2edb57c6fd6135d5c7d188407b0ff9d` |
| `src/include/nfd.h` | `61a1b7e2f09b307328a354784b22b0cbcc3dd9fe8ee89c2b4ecc68025a393709` |
| `src/nfd_win.cpp` | `0ba722262268398f39cd037e938aa61917a729719b191b01d5e26097d20b0765` |

Added: `premake5.lua`, this file.

Removed:

- **The backends that are never built** ("Windows backends only", `Docs/Decisions/0028-backlog-program.md` decision 10;
  the owner dropped the Linux and macOS dialogs, E-DIALOGS-LINUX and E-DIALOGS-MACOS): `src/nfd_gtk.cpp` (GTK 3),
  `src/nfd_portal.cpp` (xdg-desktop-portal over libdbus-1), `src/nfd_linux_shared.hpp` (shared by those two) and
  `src/nfd_cocoa.m` (AppKit). A later port adds them back from the same commit under the kept interface
  (`Engine/Platform/NativeFileDialog.h`).
- The headers the engine does not use: `src/include/nfd.hpp` (the C++ wrapper), `src/include/nfd_glfw3.h`,
  `src/include/nfd_sdl2.h` and `src/include/nfd_sdl3.h` (the glue that turns a framework's window into an
  `nfdwindowhandle_t`; the engine passes the HWND it gets from `glfwGetWin32Window` itself).
- The CMake build (`CMakeLists.txt`, `src/CMakeLists.txt`), `test/`, `screens/` (the README's screenshots), and the
  repository files `.clang-format`, `.github/`, `.gitignore`, `.gitmodules`.

Local modifications: **none**.

## What it is for

The editor's native file dialogs on Windows (E-DIALOGS; `Docs/Decisions/0029-m18-contract.md` decisions 19 to 21):
`Engine/Source/Engine/Platform/Windows/NativeFileDialogWindows.cpp`, the only first-party file that includes `nfd.h`
(`Scripts/ModuleRules.json` restricts it), shows `NFD_OpenDialogU8_With` and `NFD_PickFolderU8_With` dialogs, the COM
`IFileDialog` wrapper of `nfd_win.cpp`, for the editor's FileDialog API (`Editor/FileDialog.h`). On Linux and macOS nothing
of the library is compiled: `NativeFileDialog::Open` returns `ErrorCode::Unsupported` there
(`Platform/Posix/NativeFileDialogPosix.cpp`), and the FileDialog API shows its ImGui picker instead.

Each dialog runs on a thread of its own, which calls `NFD_Init` (a single-threaded COM apartment, which the main thread
cannot give once miniaudio's WASAPI backend has initialized COM there as multithreaded, `Docs/Decisions/0015-m12-decisions.md`),
the dialog function and `NFD_Quit`. The backend keeps its last error (`NFD_GetError`) and whether `NFD_Init` must
uninitialize COM in process globals, not per thread; every dialog thread initializes COM in a fresh apartment, so the flag
is the same for all of them, and the editor shows one dialog at a time (each is modal to the main window, which the system
disables while it shows).

## Build configuration (`premake5.lua`)

- The project is defined on every OS, so the portability stage's per-configuration project sets match the vs2026
  reference (`Scripts/CI.py`, `check_portability`). Every OS lists `src/include/nfd.h` (include directory `src/include`);
  only the Windows branch lists a backend source, `src/nfd_win.cpp`. The Linux and macOS branches list no backend source
  and link no system library: nothing compiles the library there (only Windows is verified, ADR 0028 decision 10).
- C++17 (upstream accepts 17 and newer); the compiler's default exceptions and RTTI (upstream's CMake turns both off
  for MSVC to save code size; the backend neither throws nor uses RTTI, so the default changes no behaviour).
- **No configuration macro**: upstream's defaults, with UTF-8 (`...U8`) and native (`...N`) entry points, no portal
  (`NFD_PORTAL` changes `nfdpathsetenum_t`'s layout on Linux only), no `NFD_SHARED`/`NFD_EXPORT` (static linkage) and no
  `NFD_OVERRIDE_RECENT_WITH_DEFAULT` (so a dialog may open in the folder the user last used rather than the default path,
  as Windows recommends). The backend defines `UNICODE` for its own translation unit only. The library therefore has no
  ABI-relevant define (`Scripts/CheckBuildConfig.py` compares only `NDEBUG` for it).
- Runtime library as every project (`staticruntime "off"`; Debug `runtime "Debug"`); Debug: `symbols "on"`; Release:
  `optimize "on"`, `symbols "on"`; Dist: `optimize "full"`, `symbols "off"`. Warnings: the compiler's default level.
- The vendor cache (`Scripts/Lib/vendor_cache.py`) caches it like every vendored static library.

## Consumer usage

- `UseNativeFileDialog()` in `Dependencies.lua`: `externalincludedirs { Vendor/nativefiledialog-extended/src/include }`
  for Engine, Editor and Tests, and, for the linking projects on Windows only, `links { "nfd", "ole32", "shell32", "uuid",
  "user32" }` (COM, `SHCreateItemFromParsingName`, the dialog's class and interface identifiers, and the window functions
  `NativeFileDialogWindows.cpp` closes a dialog with). No defines. The Runtime never calls it: no exported game references
  `NativeFileDialog`, so the linker never pulls the Engine object that calls the library into it, and the Dist map holds
  none of its symbols.
- Include as `#include <nfd.h>` (the header has its own `extern "C"` guard). Per dialog thread: `NFD_Init`, then
  `NFD_OpenDialogU8_With` or `NFD_PickFolderU8_With` with an `nfdopendialogu8args_t` or `nfdpickfolderu8args_t` (filter
  list, default path, the parent `{NFD_WINDOW_HANDLE_TYPE_WINDOWS, hwnd}`, title), `NFD_FreePathU8` on the returned path,
  `NFD_GetError` after `NFD_ERROR`, and `NFD_Quit` when `NFD_Init` returned `NFD_OKAY`. A filter's spec lists extensions
  without their dot, comma-separated (`eproj`, `luau,test.luau`); the backend always adds an "All files" filter after it.

## Verification performed

- Downloaded the release archive above under the product owner's standing permission (AGENTS.md "Approvals") and
  computed its SHA-256; its `pax_global_header` comment names commit `65b2f070a0fcfee0d453a64e770a217ab0b6ff4f`, and
  `git ls-remote https://github.com/btzy/nativefiledialog-extended.git refs/tags/v1.4.1*` resolves the annotated tag to
  that commit.
- Cloned the tag (`git clone -c core.autocrlf=false --depth 1 --branch v1.4.1`), checked that `HEAD` is that commit, and
  compared the kept files of the archive with the clone's: every SHA-256 above matches.
- Stream G builds the library in Debug and Release with MSVC 14.51 through `Scripts/Build.py`, runs
  `Scripts/CheckBuildConfig.py`, and runs the native layer's tests, among them the windowed child that shows a real dialog
  over a real window and closes it with `Cancel` (`Tests/Source/Engine/Platform/NativeFileDialogTests.cpp`).

## Updating

1. `git ls-remote https://github.com/btzy/nativefiledialog-extended.git "refs/tags/*"` and read the release notes.
2. Download the new release archive, record its size and SHA-256, check its `pax_global_header` commit, and clone the tag
   with `core.autocrlf=false` to compare the kept files.
3. Replace `LICENSE`, `README.md`, `src/include/nfd.h` and `src/nfd_win.cpp`; check whether `src/CMakeLists.txt` added a
   Windows source, a system library or a configuration macro, and mirror it in `premake5.lua` and `UseNativeFileDialog()`.
4. Update this file (version, commit, archive, hashes), rebuild, run `python Scripts/CheckBuildConfig.py` and the
   `NativeFileDialog` and `FileDialog` tests (the windowed child included).

## Notices

The row `Scripts/Lib/notices.py` reads for `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/0024-m15-contract.md decision 8).
Shipped is `yes` by the notices' mechanical rule: the generated Engine project references this directory (its include
path), so the library ships attributed, although no exported game links it (Projects is `-`: no project links it into the
Runtime), as cgltf's attribution over-approximates.

| Field | Value |
|---|---|
| Name | nativefiledialog-extended |
| Upstream | https://github.com/btzy/nativefiledialog-extended |
| Version | 1.4.1 65b2f070a0fcfee0d453a64e770a217ab0b6ff4f |
| License | Zlib |
| Shipped | yes |
| Projects | - |
| License files | LICENSE |
