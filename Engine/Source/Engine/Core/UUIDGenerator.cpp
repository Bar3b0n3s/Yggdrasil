#include "EnginePCH.h"
#include "Engine/Core/UUIDGenerator.h"

#include "Engine/Core/Hash.h"

namespace Engine {

	UUIDGenerator::UUIDGenerator(UUIDGeneratorMode mode, uint64_t seed)
		: m_Random(seed), m_SessionSeed(seed), m_Mode(mode)
	{
	}

	UUIDGenerator::UUIDGenerator(const Random::State& state)
		: m_Random(0), m_Mode(UUIDGeneratorMode::Random)
	{
		m_Random.SetState(state);
	}

	UUIDGenerator UUIDGenerator::CreateRandom(uint64_t seed)
	{
		return UUIDGenerator(UUIDGeneratorMode::Random, seed);
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
		// Skips the invalid UUID 0 and the reserved built-in range; each skipped draw still counts.
		uint64_t value = 0;
		do
		{
			value = m_Mode == UUIDGeneratorMode::Deterministic ? Hash64(m_SessionSeed, m_DrawCount) : m_Random.NextU64();
			++m_DrawCount;
		} while (value <= MaxReservedValue);
		return UUID(value);
	}

}
