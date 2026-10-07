#include "EnginePCH.h"
#include "Engine/Platform/Environment.h"

// Reading the environment on Windows: GetEnvironmentVariableW, whose UTF-16 value is converted to UTF-8. The MSVC
// runtime's getenv reads the C runtime's ANSI copy of the environment, which cannot represent every value.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <windows.h>

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

		const Utils::NativeString wideName = Utils::NativeStringFromUtf8(name);
		std::wstring value(64, L'\0');
		while (true)
		{
			// A return value of 0 means "not set" or "set and empty"; only the last error tells them apart.
			SetLastError(ERROR_SUCCESS);
			const DWORD result = GetEnvironmentVariableW(wideName.c_str(), value.data(), static_cast<DWORD>(value.size()));
			if (result == 0)
			{
				if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
					return std::nullopt;
				return std::string();
			}
			if (result < value.size())
			{
				value.resize(result);
				return Utils::NativeStringToUtf8(value);
			}
			// Too small: the result is the size needed, terminator included. The value may grow again between two calls,
			// so the loop retries until it fits.
			value.assign(result, L'\0');
		}
	}

}

#endif
