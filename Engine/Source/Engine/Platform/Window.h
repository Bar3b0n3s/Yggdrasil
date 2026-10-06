#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/Events.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// The application window (Architecture §4.3): one concrete GLFW class, no per-OS hierarchy.

namespace Engine {

	// The cursor over the window (script Input.SetCursorMode, §11.5).
	enum class CursorMode : uint8_t
	{
		Normal, // visible, moves freely
		Hidden, // invisible while over the window
		Locked  // invisible and captured; positions are unbounded, for relative (camera) motion
	};

	struct WindowSpecification
	{
		std::string Title{};
		uint32_t Width = 1600; // window size in screen units
		uint32_t Height = 900;
		bool Resizable = true;
		bool Fullscreen = false; // full screen on the primary monitor at its current video mode
	};

	// Receives every event the window delivers, on the main thread, during PollEvents or WaitEventsTimeout.
	using WindowEventCallback = std::function<void(Event& event)>;

	// A GLFW window created with GLFW_CLIENT_API = GLFW_NO_API (the renderer makes its own Vulkan surface, §8.1), on the
	// process's GLFW platform (GlfwLibrary): a native window in a windowed process, a null-platform window in a headless
	// one, where window logic, time and input run unchanged without a display (§4.3).
	//
	// Events. The GLFW callbacks of this window and the events passed to InjectEvent go into one queue, in arrival order.
	// PollEvents and WaitEventsTimeout process the OS events (which run the callbacks), poll the gamepads, and then deliver
	// the whole queue, in order, to the event callback; without a callback the queue is dropped. For OS events the window
	// updates its own size, framebuffer size, focus and minimized state before delivering, so the accessors agree with the
	// events. Injected events are delivered as they are and never change that state: an injected WindowResizeEvent,
	// WindowFocusEvent or WindowCloseEvent reaches the callback, but the accessors keep following the OS.
	//
	// Gamepads are process-wide in GLFW and are polled, not reported: GLFW joysticks 1 to 4 that have a gamepad mapping are
	// gamepads 0 to 3. Each poll compares their state with the previous poll and queues GamepadEvents for the changes, in
	// gamepad order, then connection, buttons in GamepadButton order and axes in GamepadAxis order. Axis values are
	// converted with GamepadAxisValueFromGlfw.
	//
	// Minimized: IsMinimized is true while the window is iconified or its framebuffer is 0x0. The frame loop then waits in
	// WaitEventsTimeout instead of PollEvents, so the CPU idles (§4.2).
	//
	// Main thread only (GLFW's rule; asserted in Debug and Release builds). A movable value: the GLFW window, the queue and
	// the callback live behind a stable private implementation, so moving a Window does not disturb GLFW's callbacks. A
	// moved-from Window may only be destroyed or assigned to. GlfwLibrary::Shutdown requires every Window to be destroyed.
	class Window
	{
	public:
		// Creates the window. Errors: InvalidState when GLFW is not initialized (no ProcessContext); InvalidArgument for a
		// zero Width or Height; Unsupported when GLFW cannot create it (the message carries GLFW's description).
		[[nodiscard]] static Result<Window> Create(const WindowSpecification& specification);

		~Window();

		Window(Window&& other) noexcept;
		Window& operator=(Window&& other) noexcept;
		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		// Replaces the event callback; an empty function removes it.
		void SetEventCallback(WindowEventCallback callback);

		// Processes pending OS events without waiting (glfwPollEvents), polls the gamepads and delivers the queue.
		void PollEvents();

		// Like PollEvents, but first waits up to `timeoutSeconds` (> 0, finite; asserted) for an OS event
		// (glfwWaitEventsTimeout). Queued injected events are delivered without waiting.
		void WaitEventsTimeout(double timeoutSeconds);

		// Appends `event` to the queue; the next PollEvents or WaitEventsTimeout delivers it after the events queued before
		// it, along the same path as a real one (Application::OnEvent first, then InputState). Tests use it to drive the
		// frame loop and the window glue. Automation and replay input never goes through the window: from M7 it is applied
		// by PlaySession at step 1 of a tick (§5.7, §13.6), so the editor's filtering of real input (§4.3) never sees it.
		void InjectEvent(Event event);

		// Iconifies or restores the window (glfwIconifyWindow, glfwRestoreWindow); the resulting events are queued.
		void Minimize();
		void Restore();

		void SetTitle(std::string_view title);
		[[nodiscard]] const std::string& GetTitle() const;

		void SetCursorMode(CursorMode mode);
		[[nodiscard]] CursorMode GetCursorMode() const;

		// The window size in screen units.
		[[nodiscard]] uint32_t GetWidth() const;
		[[nodiscard]] uint32_t GetHeight() const;
		// The framebuffer size in pixels; 0x0 while minimized on some platforms.
		[[nodiscard]] uint32_t GetFramebufferWidth() const;
		[[nodiscard]] uint32_t GetFramebufferHeight() const;
		// Pixels per screen unit per axis (glfwGetWindowContentScale); (1, 1) on the null platform.
		[[nodiscard]] glm::vec2 GetContentScale() const;

		[[nodiscard]] bool IsMinimized() const;
		[[nodiscard]] bool IsFocused() const;

		// The GLFWwindow*, for the code outside Platform that must call GLFW with it: the GLFW glue of Dear ImGui
		// (ImGui/ImGuiGlfwImplementation.cpp, §2.2) and the Graphics device setup, which creates the Vulkan surface
		// (glfwCreateWindowSurface) and queries instance extensions and presentation support (§8.1; ADR 0005 decision 8).
		// Never null for a live Window.
		[[nodiscard]] void* GetNativeHandle() const;
	private:
		struct Impl;

		explicit Window(Scope<Impl> impl);
	private:
		Scope<Impl> m_Impl;
	};

	// "Normal", "Hidden" or "Locked".
	[[nodiscard]] std::string_view CursorModeToString(CursorMode mode);

}
