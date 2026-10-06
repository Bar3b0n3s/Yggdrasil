#include "EnginePCH.h"
#include "Engine/Core/UUIDGenerator.h"

// M1 contract stub (Roadmap rule 3): stream B implements both modes. Until then Next() returns the invalid UUID.

namespace Engine {

	UUIDGenerator::UUIDGenerator(UUIDGeneratorMode mode, uint64_t seed)
		: m_Random(seed), m_Mode(mode)
	{
	}

	UUIDGenerator UUIDGenerator::CreateRandom(uint64_t seed)
	{
		return UUIDGenerator(UUIDGeneratorMode::Random, seed);
	}

	UUIDGenerator::UUIDGenerator(const Random::State& /*state*/)
		: m_Random(0), m_Mode(UUIDGeneratorMode::Random)
	{
	}

	UUIDGenerator UUIDGenerator::CreateRandom(const Random::State& state)
	{
		return UUIDGenerator(state);
	}

	UUIDGenerator UUIDGenerator::CreateDeterministic(uint64_t sessionSeed)
	{
		return UUIDGenerator(UUIDGeneratorMode::Deterministic, sessionSeed);
	}

	UUID UUIDGenerator::Next()
	{
		return UUID();
	}

}
