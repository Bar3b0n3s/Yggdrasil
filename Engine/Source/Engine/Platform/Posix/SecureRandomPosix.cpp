#include "EnginePCH.h"
#include "Engine/Platform/SecureRandom.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements Fill on Linux
// (getrandom) and macOS (arc4random_buf). Until then it fails with Unsupported.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

namespace Engine {

	Status SecureRandom::Fill(std::span<std::byte> /*output*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SecureRandom::Fill is not implemented yet");
	}

}

#endif
