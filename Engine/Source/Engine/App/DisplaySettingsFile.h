#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/DisplayModes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 3): the display settings a game chose (Application.SetDisplaySettings),
// which the engine keeps in user://Display.json and applies when the Runtime starts, before its window first shows:
//   {"Format": "DisplaySettings", "Version": 1, "WindowMode": "Windowed" | "Borderless" | "Exclusive", "Width": <1 to 16384>,
//    "Height": <1 to 16384>, "RefreshRate": <0 to 1000>, "VSync": <bool>}
// (MaxDisplayExtent and MaxDisplayRefreshRate, ValidateDisplaySettings' ranges)
// written tab-indented with a final line feed; unknown members are ignored. Engine-written user data, as EditorPreferences'
// Editor.json is, and a fuzz target ("DisplaySettings").

namespace Engine {

	class VirtualFileSystem;
	struct ApplicationSpecification;

	inline constexpr std::string_view DisplaySettingsFormatName = "DisplaySettings";
	inline constexpr uint32_t DisplaySettingsFormatVersion = 1;
	inline constexpr std::string_view DisplaySettingsPath = "user://Display.json";

	// The settings `text` holds. Errors: the JSON reader's (located), UnsupportedVersion for a newer version, Validation for
	// a member missing, of the wrong type or out of range (located at its pointer).
	[[nodiscard]] Result<DisplaySettings> ParseDisplaySettingsFile(std::string_view text);
	// The file's text for `settings`. Errors: ValidateDisplaySettings' InvalidArgument (its ranges are the reader's).
	[[nodiscard]] Result<std::string> SerializeDisplaySettingsFile(const DisplaySettings& settings);

	// Writes `settings` to user://Display.json atomically. Errors: InvalidState without a user:// mount; the serializer's and
	// the file system's.
	[[nodiscard]] Status WriteDisplaySettingsFile(VirtualFileSystem& vfs, const DisplaySettings& settings);

	// What the Runtime's startup read found: the settings (nullopt without a readable file) and, for a file that cannot be read
	// or parsed, the problem, which the Runtime logs once logging exists (the factory runs before the process level).
	struct StartupDisplaySettings
	{
		std::optional<DisplaySettings> Settings{};
		std::string Problem{};
	};

	// The settings a Runtime starts with, read before the process level exists (the application factory): `appName`'s
	// Display.json under `userDataRoot` (--user-data-dir), or under the operating system's root when it is empty
	// (Paths::GetUserDataPaths). No settings without a file; a file that cannot be read or parsed gives no settings and its
	// problem, so the project's Window settings apply. Reads only; logs nothing.
	[[nodiscard]] StartupDisplaySettings ReadStartupDisplaySettings(std::string_view appName, const std::filesystem::path& userDataRoot);

	// Makes `specification` create its window in `settings`' mode (WindowSpecification::DisplayMode and RefreshRate; Width
	// and Height but in Borderless, which covers the monitor) with `settings`' VSync (GraphicsSpecification::VSync), so the
	// first window shows in the chosen mode. The project's Fullscreen is cleared: the restored mode replaces it.
	void ApplyStartupDisplaySettings(ApplicationSpecification& specification, const DisplaySettings& settings);

}
