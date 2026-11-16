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
