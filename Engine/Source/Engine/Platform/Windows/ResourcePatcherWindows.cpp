#include "EnginePCH.h"
#include "Engine/Platform/ResourcePatcher.h"

// ResourcePatcher::Patch on Windows: BeginUpdateResourceW, UpdateResourceW and EndUpdateResourceW (ResourcePatcher.h).

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Error.h"

namespace Engine {

	Status ResourcePatcher::Patch(const std::filesystem::path& /*executable*/, const ExecutableResourceSet& /*resources*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M15 resource patcher contract (stream A)"));
	}

}

#endif
