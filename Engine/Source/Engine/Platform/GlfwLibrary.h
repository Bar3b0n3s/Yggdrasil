#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>

// GLFW's process-level state (Architecture §3 rule 5, §4.1, §4.3). GLFW is main-thread-only and runs one platform per
// process, so it is initialized once, by ProcessContext (from RunApplication or the Tests main), before any Window
// exists, and terminated after the last one is gone.

namespace Engine {

	class Window;

	// How a process presents itself, chosen once per process (§4.1): a process is windowed or headless, never both.
	enum class WindowMode : uint8_t
	{
		Windowed, // GLFW's native platform: Win32, Cocoa or X11 (Linux runs X11 only, including XWayland sessions)
		Headless  // GLFW's null platform: windows, time and input logic run unchanged without a display
	};

	// The platform GLFW runs on (glfwGetPlatform).
	enum class GlfwPlatform : uint8_t
	{
		None, // GLFW is not initialized
		Null,
		Win32,
		Cocoa,
		X11
	};

	// PFN_vkGetInstanceProcAddr, spelled without the Vulkan headers, which Platform's public headers do not expose (§3
	// rule 3). VkInstance is a dispatchable handle, a pointer.
	using VulkanVoidFunction = void (*)();
	using VulkanGetInstanceProcAddrFunction = VulkanVoidFunction (*)(void* instance, const char* name);

	struct GlfwLibrarySpecification
	{
		// Headless selects GLFW_PLATFORM_NULL; Windowed the native platform of the host.
		WindowMode Mode = WindowMode::Windowed;
		// The engine's Vulkan loader entry point, handed to glfwInitVulkanLoader before glfwInit so that GLFW uses the
		// engine's loader instead of loading its own (§4.1, §8.1). Null: GLFW is not told about a loader (no renderer yet;
		// the Graphics milestone passes the loader's vkGetInstanceProcAddr).
		VulkanGetInstanceProcAddrFunction VulkanLoader = nullptr;
	};

	// Static access to the process's GLFW library. Every member is main-thread-only (GLFW's rule; asserted in Debug and
	// Release builds against the thread that called Initialize).
	class GlfwLibrary
	{
	public:
		GlfwLibrary() = delete;

		// In order: installs the GLFW error callback (GLFW errors are logged at Error level as "GLFW error <code>:
		// <description>"), glfwInitVulkanLoader when VulkanLoader is set, glfwInitHint(GLFW_PLATFORM, ...) with the
		// platform chosen by Mode, glfwInit. Initializing twice without Shutdown in between is a programmer error
		// (asserted). Errors: Unsupported when GLFW cannot initialize the requested platform, for example Windowed on a
		// Linux host without an X display; the message carries GLFW's description and the hint names --headless. Nothing
		// stays initialized on failure.
		[[nodiscard]] static Status Initialize(const GlfwLibrarySpecification& specification);

		// glfwTerminate. Every Window must be destroyed before (asserted). Idempotent.
		static void Shutdown();

		[[nodiscard]] static bool IsInitialized();

		// The platform GLFW runs on; None when not initialized.
		[[nodiscard]] static GlfwPlatform GetPlatform();

		// The mode of the current initialization; Windowed when not initialized.
		[[nodiscard]] static WindowMode GetMode();

		// The number of live Window objects (Window registers itself), checked by Shutdown.
		[[nodiscard]] static uint32_t GetWindowCount();
	private:
		// Called by every Window when its GLFW window is created and destroyed.
		static void RegisterWindow();
		static void UnregisterWindow();

		// True on the thread that called Initialize while GLFW is initialized.
		[[nodiscard]] static bool IsMainThread();

		// The calling thread's last GLFW error as text, which also clears it: "<description> (GLFW error 0x<code>)".
		[[nodiscard]] static std::string TakeErrorDescription();
	private:
		friend class Window;
	};

	// "Windowed" or "Headless".
	[[nodiscard]] std::string_view WindowModeToString(WindowMode mode);

	// "None", "Null", "Win32", "Cocoa" or "X11".
	[[nodiscard]] std::string_view GlfwPlatformToString(GlfwPlatform platform);

}
