#include "EnginePCH.h"
#include "Engine/Core/Random.h"

// M1 contract stub (Roadmap rule 3): stream B implements SplitMix64 seeding and xoshiro256**. Until then every draw
// returns its lower bound.

namespace Engine {

	Random::Random(uint64_t /*seed*/)
	{
	}

	void Random::Seed(uint64_t /*seed*/)
	{
	}

	uint64_t Random::NextU64()
	{
		return 0;
	}

	uint32_t Random::NextU32()
	{
		return 0;
	}

	double Random::NextDouble()
	{
		return 0.0;
	}

	float Random::NextFloat()
	{
		return 0.0f;
	}

	int64_t Random::RangeInt(int64_t min, int64_t /*max*/)
	{
		return min;
	}

	double Random::RangeDouble(double min, double /*max*/)
	{
		return min;
	}

	bool Random::NextBool(double /*probability*/)
	{
		return false;
	}

	void Random::SetState(const State& /*state*/)
	{
	}

}
