# Dear ImGui (docking branch)

| Field           | Value |
|-----------------|-------|
| Upstream        | https://github.com/ocornut/imgui |
| Version / tag   | `v1.92.9b-docking` (`IMGUI_VERSION "1.92.9b"`, `IMGUI_VERSION_NUM 19291`) |
| Commit          | `b48d1afbe8ee8b238e2961dc363a949dd7304e23` ("Merge branch 'master' into docking", 2026-07-31) |
| Tag object      | `9acdfbf46810c0c74ab281ce04122c4149ae8bd1` (annotated tag) |
| Date vendored   | 2026-10-05 |
| License         | MIT (`LICENSE.txt`) |
| Premake project | `ImGui` (StaticLib, C++, `cppdialect "C++11"` = upstream minimum) |

The docking branch is required: it defines `IMGUI_HAS_DOCK` and `IMGUI_HAS_VIEWPORT` (dock spaces and
multi-viewport support used by the editor). Upstream publishes a `-docking` tag for every release; always
take the docking variant of the latest release tag.

## What is kept

```
LICENSE.txt
imconfig.h  imgui.h  imgui_internal.h  imstb_rectpack.h  imstb_textedit.h  imstb_truetype.h
imgui.cpp  imgui_demo.cpp  imgui_draw.cpp  imgui_tables.cpp  imgui_widgets.cpp
misc/cpp/imgui_stdlib.h  misc/cpp/imgui_stdlib.cpp       (ImGui::InputText & co. for std::string)
backends/imgui_impl_glfw.h  backends/imgui_impl_glfw.cpp (vendored, NOT compiled by this project)
premake5.lua  VENDOR.md                                   (local, not upstream)
```

The upstream directory structure is preserved, so `#include "misc/cpp/imgui_stdlib.h"` and
`#include "backends/imgui_impl_glfw.h"` work with the single include root `Vendor/imgui`.

## What is removed

`docs/`, `examples/`, `.github/`, `.editorconfig`, `.gitattributes`, `.gitignore`, every backend other than
GLFW (DX9-12, Metal, OpenGL, SDL, Vulkan, WebGPU, Win32, OSX, Android, Allegro, GLUT, null, ...),
`misc/fonts/` (TTF files + `binary_to_compressed_c.cpp`), `misc/freetype/`, `misc/single_file/`,
`misc/debuggers/` (natvis/gdb/lldb helpers), all README files.

The upstream Vulkan backend is intentionally not vendored: the engine renders ImGui draw data through its own
NVRHI renderer.

## Local modifications

None. Files are byte-identical to the upstream commit. (Upstream stores these files with CRLF line endings;
the engine repository's `.gitattributes` (`* text=auto eol=lf`) normalizes them to LF on commit. Only line
endings change, not content.)

## Consumer usage

```lua
includedirs { "%{wks.location}/Vendor/imgui" }
links { "ImGui" }
```

- **Required defines:** none. The library is built with upstream defaults (unmodified `imconfig.h`).
- **Optional configuration defines** (`IMGUI_DISABLE_OBSOLETE_FUNCTIONS`, `IMGUI_USER_CONFIG`,
  `ImTextureID`/`ImDrawIdx` overrides, ...) change declarations and/or struct layout. Any such define **must**
  be added identically to the `ImGui` project, the `ImGuizmo` project and every consumer (Engine, Editor,
  Tests), for example through the root workspace or a shared Lua table. Mismatches cause ODR violations.
  `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` was verified to build cleanly for both `ImGui` and `ImGuizmo` at this
  version. Note that it removes the legacy font atlas path (`ImFontAtlas::GetTexDataAsRGBA32`/`SetTexID`).
- Defaults that matter for the renderer: `ImTextureID` is `ImU64` (enough to store a pointer or a
  bindless/descriptor handle), `ImDrawIdx` is 16-bit `unsigned short` (set
  `ImGuiBackendFlags_RendererHasVtxOffset` in the renderer so large meshes are split by `VtxOffset`).
- **Texture API (1.92+):** a new renderer should set `ImGuiBackendFlags_RendererHasTextures` and service
  `ImDrawData::Textures` each frame (`ImTextureStatus_WantCreate` / `WantUpdates` / `WantDestroy`, then
  `ImTextureData::SetTexID()` + `SetStatus()`), and on shutdown destroy every texture in
  `ImGui::GetPlatformIO().Textures` whose `RefCount == 1`. See upstream `docs/BACKENDS.md`. Fonts are then
  rasterized on demand; do not call `ImFontAtlas::Build()`/`GetTexDataAsRGBA32()` in that mode.
- `io.IniFilename`: the editor should point this at a project/user path (or `nullptr`); the default
  `imgui.ini` in the working directory is ignored by the repository's `.gitignore`.

### GLFW platform backend (`backends/imgui_impl_glfw.cpp`)

