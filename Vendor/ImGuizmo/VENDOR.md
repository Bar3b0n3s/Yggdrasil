# ImGuizmo

| Field           | Value |
|-----------------|-------|
| Upstream        | https://github.com/CedricGuillemet/ImGuizmo |
| Version         | `master` at commit `18cef5e031d8c6973d80284c67f60549fafd78c1` ("Fix SetRect Y max computation (#431)", 2026-08-08) |
| Date vendored   | 2026-10-05 |
| License         | MIT (`LICENSE`) |
| Premake project | `ImGuizmo` (StaticLib, C++, `cppdialect "C++17"` as in upstream `CMakeLists.txt`) |
| Built against   | Dear ImGui `v1.92.9b-docking` (`Vendor/imgui`, `IMGUI_VERSION_NUM 19291`) |

## Why a master commit and not a tag

The newest tag is `1.10` (commit `b796ac3`, 2026-05-11); the other tags (`1.9`, `1.83`) are older. `1.10`
already uses the ImGui 1.92.8+ `AddPolyline(..., thickness, flags)` argument order, so it would also compile
correctly against the vendored ImGui, but master has ten further commits with fixes that matter for editor use:

- #415 (bounds edges when an endpoint is off-screen/behind camera), #419 (disappearing axis while
  translating), #420 (`IsOver` for `SCALEU`), #426 / #429 (multi-view / multi-gizmo fixes), #427 (jitter,
  consistent camera ray), #431 (`SetRect` Y max computation).
- #413: `MOVETYPE` exposed in the public API, with tracking of the active/hovered gizmo handle (useful for
  editor undo grouping and hover feedback).
- #414 / #418: the `AddPolyline` call sites choose the argument order based on `IMGUI_VERSION_NUM`, so the
  code works with ImGui versions before and after 1.92.8.

The pinned commit compiles warning-free against the vendored Dear ImGui (docking) and passes the smoke test
below. Since 2026 upstream keeps its sources under `src/`; that layout is preserved here.

## What is kept

```
LICENSE
src/ImGuizmo.h
src/ImGuizmo.cpp
premake5.lua  VENDOR.md   (local, not upstream)
```

## What is removed

The other widgets (`src/GraphEditor.*`, `src/ImCurveEdit.*`, `src/ImGradient.*`, `src/ImSequencer.*`,
`src/ImVectorEditor.*`, `src/ImLightRig.h`, `src/ImZoomSlider.h`), `example/`, `vcpkg-example/`, `docs/`,
`images/`, `.github/`, `.gitignore`, `CMakeLists.txt`, `Makefile`, `README.md`.

## Local modifications

None. Files are byte-identical to the upstream commit.

## Consumer usage

```lua
includedirs { "%{wks.location}/Vendor/imgui", "%{wks.location}/Vendor/ImGuizmo/src" }
links { "ImGuizmo", "ImGui" }
```

- **Required defines:** none. Do not define `USE_IMGUI_API` or `IMGUIZMO_NAMESPACE` (defaults: no export
  macro, namespace `ImGuizmo`). Any Dear ImGui configuration define (see `Vendor/imgui/VENDOR.md`) must also be
  applied to this project.
- `ImGuizmo.h` does not include `imgui.h`; include `"imgui.h"` first.
- Per frame: `ImGuizmo::BeginFrame()` right after `ImGui::NewFrame()`; inside the viewport window call
  `ImGuizmo::SetDrawlist()` (window draw list), `ImGuizmo::SetRect(x, y, w, h)` (viewport image rect),
  `ImGuizmo::SetOrthographic(isOrtho)`, then `ImGuizmo::Manipulate(view, projection, op, mode, model, ...)`.
  Matrices are column-major `float[16]` (glm layout, `glm::value_ptr`).
- ImGuizmo keeps one global context. With more than one ImGui context, call
  `ImGuizmo::SetImGuiContext(ctx)` after switching.
- An identity (or any orthographic) projection requires `SetOrthographic(true)`; in perspective mode a gizmo
  whose clip-space z is < 0.001 (behind or at the camera) is not drawn.

### Known upstream issue (unmodified here)

In `ImGuizmo::Manipulate`, the perspective "behind camera" early return (`camSpacePosition.z < 0.001f` and not
currently dragging) returns before the `PopClipRect()` that matches the `PushClipRect()` at the top of the
function, leaving one extra clip rect on the target draw list (confirmed by the smoke test). The draw list is
reset next frame, so the effect is limited to clipping anything drawn later into the same draw list in the same
frame to the gizmo rect. Mitigations for the editor (no source patch needed): call `Manipulate` last in the
viewport window, or record `drawList->_ClipRectStack.Size` before the call and pop any surplus afterwards. If a
future upstream commit fixes it, the mitigation becomes a no-op.

## Link requirements

| Platform | Requirements |
|----------|--------------|
| Windows  | `ImGui` only. |
| Linux    | `ImGui` only (`pic "On"`). `alloca` comes from `<stdlib.h>`/`<alloca.h>` via glibc. |
| macOS    | `ImGui` only. |

## Verification (2026-10-05)

Built in the same throwaway workspace as Dear ImGui (premake 5.0.0 `vs2026`, MSBuild, VS 2026 v145) in Debug,
Release and Dist with no warnings, also with `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` defined. A C++23 smoke
executable called `ImGuizmo::SetImGuiContext`, `BeginFrame`, `SetDrawlist`, `SetRect`, `SetOrthographic`,
`Manipulate` (identity view/projection/model; 100 vertices emitted) and `DecomposeMatrixToComponents` in every
configuration.

## Updating

1. `git ls-remote --tags https://github.com/CedricGuillemet/ImGuizmo` and
   `git ls-remote https://github.com/CedricGuillemet/ImGuizmo HEAD`. Prefer a tag only if it is not behind
   master on fixes relevant to the vendored Dear ImGui version.
2. Clone (`git -c core.autocrlf=false clone https://github.com/CedricGuillemet/ImGuizmo <tmp>`, check out the
   chosen commit) and replace `LICENSE`, `src/ImGuizmo.h`, `src/ImGuizmo.cpp`.
3. Check the `IMGUI_VERSION_NUM` conditionals in `ImGuizmo.cpp` cover the vendored Dear ImGui version; rebuild
   Debug/Release/Dist and exercise the editor gizmo (translate/rotate/scale, local/world, snapping).
4. Re-check whether the known issue above is fixed, then update this file (commit, date, notes).
