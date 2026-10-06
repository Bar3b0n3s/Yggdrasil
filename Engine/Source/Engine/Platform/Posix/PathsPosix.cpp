#include "EnginePCH.h"
#include "Engine/Platform/Paths.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements GetUserDataRoot
// on Linux (XDG_DATA_HOME, HOME) and macOS (HOME/Library/Application Support). Until then it fails with Unsupported.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

namespace Engine {

	Result<std::filesystem::path> Paths::GetUserDataRoot()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Paths::GetUserDataRoot is not implemented yet");
	}

}

#endif
