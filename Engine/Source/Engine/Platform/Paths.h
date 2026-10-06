#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

// Per-user data folders (Architecture §4.4, §4.10, §4.13). Everything an application writes for the user goes under
// <UserData>/<AppName>/: logs, crash reports and user://. AppName is ENGINE_PRODUCT_NAME for the Editor and Tests and the
// manifest Name for exported games (§14.3), so a shipped game never writes into the engine's folder. Paths only computes
// and validates; ProcessContext and EngineContext create and use the folders.

namespace Engine {

	// The folders of one application under one user-data root.
	struct UserDataPaths
	{
		std::filesystem::path Root{};    // <UserData>/<AppName>: mounted as user:// (§4.10)
		std::filesystem::path Logs{};    // Root/Logs: the rotating log file (§4.4)
		std::filesystem::path Crashes{}; // Root/Crashes: crash reports (§4.13)

		// Logs/<executableName>.log, the rotating log file of one executable ("Editor", "Tests", "Tetris").
		[[nodiscard]] std::filesystem::path GetLogFile(std::string_view executableName) const;
	};

	class Paths
	{
	public:
		// The longest accepted application name, in bytes.
		static constexpr size_t MaxAppNameLength = 64;

		Paths() = delete;

		// The operating system's per-user data root: %LOCALAPPDATA% (FOLDERID_LocalAppData) on Windows, $XDG_DATA_HOME or
		// else $HOME/.local/share on Linux, $HOME/Library/Application Support on macOS. Absolute. Errors: NotFound when it
		// cannot be determined (no profile folder, no usable HOME); Io.
		[[nodiscard]] static Result<std::filesystem::path> GetUserDataRoot();

		// Checks that `appName` (a manifest Name is user input) is a single folder name that works on every host: 1 to
		// MaxAppNameLength bytes of valid UTF-8; no control characters and none of < > : " / \ | ? *; not "." or "..";
		// no leading or trailing space and no trailing dot; not a Windows device name (CON, PRN, AUX, NUL, COM1 to COM9,
		// LPT1 to LPT9, in any case, with or without an extension). The same rules apply on every host, so a name that
		// works on Linux also works on Windows. Errors: Validation naming the rule that failed.
		[[nodiscard]] static Status ValidateAppName(std::string_view appName);

		// The folders of `appName` under `root`, or under GetUserDataRoot() when `root` is empty (tests pass a temporary
		// directory). Creates nothing. Errors: those of ValidateAppName and GetUserDataRoot; InvalidArgument for a relative
		// `root`.
		[[nodiscard]] static Result<UserDataPaths> GetUserDataPaths(std::string_view appName, const std::filesystem::path& root = {});

		// Creates Root, Logs and Crashes where missing. Errors: Io naming the folder.
		[[nodiscard]] static Status CreateUserDataDirectories(const UserDataPaths& paths);
	};

}
