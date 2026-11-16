#include "TestsPCH.h"
#include "Engine/Platform/DisplayModes.h"

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <utility>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 3): the platform-independent half of the display modes. The
// switch itself runs in the windowed child of SwapchainTests.cpp, which never enters Exclusive.

namespace Engine {

	TEST_SUITE("Platform")
	{
		TEST_CASE("DisplayModes: the mode names round-trip and others are refused")
		{
			for (const DisplayWindowMode mode : { DisplayWindowMode::Windowed, DisplayWindowMode::Borderless, DisplayWindowMode::Exclusive })
				CHECK(DisplayWindowModeFromString(DisplayWindowModeToString(mode)) == mode);
			CHECK_FALSE(DisplayWindowModeFromString("Fullscreen").has_value());
			CHECK_FALSE(DisplayWindowModeFromString("windowed").has_value());
		}

		TEST_CASE("DisplayModes: settings outside the ranges are refused, naming the member")
		{
			CHECK(ValidateDisplaySettings(DisplaySettings{ .Width = 1, .Height = MaxDisplayExtent, .RefreshRate = MaxDisplayRefreshRate }).has_value());
			for (const DisplaySettings& refused : { DisplaySettings{ .Width = 0, .Height = 720 }, DisplaySettings{ .Width = 1280, .Height = MaxDisplayExtent + 1 },
					 DisplaySettings{ .Width = 1280, .Height = 720, .RefreshRate = MaxDisplayRefreshRate + 1 },
					 DisplaySettings{ .WindowMode = static_cast<DisplayWindowMode>(7), .Width = 1280, .Height = 720 } })
			{
				const Status valid = ValidateDisplaySettings(refused);
				REQUIRE_FALSE(valid.has_value());
				CHECK(valid.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const std::array<DisplayVideoMode, 2> modes = { DisplayVideoMode{ 1280, 720, 60 }, DisplayVideoMode{ 1920, 1080, 144 } };
			CHECK(ContainsDisplayVideoMode(modes, DisplayVideoMode{ 1920, 1080, 144 }));
			CHECK_FALSE(ContainsDisplayVideoMode(modes, DisplayVideoMode{ 1920, 1080, 60 }));
		}

		// The W28.1 review's finding 1: the geometry is computed from the desktop's mode, read once the window has left an
		// exclusive mode, never from the exclusive mode's size.
		TEST_CASE("DisplayModes: a Borderless window covers its monitor and a Windowed one keeps its place or is centred")
		{
			const DisplayVideoMode desktop{ .Width = 2560, .Height = 1440, .RefreshRate = 60 };
			const DisplaySettings borderless{ .WindowMode = DisplayWindowMode::Borderless, .Width = 1280, .Height = 720 };
			CHECK(ComputeDisplayWindowRect(borderless, -2560, 0, desktop, std::nullopt) == DisplayWindowRect{ .X = -2560, .Y = 0, .Width = 2560, .Height = 1440 });
			CHECK(ComputeDisplayWindowRect(borderless, 0, 0, desktop, std::pair<int32_t, int32_t>(40, 50)) == DisplayWindowRect{ .X = 0, .Y = 0, .Width = 2560, .Height = 1440 });
			const DisplaySettings windowed{ .WindowMode = DisplayWindowMode::Windowed, .Width = 1600, .Height = 900 };
			// Leaving Exclusive (no windowed position): centred on the desktop's mode.
			CHECK(ComputeDisplayWindowRect(windowed, 0, 0, desktop, std::nullopt) == DisplayWindowRect{ .X = 480, .Y = 270, .Width = 1600, .Height = 900 });
			CHECK(ComputeDisplayWindowRect(windowed, 2560, 100, desktop, std::nullopt) == DisplayWindowRect{ .X = 3040, .Y = 370, .Width = 1600, .Height = 900 });
			// Already windowed: it keeps its place.
			CHECK(ComputeDisplayWindowRect(windowed, 0, 0, desktop, std::pair<int32_t, int32_t>(40, 50)) == DisplayWindowRect{ .X = 40, .Y = 50, .Width = 1600, .Height = 900 });
			// Larger than the monitor: never above or left of its origin.
			const DisplaySettings large{ .WindowMode = DisplayWindowMode::Windowed, .Width = 4000, .Height = 3000 };
			CHECK(ComputeDisplayWindowRect(large, 100, 200, desktop, std::nullopt) == DisplayWindowRect{ .X = 100, .Y = 200, .Width = 4000, .Height = 3000 });
		}
	}

}
