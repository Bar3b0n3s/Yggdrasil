#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <span>

// Bytes from the operating system's cryptographically secure generator: BCryptGenRandom on Windows, getrandom on Linux,
// arc4random_buf on macOS. They seed the editor's UUIDGenerator (Architecture §4.8; Core may not call the OS, ADR 0003
// decision 19) and make the automation session token (§13.2). Never on the simulation path: simulated randomness comes
// only from seeded Random streams (§4.12).

namespace Engine {

	class SecureRandom
	{
	public:
		SecureRandom() = delete;

		// Fills `output` completely. Thread-safe. Errors: Io when the OS generator fails (the message carries its error).
		[[nodiscard]] static Status Fill(std::span<std::byte> output);

		// A xoshiro256** state for UUIDGenerator::CreateRandom or Random::SetState: 32 secure bytes, drawn again in the
		// (astronomically unlikely) case that they are all zero, which xoshiro forbids. Errors: those of Fill.
		[[nodiscard]] static Result<Random::State> GenerateState();
	};

}
