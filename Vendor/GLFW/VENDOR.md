# GLFW (vendored)

| | |
|---|---|
| Upstream | https://github.com/glfw/glfw |
| Version | 3.5.1 (tag `3.5.1`, latest stable 3.x release at time of vendoring) |
| Commit | `d9d6f0f1f967807ffade6598ea9a631ebaf37a56` (annotated tag object `70a9bb3881fe80fd483236e2b203cb451c6ecf40`, commit dated 2026-07-30) |
| Date vendored | 2026-10-05 |
| License | Zlib (SPDX: `Zlib`), see `LICENSE.md` |
| Premake project | `GLFW` (`kind "StaticLib"`, `language "C"`, `cdialect "C99"`), defined in `premake5.lua` |

## What was kept

Every kept file is byte-identical to upstream at the commit above (LF line endings, verified by comparing git blob hashes).

- `LICENSE.md`
- `include/GLFW/glfw3.h`, `include/GLFW/glfw3native.h`: the public headers
- `src/`: all library sources for the backends we build:
  - Shared: `context.c init.c input.c monitor.c platform.c vulkan.c window.c egl_context.c osmesa_context.c internal.h platform.h mappings.h`
  - Null backend, built on every platform: `null_*.c/h`
  - Windows: `win32_*.c/h wgl_context.c`
  - Linux (X11): `x11_*.c/h xkb_unicode.c glx_context.c linux_joystick.c/h posix_poll.c/h posix_time.c/h posix_thread.c/h posix_module.c`
  - macOS (Cocoa): `cocoa_*.m/h nsgl_context.m macos_time.c/h` plus the shared `posix_thread.c/h posix_module.c`

Note: upstream 3.5 renamed `cocoa_time.c` to `macos_time.c`. The file list follows upstream `src/CMakeLists.txt` at this tag.

## What was removed

- CMake build system: `CMakeLists.txt`, `src/CMakeLists.txt`, `CMake/` (toolchains, pkg-config/config templates, `GenerateMappings.cmake`)
- `docs/`, `examples/`, `tests/`, `README.md`, `CONTRIBUTORS.md`
- `deps/`: glad, tinycthread, getopt, nuklear, linmath, stb_image_write. Only the examples and tests use these. `deps/wayland/*.xml` was removed with the Wayland backend.
- Wayland backend: `src/wl_init.c src/wl_monitor.c src/wl_window.c src/wl_platform.h`. This is out of scope for now; see below.
- `src/glfw.rc.in`: the version resource, used only for DLL builds
- `src/mappings.h.in`: the template for upstream's `update_mappings` CMake target. The generated `src/mappings.h` is kept.
- CI and repository files: `.github/`, `.appveyor.yml`, `.editorconfig`, `.gitattributes`, `.gitignore`, `.mailmap`

## Local modifications

None.

## Build configuration (mirrors upstream `src/CMakeLists.txt`, static library)

| Platform | Backend define | Other private defines | Notes |
|---|---|---|---|
| All | (null backend always on) | none | `cdialect "C99"` = upstream `C_STANDARD 99`, `C_EXTENSIONS OFF`. CMake also applies this to Objective-C sources through its `OBJC_STANDARD` fallback, and premake does the same (`-std=c99 -x objective-c`, Xcode `GCC_C_LANGUAGE_STANDARD = c99`). |
| Windows | `_GLFW_WIN32` | `UNICODE`, `_UNICODE`, `_CRT_SECURE_NO_WARNINGS` | `systemversion "latest"`. `_GLFW_USE_HYBRID_HPG` is off, as it is by default upstream. Vulkan picks the physical device explicitly, so the Optimus/PowerXpress exports are not needed. |
| Linux | `_GLFW_X11` | `_DEFAULT_SOURCE` | `pic "On"`. Upstream adds `_DEFAULT_SOURCE` because `-std=c99` hides POSIX 2008 APIs on glibc. |
| macOS | `_GLFW_COCOA` | none | `.m` files are compiled as Objective-C (`compileas "Objective-C"`). They are non-ARC, matching upstream; do not enable `-fobjc-arc` for this project. |

The library is built with the GLFW defaults: no `_GLFW_BUILD_DLL`, no custom `_GLFW_*_LIBRARY` overrides. These are all private: consumers must **not** define any `_GLFW_*` macro.

## Consumer usage

- Include path: `Vendor/GLFW/include`. In the root premake this is `%{wks.location}/Vendor/GLFW/include`.
- Include as `#include <GLFW/glfw3.h>`.
- Required public defines: **none**. Do **not** define `GLFW_DLL`, because this is a static library.
- Recommended: define `GLFW_INCLUDE_NONE` project-wide. The engine renders through NVRHI and Vulkan and must not pull in OpenGL headers.
  - To get the Vulkan-typed helpers (`glfwCreateWindowSurface`, `glfwGetInstanceProcAddress`, `glfwGetPhysicalDevicePresentationSupport`, `glfwInitVulkanLoader`), include the Vulkan headers before `glfw3.h`, or define `GLFW_INCLUDE_VULKAN` in that translation unit. Either way, the Vulkan headers must be on the include path (e.g. `$(VULKAN_SDK)/Include`). `glfwVulkanSupported()` and `glfwGetRequiredInstanceExtensions()` need no Vulkan headers.
  - For native handles, include `<GLFW/glfw3native.h>` after defining `GLFW_EXPOSE_NATIVE_WIN32`, `GLFW_EXPOSE_NATIVE_X11` or `GLFW_EXPOSE_NATIVE_COCOA` for the current platform.
