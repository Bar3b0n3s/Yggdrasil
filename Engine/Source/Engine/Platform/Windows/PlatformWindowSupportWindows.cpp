#include "EnginePCH.h"
#include "Engine/Platform/PlatformWindowSupport.h"

// CheckPlatformWindowSupport on Windows (Docs/Decisions/0035-m24-contract.md decision 17): stream V implements it.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Error.h"

namespace Engine {

	Status CheckPlatformWindowSupport()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CheckPlatformWindowSupport (Docs/Decisions/0035-m24-contract.md decision 17, stream V)");
	}

}

#endif
