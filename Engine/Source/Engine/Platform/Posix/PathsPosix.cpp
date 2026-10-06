#include "EnginePCH.h"
#include "Engine/Platform/Paths.h"

// Paths::GetUserDataRoot on Linux ($XDG_DATA_HOME, else $HOME/.local/share, per the XDG Base Directory specification)
// and macOS ($HOME/Library/Application Support).

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include <cstdlib>

namespace Engine {

	namespace Utils {

		// The value of the environment variable `name` when it is set to an absolute path; nullopt otherwise. The XDG
		// specification says relative values are invalid and must be ignored, and an empty HOME is no home either.
		static std::optional<std::filesystem::path> GetAbsolutePathVariable(const char* name)
		{
			const char* value = std::getenv(name);
			if (value == nullptr || value[0] == '\0')
				return std::nullopt;
			std::filesystem::path path(value);
			if (!path.is_absolute())
				return std::nullopt;
			return path;
		}

	}

	Result<std::filesystem::path> Paths::GetUserDataRoot()
	{
	#if defined(ENGINE_PLATFORM_LINUX)
		if (std::optional<std::filesystem::path> dataHome = Utils::GetAbsolutePathVariable("XDG_DATA_HOME"))
			return std::move(*dataHome);
	#endif

		const std::optional<std::filesystem::path> home = Utils::GetAbsolutePathVariable("HOME");
		if (!home.has_value())
		{
			return MakeError(ErrorCode::NotFound, "cannot determine the user-data folder: HOME is not set to an absolute path");
		}

	#if defined(ENGINE_PLATFORM_LINUX)
		return *home / ".local" / "share";
	#elif defined(ENGINE_PLATFORM_MACOS)
		return *home / "Library" / "Application Support";
	#endif
	}

}

#endif
