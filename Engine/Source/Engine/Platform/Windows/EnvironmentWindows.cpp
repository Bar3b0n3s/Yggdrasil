#include "EnginePCH.h"
#include "Engine/Platform/Environment.h"

// M5 contract stub (Roadmap rule 3): stream A (loader, device, selection, creation wrappers, host image upload) implements
// reading a variable with GetEnvironmentVariableW and converting it to UTF-8. Until then every variable reads as not set.

#if defined(ENGINE_PLATFORM_WINDOWS)

namespace Engine {

	std::optional<std::string> ReadEnvironmentVariable(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}

#endif
