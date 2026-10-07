#include "EnginePCH.h"
#include "Engine/Platform/Environment.h"

// Reading the environment on Linux and macOS: getenv, whose bytes are the value (UTF-8 by convention, §16).

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include <cstdlib>

namespace Engine {

	namespace Utils {

		// Whether `name` can name a variable: not empty, and without '=' (the separator of the environment block) or NUL.
		static bool IsValidEnvironmentVariableName(std::string_view name)
		{
			return !name.empty() && name.find('=') == std::string_view::npos && name.find('\0') == std::string_view::npos;
		}

	}

	std::optional<std::string> ReadEnvironmentVariable(std::string_view name)
	{
		if (!Utils::IsValidEnvironmentVariableName(name))
			return std::nullopt;

		const std::string terminatedName(name);
		const char* value = std::getenv(terminatedName.c_str());
		if (value == nullptr)
			return std::nullopt;
		return std::string(value);
	}

}

#endif
