#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 3): the display modes a game's options menu switches between, and
// the monitor's video modes it picks a resolution from. The query and the switch have a Windows backend only
// (Platform/Windows/DisplayModesWindows.cpp, through GLFW's Win32 platform); a headless process (GLFW's null platform) and
// Linux and macOS (Platform/Posix/DisplayModesPosix.cpp) report Unsupported (Docs/Decisions/0028-backlog-program.md decision
// 10, PlatformWindowSupport's precedent). Main thread only.

namespace Engine {

	class Window;

	enum class DisplayWindowMode : uint8_t
	{
		Windowed,   // a decorated window of Width x Height screen units
		Borderless, // an undecorated window covering its monitor at the monitor's current video mode
		Exclusive   // GLFW's full-screen monitor mode at Width x Height and RefreshRate, one of the monitor's video modes
	};

	struct DisplayVideoMode
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RefreshRate = 0; // Hz

		bool operator==(const DisplayVideoMode&) const = default;
	};

	// A window's display settings. Width, Height and RefreshRate are the window's size in Windowed mode (RefreshRate 0) and
	// the monitor's video mode in the two full-screen modes (Borderless: its current mode, which it never changes).
	struct DisplaySettings
	{
		DisplayWindowMode WindowMode = DisplayWindowMode::Windowed;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RefreshRate = 0;
		bool VSync = true;

		bool operator==(const DisplaySettings&) const = default;
	};

	// Whether `modes` lists `mode` (a lambda's comparison: MSVC's vectorized std::find has no path for a 12-byte element, which
	// clang-cl cannot instantiate).
	[[nodiscard]] bool ContainsDisplayVideoMode(std::span<const DisplayVideoMode> modes, const DisplayVideoMode& mode);

	// The screen rectangle of a window in a non-exclusive display mode.
	struct DisplayWindowRect
	{
		int32_t X = 0;
		int32_t Y = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;

		bool operator==(const DisplayWindowRect&) const = default;
	};

	// Where ApplyDisplaySettings puts a Windowed or Borderless window, on a monitor at (monitorX, monitorY) whose desktop
	// video mode is `desktop` (read once the window has left any exclusive mode, which changes the monitor's mode):
	// Borderless covers the monitor; Windowed takes settings' Width x Height at `windowedPosition` (the place of a window
	// that was windowed already), else centred on the monitor and never above or left of its origin. Pure; the Exclusive
	// mode has no rectangle (the monitor's mode is the window).
	[[nodiscard]] DisplayWindowRect ComputeDisplayWindowRect(const DisplaySettings& settings, int32_t monitorX, int32_t monitorY,
		const DisplayVideoMode& desktop, const std::optional<std::pair<int32_t, int32_t>>& windowedPosition);

	// What a window shows of its display settings now (Application::GetDisplaySettings' inputs).
	struct DisplayObservation
	{
		DisplayWindowMode Mode = DisplayWindowMode::Windowed; // the mode in force
		bool VSync = true;                                    // the VSync in force
		uint32_t Width = 0;                                   // the window's size; 0 x 0 while it is minimized
		uint32_t Height = 0;
		uint32_t MonitorRefreshRate = 0; // the current refresh rate of the window's monitor, 0 when unknown
		DisplaySettings Last{};          // what was resolved while the window was last not minimized
	};

	// The settings in force (M28, the W28.1 review's finding 4 and the W28.2 review's finding 10): the observation's mode and
	// VSync; while the window is minimized (an Exclusive window is, at every focus loss, when its monitor goes back to the
	// desktop's mode) the last size and refresh rate it had; otherwise its size and, in the two full-screen modes, its
	// monitor's current refresh rate (Windowed has none: 0). Pure.
	[[nodiscard]] DisplaySettings ResolveDisplaySettings(const DisplayObservation& observation);

	// What a frame boundary records of a window that is not minimized (M28; batch 2's merge review, finding 1): `last` with
	// the mode and VSync in force and the window's size, its refresh rate kept (0 Windowed), so the frame loop never queries
	// the monitor. The refresh rate is the last one known: the startup settings', an applied request's, or that of the last
	// GetDisplaySettings that queried the monitor. Pure.
	[[nodiscard]] DisplaySettings RecordFrameDisplay(const DisplaySettings& last, DisplayWindowMode mode, bool vsync, uint32_t width, uint32_t height);

	// The ranges every display request and user://Display.json take (ValidateDisplaySettings).
	inline constexpr uint32_t MaxDisplayExtent = 16384;
	inline constexpr uint32_t MaxDisplayRefreshRate = 1000;

	// "Windowed", "Borderless" or "Exclusive"; empty for a value outside the enumeration.
	[[nodiscard]] std::string_view DisplayWindowModeToString(DisplayWindowMode mode);
	// The mode of one of DisplayWindowModeToString's names (exact spelling); nullopt otherwise.
	[[nodiscard]] std::optional<DisplayWindowMode> DisplayWindowModeFromString(std::string_view name);

	// Checks `settings` against the ranges a display request and Display.json take, in every mode (Borderless ignores the
	// size, but a request carries the window's): a known WindowMode, Width and Height 1 to MaxDisplayExtent, RefreshRate 0 to
	// MaxDisplayRefreshRate. Errors: InvalidArgument naming the member. Pure; whether the monitor lists an Exclusive mode is
	// Application::RequestDisplaySettings' check.
	[[nodiscard]] Status ValidateDisplaySettings(const DisplaySettings& settings);

	// The video modes of the monitor `window` is on (the one it is full screen on, else the one holding its centre, else the
	// primary monitor), sorted by width, height and refresh rate, without duplicates (modes that differ only in colour depth).
	// Errors: Unsupported for a headless process, without a monitor, and on Linux and macOS.
	[[nodiscard]] Result<std::vector<DisplayVideoMode>> QueryDisplayVideoModes(const Window& window);
	// The current video mode of that monitor (a Borderless window's size and refresh rate). Errors: those of
	// QueryDisplayVideoModes.
	[[nodiscard]] Result<DisplayVideoMode> QueryCurrentVideoMode(const Window& window);

	// Switches `window` to `settings`' mode (VSync is the swapchain's, not the window's: Application applies it). Windowed
	// takes Width x Height (both at least 1), decorated, where a windowed window already is, or centred on its monitor when it
	// leaves a full-screen mode; Borderless covers the monitor at its current mode, undecorated (Width, Height and RefreshRate
	// ignored); Exclusive switches the monitor to the video mode of QueryDisplayVideoModes equal to Width, Height and
	// RefreshRate. Errors: InvalidArgument for a zero size or an Exclusive mode the monitor does not list;
	// Unsupported as QueryDisplayVideoModes. Nothing changes on error.
	[[nodiscard]] Status ApplyDisplaySettings(Window& window, const DisplaySettings& settings);

}
