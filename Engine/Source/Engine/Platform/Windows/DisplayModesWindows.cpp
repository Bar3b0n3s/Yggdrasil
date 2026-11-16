#include "EnginePCH.h"
#include "Engine/Platform/DisplayModes.h"

// The display modes on Windows (Docs/Decisions/0043-m28-game-services.md decision 3), through GLFW's Win32 platform: its
// monitor and video-mode queries and glfwSetWindowMonitor. Exclusive is GLFW's full-screen monitor mode, without
// VK_EXT_full_screen_exclusive. A headless process runs GLFW's null platform, which has no display, so both calls refuse it.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Error.h"
	#include "Engine/Platform/GlfwLibrary.h"
	#include "Engine/Platform/Window.h"

	#include <GLFW/glfw3.h>

	#include <algorithm>
	#include <format>
	#include <span>
	#include <string>
	#include <tuple>
	#include <vector>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static Status CheckDisplayPlatform()
		{
			switch (GlfwLibrary::GetPlatform())
			{
				case GlfwPlatform::Win32: return {};
				case GlfwPlatform::Null:
					return std::unexpected(Error(ErrorCode::Unsupported, "a headless process has no display: GLFW's null platform has no monitors")
							.WithHint("display settings apply to a windowed Runtime"));
				case GlfwPlatform::None:  return std::unexpected(Error(ErrorCode::Unsupported, "display settings need GLFW, which is not initialized"));
				case GlfwPlatform::Cocoa:
				case GlfwPlatform::X11:   break;
			}
			return std::unexpected(Error(ErrorCode::Unsupported, "display settings need GLFW's Win32 platform"));
		}

		// The monitor `handle` is full screen on, else the monitor holding the window's centre, else the primary monitor.
		[[nodiscard]] static GLFWmonitor* FindWindowMonitor(GLFWwindow* handle)
		{
			if (GLFWmonitor* fullScreen = glfwGetWindowMonitor(handle))
				return fullScreen;
			int x = 0;
			int y = 0;
			int width = 0;
			int height = 0;
			glfwGetWindowPos(handle, &x, &y);
			glfwGetWindowSize(handle, &width, &height);
			const int centreX = x + width / 2;
			const int centreY = y + height / 2;
			int count = 0;
			GLFWmonitor** monitors = glfwGetMonitors(&count);
			for (GLFWmonitor* monitor : std::span(monitors, monitors != nullptr ? static_cast<size_t>(count) : 0))
			{
				int left = 0;
				int top = 0;
				glfwGetMonitorPos(monitor, &left, &top);
				const GLFWvidmode* mode = glfwGetVideoMode(monitor);
				if (mode != nullptr && centreX >= left && centreX < left + mode->width && centreY >= top && centreY < top + mode->height)
					return monitor;
			}
			return glfwGetPrimaryMonitor();
		}

		[[nodiscard]] static Result<std::vector<DisplayVideoMode>> ReadVideoModes(GLFWmonitor* monitor)
		{
			int count = 0;
			const GLFWvidmode* modes = glfwGetVideoModes(monitor, &count);
			if (modes == nullptr || count <= 0)
				return MakeError(ErrorCode::Unsupported, "the monitor reports no video modes");
			std::vector<DisplayVideoMode> result;
			for (const GLFWvidmode& mode : std::span(modes, static_cast<size_t>(count)))
			{
				if (mode.width <= 0 || mode.height <= 0 || mode.refreshRate < 0)
					continue;
				result.push_back({ .Width = static_cast<uint32_t>(mode.width), .Height = static_cast<uint32_t>(mode.height), .RefreshRate = static_cast<uint32_t>(mode.refreshRate) });
			}
			std::ranges::sort(result, [](const DisplayVideoMode& left, const DisplayVideoMode& right)
			{
				return std::tie(left.Width, left.Height, left.RefreshRate) < std::tie(right.Width, right.Height, right.RefreshRate);
			});
			result.erase(std::unique(result.begin(), result.end()), result.end());
			return result;
		}

	}

	Result<std::vector<DisplayVideoMode>> QueryDisplayVideoModes(const Window& window)
	{
		ENGINE_TRY(Utils::CheckDisplayPlatform());
		GLFWmonitor* monitor = Utils::FindWindowMonitor(static_cast<GLFWwindow*>(window.GetNativeHandle()));
		if (monitor == nullptr)
			return MakeError(ErrorCode::Unsupported, "no monitor is connected");
		return Utils::ReadVideoModes(monitor);
	}

	Result<DisplayVideoMode> QueryCurrentVideoMode(const Window& window)
	{
		ENGINE_TRY(Utils::CheckDisplayPlatform());
		GLFWmonitor* monitor = Utils::FindWindowMonitor(static_cast<GLFWwindow*>(window.GetNativeHandle()));
		const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
		if (mode == nullptr || mode->width <= 0 || mode->height <= 0 || mode->refreshRate < 0)
			return MakeError(ErrorCode::Unsupported, "no monitor reports a current video mode");
		return DisplayVideoMode{ .Width = static_cast<uint32_t>(mode->width), .Height = static_cast<uint32_t>(mode->height), .RefreshRate = static_cast<uint32_t>(mode->refreshRate) };
	}

	Status ApplyDisplaySettings(Window& window, const DisplaySettings& settings)
	{
		ENGINE_TRY(Utils::CheckDisplayPlatform());
		GLFWwindow* handle = static_cast<GLFWwindow*>(window.GetNativeHandle());
		GLFWmonitor* monitor = Utils::FindWindowMonitor(handle);
		if (monitor == nullptr)
			return MakeError(ErrorCode::Unsupported, "no monitor is connected");
		const GLFWvidmode* current = glfwGetVideoMode(monitor);
		if (current == nullptr)
			return MakeError(ErrorCode::Unsupported, "the monitor reports no current video mode");
		int left = 0;
		int top = 0;
		glfwGetMonitorPos(monitor, &left, &top);
		switch (settings.WindowMode)
		{
			case DisplayWindowMode::Windowed:
			{
				if (settings.Width == 0 || settings.Height == 0)
					return MakeError(ErrorCode::InvalidArgument, "a windowed size needs a width and a height of at least 1");
				// A window leaving a full-screen mode is centred on its monitor; a window already windowed keeps its place.
				int x = left + std::max(0, (current->width - static_cast<int>(settings.Width)) / 2);
				int y = top + std::max(0, (current->height - static_cast<int>(settings.Height)) / 2);
				const bool wasWindowed = glfwGetWindowMonitor(handle) == nullptr && glfwGetWindowAttrib(handle, GLFW_DECORATED) == GLFW_TRUE;
				if (wasWindowed)
					glfwGetWindowPos(handle, &x, &y);
				glfwSetWindowAttrib(handle, GLFW_DECORATED, GLFW_TRUE);
				glfwSetWindowMonitor(handle, nullptr, x, y, static_cast<int>(settings.Width), static_cast<int>(settings.Height), GLFW_DONT_CARE);
				return {};
			}
			case DisplayWindowMode::Borderless:
				glfwSetWindowMonitor(handle, nullptr, left, top, current->width, current->height, GLFW_DONT_CARE);
				glfwSetWindowAttrib(handle, GLFW_DECORATED, GLFW_FALSE);
				glfwSetWindowMonitor(handle, nullptr, left, top, current->width, current->height, GLFW_DONT_CARE);
				return {};
			case DisplayWindowMode::Exclusive:
			{
				ENGINE_TRY_ASSIGN(const std::vector<DisplayVideoMode> modes, Utils::ReadVideoModes(monitor));
				const DisplayVideoMode wanted{ .Width = settings.Width, .Height = settings.Height, .RefreshRate = settings.RefreshRate };
				if (!ContainsDisplayVideoMode(modes, wanted))
				{
					return std::unexpected(Error(ErrorCode::InvalidArgument,
						std::format("the monitor has no video mode {} x {} at {} Hz", settings.Width, settings.Height, settings.RefreshRate))
							.WithHint("pick one of Application.GetDisplayModes()"));
				}
				glfwSetWindowAttrib(handle, GLFW_DECORATED, GLFW_TRUE);
				glfwSetWindowMonitor(handle, monitor, 0, 0, static_cast<int>(settings.Width), static_cast<int>(settings.Height), static_cast<int>(settings.RefreshRate));
				return {};
			}
		}
		return MakeError(ErrorCode::InvalidArgument, "unknown window mode");
	}

}

#endif
