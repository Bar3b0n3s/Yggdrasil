#include "EnginePCH.h"
#include "Engine/Platform/ResourcePatcher.h"

// ResourcePatcher::Patch on Linux and macOS: executables carry no resources there (§16); macOS writes the same data into
// the bundle's Info.plist and .icns (EditorCore/Export/AppBundle.h).

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Error.h"
	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <format>

namespace Engine {

	Status ResourcePatcher::Patch(const std::filesystem::path& executable, const ExecutableResourceSet& /*resources*/)
	{
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("'{}' cannot carry resources: only Windows executables do", Utils::NativePathToUtf8(executable)))
				.WithHint("macOS exports write the version and icon into the bundle's Info.plist and AppIcon.icns; Linux executables carry "
						  "none"));
	}

}

#endif
