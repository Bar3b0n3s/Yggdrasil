#include "EnginePCH.h"
#include "Engine/Platform/ResourcePatcher.h"

// ResourcePatcher::Patch on Linux and macOS: executables carry no resources there (§16); macOS writes the same data into
// the bundle's Info.plist and .icns (EditorCore/Export/AppBundle.h).

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Error.h"

namespace Engine {

	Status ResourcePatcher::Patch(const std::filesystem::path& /*executable*/, const ExecutableResourceSet& /*resources*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 resource patcher contract (stream A)"));
	}

}

#endif