Not part of the `ImGui` project. The engine compiles it in its own translation unit,
`Engine/Source/Engine/ImGui/ImGuiGlfwImplementation.cpp`, which includes `<vulkan/vulkan.h>` and then
`backends/imgui_impl_glfw.cpp`, excluded from the engine PCH with `enablepch "Off"` (premake 5.0.0 has no `NoPCH`
flag), with include dirs `Vendor/imgui` and `Vendor/GLFW/include` (`Docs/Decisions/0009-m5-decisions.md` decision 15).

- It needs a native window (`glfwGetWin32Window`, `glfwGetCocoaWindow`, X11): on GLFW's null platform
  `ImGui_ImplGlfw_InitForVulkan` logs a GLFW error and, on Windows, asserts on the missing window procedure. The engine
  therefore uses it on Win32, Cocoa and X11 only; headless, `ImGuiLayer` is the platform side itself
  (`Docs/Decisions/0009-m5-decisions.md`, integration decision 24).

- Use `ImGui_ImplGlfw_InitForVulkan(window, true)` (or `InitForOther`), `ImGui_ImplGlfw_NewFrame()`,
  `ImGui_ImplGlfw_Shutdown()`.
- Verified: compiles warning-free at `/W4`, `cppdialect "C++23"`, MSVC 14.51, against the vendored GLFW
  3.5.1 headers.
- It defines `GLFW_EXPOSE_NATIVE_WIN32` / `GLFW_EXPOSE_NATIVE_COCOA` / `GLFW_EXPOSE_NATIVE_X11` itself
  (and includes `<GLFW/glfw3native.h>`); consumers need not define them.
- Linux: X11 and Wayland support are auto-detected with `__has_include(<X11/Xlib.h>)`,
  `<X11/extensions/Xrandr.h>` and `<wayland-client.h>` (the same dev packages GLFW itself needs). Force-off
  with `IMGUI_IMPL_GLFW_DISABLE_X11` / `IMGUI_IMPL_GLFW_DISABLE_WAYLAND` on that one file if required. The X11
  path uses `dlopen()`, which is in libc on glibc >= 2.34 (Ubuntu 24.04 ships 2.39), so no `-ldl` is needed.
- macOS: compiles as plain C++ (no Objective-C needed); frameworks come from the GLFW link requirements.

## Link requirements

| Platform | Requirements |
|----------|--------------|
| Windows  | `user32`, `kernel32` (clipboard), `shell32` (`OpenInShell`), `imm32` (IME). All are pulled in automatically by `#pragma comment(lib, ...)` inside `imgui.cpp` under MSVC; nothing to add. |
| Linux    | None for the core library (`pic "On"` is set so it can go into shared objects). |
| macOS    | None for the core library (the optional Carbon clipboard, `IMGUI_ENABLE_OSX_DEFAULT_CLIPBOARD_FUNCTIONS`, is not enabled; the GLFW backend provides clipboard support). |

## Verification (2026-10-05)

Throwaway workspace (outside the repository) including this `premake5.lua` and `Vendor/ImGuizmo/premake5.lua`,
generated with premake 5.0.0 `vs2026`, built with MSBuild (VS 2026, v145) in Debug, Release and Dist with no
warnings. A C++23 smoke executable linked `ImGui` + `ImGuizmo` and, in every configuration:
created a context, built the font atlas with the legacy `GetTexDataAsRGBA32` path and, in a second context,
serviced the 1.92 `RendererHasTextures` texture requests; ran several frames with `DockSpaceOverViewport` +
`SetNextWindowDockID` (window reported docked), `ImGui::InputText(std::string*)` from `imgui_stdlib`,
`ImGuizmo::BeginFrame` + `ImGuizmo::Manipulate` with identity matrices; called `ImGui::Render` (294 vertices
in the draw data, 100 of them from the gizmo) and destroyed the contexts cleanly.

## Updating

1. Find the newest release tag: `git ls-remote --tags https://github.com/ocornut/imgui` and take the newest
   `vX.Y.Z-docking` (a `b`/`c` suffix denotes a hotfix release of the same version).
2. `git -c core.autocrlf=false clone --depth 1 --branch vX.Y.Z-docking https://github.com/ocornut/imgui <tmp>`
3. Replace exactly the files listed under "What is kept" (keep `premake5.lua` and `VENDOR.md`). Check whether
   upstream added or renamed core `.cpp`/`.h` files (compare the root directory listing) and update the
   `files` list.
4. Read the "API BREAKING CHANGES" section at the top of `imgui.cpp` and the changelog in `docs/CHANGELOG.txt`
   (upstream) for renderer/backend changes, and check `backends/imgui_impl_glfw.cpp` for new GLFW
   requirements.
5. Re-verify ImGuizmo against the new version (see `Vendor/ImGuizmo/VENDOR.md`); bump it if needed.
6. Update the version, commit SHA and date in this file, rebuild all configurations and run the editor and
   unit tests.
