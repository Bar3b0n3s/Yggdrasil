#include "EnginePCH.h"
#include "Engine/Platform/DisplayModes.h"

#include <algorithm>
#include <array>

namespace Engine {

	namespace {

		constexpr std::array<std::string_view, 3> DisplayWindowModeNames = { "Windowed", "Borderless", "Exclusive" };

	}

	std::string_view DisplayWindowModeToString(DisplayWindowMode mode)
	{
		const auto index = static_cast<size_t>(mode);
		return index < DisplayWindowModeNames.size() ? DisplayWindowModeNames[index] : std::string_view();
	}

	std::optional<DisplayWindowMode> DisplayWindowModeFromString(std::string_view name)
	{
		for (size_t index = 0; index < DisplayWindowModeNames.size(); ++index)
		{
			if (DisplayWindowModeNames[index] == name)
				return static_cast<DisplayWindowMode>(index);
		}
		return std::nullopt;
	}

	bool ContainsDisplayVideoMode(std::span<const DisplayVideoMode> modes, const DisplayVideoMode& mode)
	{
		return std::ranges::any_of(modes, [&mode](const DisplayVideoMode& listed)
		{
			return listed == mode;
		});
	}

	DisplaySettings ResolveDisplaySettings(const DisplayObservation& observation)
	{
		DisplaySettings settings{ .WindowMode = observation.Mode, .VSync = observation.VSync };
		if (observation.Width == 0 || observation.Height == 0)
		{
			// Minimized: the monitor may be back at the desktop's mode, so the last size and refresh rate stand in, and a partial
			// request (a VSync toggle) keeps the mode the window had.
			settings.Width = observation.Last.Width;
			settings.Height = observation.Last.Height;
			settings.RefreshRate = observation.Mode == DisplayWindowMode::Windowed ? 0 : observation.Last.RefreshRate;
			return settings;
		}
		settings.Width = observation.Width;
		settings.Height = observation.Height;
		settings.RefreshRate = observation.Mode == DisplayWindowMode::Windowed ? 0 : observation.MonitorRefreshRate;
		return settings;
	}

	DisplayWindowRect ComputeDisplayWindowRect(const DisplaySettings& settings, int32_t monitorX, int32_t monitorY, const DisplayVideoMode& desktop,
		const std::optional<std::pair<int32_t, int32_t>>& windowedPosition)
	{
		if (settings.WindowMode == DisplayWindowMode::Borderless)
			return { .X = monitorX, .Y = monitorY, .Width = desktop.Width, .Height = desktop.Height };
		DisplayWindowRect rect{ .Width = settings.Width, .Height = settings.Height };
		if (windowedPosition.has_value())
		{
			rect.X = windowedPosition->first;
			rect.Y = windowedPosition->second;
			return rect;
		}
		const int64_t spareX = static_cast<int64_t>(desktop.Width) - static_cast<int64_t>(settings.Width);
		const int64_t spareY = static_cast<int64_t>(desktop.Height) - static_cast<int64_t>(settings.Height);
		rect.X = monitorX + static_cast<int32_t>(std::max<int64_t>(0, spareX / 2));
		rect.Y = monitorY + static_cast<int32_t>(std::max<int64_t>(0, spareY / 2));
		return rect;
	}

	Status ValidateDisplaySettings(const DisplaySettings& settings)
	{
		if (DisplayWindowModeToString(settings.WindowMode).empty())
			return MakeError(ErrorCode::InvalidArgument, "WindowMode must be Windowed, Borderless or Exclusive");
		if (settings.Width < 1 || settings.Width > MaxDisplayExtent)
			return MakeError(ErrorCode::InvalidArgument, "Width must be 1 to {} (got {})", MaxDisplayExtent, settings.Width);
		if (settings.Height < 1 || settings.Height > MaxDisplayExtent)
			return MakeError(ErrorCode::InvalidArgument, "Height must be 1 to {} (got {})", MaxDisplayExtent, settings.Height);
		if (settings.RefreshRate > MaxDisplayRefreshRate)
			return MakeError(ErrorCode::InvalidArgument, "RefreshRate must be 0 to {} Hz (got {})", MaxDisplayRefreshRate, settings.RefreshRate);
		return {};
	}

}
