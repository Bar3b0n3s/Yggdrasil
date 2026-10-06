#include "EnginePCH.h"
#include "Engine/Platform/SecureRandom.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements Fill on Windows
// (BCryptGenRandom with the system-preferred generator). Until then it fails with Unsupported.

#if defined(ENGINE_PLATFORM_WINDOWS)

namespace Engine {

	Status SecureRandom::Fill(std::span<std::byte> /*output*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SecureRandom::Fill is not implemented yet");
	}

}

#endif
