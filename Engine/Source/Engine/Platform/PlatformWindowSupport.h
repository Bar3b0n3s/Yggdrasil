#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

// Platform windows (M24, C24.1, E-VIEWPORTS; Docs/Decisions/0035-m24-contract.md decisions 17 to 21; stream V): whether this
// process may show Dear ImGui's platform windows, panels undocked out of the main window into OS windows of their own (the
// multi-viewport feature, ImGuiConfigFlags_ViewportsEnable). The windows themselves are GLFW windows that the vendored GLFW
// backend creates (ImGui/ImGuiLayer.h), drawn by ImGui/ImGuiPlatformWindows.h. Windows backends only (ADR 0028 decision 10):
// on Windows the backend (Platform/Windows/PlatformWindowSupportWindows.cpp) allows them on the native platform; on Linux and
// macOS CheckPlatformWindowSupport returns ErrorCode::Unsupported naming the platform
// (Platform/Posix/PlatformWindowSupportPosix.cpp), as ResourcePatcher::Patch and NativeFileDialog::Open do there, so ImGuiLayer
// keeps ImGuiConfigFlags_ViewportsEnable cleared, as before M24, and every panel stays docked in the main window. No X11 or
// Cocoa platform-window code is written.

namespace Engine {

	// Ok when this process can show platform windows: Windows, a windowed process (GLFW's native Win32 platform). Errors:
	// Unsupported naming the reason: a headless process (GLFW's null platform has no OS windows, so headless editors, their
	// editor.screenshot and the editor-UI goldens keep the docked layout), or Linux and macOS (no backend: Windows backends
	// only), its hint saying that panels stay docked in the main window. Main thread, after GlfwLibrary's initialization.
	[[nodiscard]] Status CheckPlatformWindowSupport();

}