- Link: `links { "GLFW" }` plus the system libraries below. Upstream links these privately; with a static library the final executable must link them.

### Link requirements per platform

| Platform | premake `links` in the consuming executable | Loaded at runtime with `LoadLibrary`/`dlopen` (no link needed) |
|---|---|---|
| Windows | `"gdi32", "user32", "shell32"` (MSVC links these by default, but list them for non-VS generators) | `user32.dll` extras, `dinput8.dll`, `xinput1_4/1_3/9_1_0/1_2/1_1.dll`, `dwmapi.dll`, `shcore.dll`, `ntdll.dll`, `opengl32.dll`, `vulkan-1.dll` |
| Linux | `"pthread", "dl", "m", "rt"` (upstream: `Threads::Threads`, `rt`, `m`, `${CMAKE_DL_LIBS}`) | `libX11.so.6`, `libXrandr.so.2`, `libXinerama.so.1`, `libXcursor.so.1`, `libXi.so.6`, `libXext.so.6`, `libXrender.so.1`, `libX11-xcb.so.1`, `libXxf86vm.so.1`, `libGL.so.1`/`libGLX.so.0`, `libEGL.so.1`, `libOSMesa.so.8`, `libvulkan.so.1` |
| macOS | `"Cocoa.framework", "IOKit.framework", "CoreFoundation.framework", "QuartzCore.framework"` | `libvulkan.1.dylib`: first from the default dyld search path, then from the app bundle's `Contents/Frameworks/libvulkan.1.dylib`. Exported macOS games should ship the Vulkan loader and MoltenVK/KosmicKrisp in the bundle's Frameworks folder, or the engine can pass its own loader with `glfwInitVulkanLoader(vkGetInstanceProcAddr)` before `glfwInit()`. |

### Build-time system packages

- Windows: Windows SDK only (installed with Visual Studio).
- Linux (Ubuntu 24.04+): X11 development headers, `sudo apt install xorg-dev`. The minimal set is `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev`.
- macOS: Xcode or the Command Line Tools (macOS SDK).

## Wayland (not built)

Only the X11 backend is built on Linux. Ubuntu 24.04 runs Wayland sessions with XWayland enabled, so X11 GLFW applications work there unchanged. The `GLFW_PLATFORM` init hint can still select `GLFW_PLATFORM_X11` or `GLFW_PLATFORM_NULL` explicitly. To add Wayland later:

1. Copy `src/wl_init.c`, `src/wl_monitor.c`, `src/wl_window.c`, `src/wl_platform.h` and `deps/wayland/*.xml` from the same upstream tag.
2. Generate the protocol headers with `wayland-scanner client-header <xml> <name>-client-protocol.h` and `wayland-scanner private-code <xml> <name>-client-protocol-code.h` for each of the 9 XML files, as upstream's `generate_wayland_protocol` does (pre-build step or a script in `Scripts/`). Put the output directory on the include path.
3. Add `_GLFW_WAYLAND` (and `HAVE_MEMFD_CREATE` when glibc provides `memfd_create`), plus the pkg-config include dirs of `wayland-client wayland-cursor wayland-egl xkbcommon` (`sudo apt install libwayland-dev libxkbcommon-dev wayland-protocols`).

## Verification performed

- Windows 11, Visual Studio 2026 (v145, MSVC 14.51), premake 5.0.0 `vs2026`: built a throwaway workspace (`GLFW` + a C++23 `Smoke` console app) in Debug, Release and Dist. GLFW compiled with zero warnings at the default level. Smoke ran in every configuration with no hints set: `glfwInit()`, `glfwGetPlatform()` returned Win32, `glfwVulkanSupported()` returned true (`VK_KHR_surface`, `VK_KHR_win32_surface`), and `glfwGetVersionString()` returned `3.5.1 Win32 WGL Null EGL OSMesa VisualC`. It then called `glfwTerminate()`.
- Linux and macOS cannot be built on the development machine. The `gmake` (`--os=linux`, `--os=macosx`) and `xcode4` outputs were generated and checked:
  - Linux: 23 objects, `-D_GLFW_X11 -D_DEFAULT_SOURCE -fPIC -std=c99`.
  - macOS: 21 objects, `-D_GLFW_COCOA`, `.m` files compiled with `-x objective-c`.
  - Both object lists match upstream `src/CMakeLists.txt` exactly, and every file listed in `premake5.lua` exists.

## Update instructions

1. Find the newest stable tag: `git ls-remote --tags https://github.com/glfw/glfw`.
2. `git -c core.autocrlf=false clone --depth 1 --branch <tag> https://github.com/glfw/glfw <scratch>/src`. Disabling autocrlf keeps LF line endings identical to upstream.
3. Replace `include/GLFW/*.h`, `LICENSE.md` and the files in `src/`, excluding `wl_*`, `CMakeLists.txt`, `glfw.rc.in` and `mappings.h.in` as described above.
4. Diff the new `src/CMakeLists.txt` against the file lists and defines in `premake5.lua`, per platform. Watch for added, removed or renamed sources and new compile definitions or link libraries. Update `premake5.lua`.
5. Rebuild Debug, Release and Dist, run the engine's tests, and update this file (version, commit, date, notes).
