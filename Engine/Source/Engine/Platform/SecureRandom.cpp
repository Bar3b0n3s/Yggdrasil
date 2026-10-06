#include "EnginePCH.h"
#include "Engine/Platform/SecureRandom.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements GenerateState on
// top of Fill, whose OS-specific implementation lives in Platform/Windows/SecureRandomWindows.cpp and
// Platform/Posix/SecureRandomPosix.cpp.

namespace Engine {

	Result<Random::State> SecureRandom::GenerateState()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SecureRandom::GenerateState is not implemented yet");
	}

}
