#include "EnginePCH.h"
#include "Engine/Platform/SecureRandom.h"

// The host-independent part of SecureRandom: GenerateState on top of Fill, whose OS-specific implementation lives in
// Platform/Windows/SecureRandomWindows.cpp and Platform/Posix/SecureRandomPosix.cpp.

namespace Engine {

	Result<Random::State> SecureRandom::GenerateState()
	{
		const Random::State zero{};
		Random::State state{};
		do
		{
			ENGINE_TRY(Fill(std::as_writable_bytes(std::span(state))));
		} while (state == zero);
		return state;
	}

}
