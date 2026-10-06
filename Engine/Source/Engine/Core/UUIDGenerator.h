#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/UUID.h"

#include <cstdint>

namespace Engine {

	enum class UUIDGeneratorMode : uint8_t
	{
		Random,       // editor: xoshiro256** outputs
		Deterministic // runtime (play, export, tests): Hash64(sessionSeed, counter)
	};

	// Produces new UUIDs (Architecture §4.8). Always an instance owned by its user (an editor document, a play session),
	// never a global. Next() never returns the invalid UUID 0 nor a value in the reserved built-in asset range
	// 0x0000000000000001-0x00000000000003ff; such outputs are skipped and the next one is drawn. A value type; not
	// thread-safe (one owner).
	class UUIDGenerator
	{
	public:
		// Highest value of the reserved built-in asset range (Resources/EngineAssets.json).
		static constexpr uint64_t MaxReservedValue = 0x3ff;

		// Random mode seeded explicitly: the outputs of Random(seed).NextU64(). Tests use it for reproducible editor IDs.
		[[nodiscard]] static UUIDGenerator CreateRandom(uint64_t seed);

		// Random mode starting from the 256-bit xoshiro state `state` (Random::SetState; not all zero, asserted): the
		// editor fills it with OS CSPRNG bytes (§4.8), which Core does not obtain itself because it is on the simulation
		// path (no std::random_device, AGENTS.md); Platform provides them (M2). Its IDs are not reproducible, so it is
		// never used on the simulation path.
		[[nodiscard]] static UUIDGenerator CreateRandom(const Random::State& state);

		// Deterministic mode: the n-th call (n = 0, 1, 2 ...) yields Hash64(sessionSeed, n), skipping reserved values (a
		// skipped value still consumes its n). The caller re-calls Next() on the astronomically unlikely collision with
		// an existing ID, which re-hashes with the next counter (§4.8). Runtime spawns are therefore reproducible.
		[[nodiscard]] static UUIDGenerator CreateDeterministic(uint64_t sessionSeed);

		[[nodiscard]] UUID Next();

		[[nodiscard]] UUIDGeneratorMode GetMode() const { return m_Mode; }
		// The number of values drawn so far, skipped ones included (the next counter in Deterministic mode).
		[[nodiscard]] uint64_t GetDrawCount() const { return m_DrawCount; }
	private:
		UUIDGenerator(UUIDGeneratorMode mode, uint64_t seed);
		explicit UUIDGenerator(const Random::State& state);
	private:
		Random m_Random;
		uint64_t m_SessionSeed = 0; // Deterministic mode only
		uint64_t m_DrawCount = 0;
		UUIDGeneratorMode m_Mode = UUIDGeneratorMode::Random;
	};

}
