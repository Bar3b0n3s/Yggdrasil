#include "EnginePCH.h"
#include "Engine/Core/Random.h"

#include "Engine/Core/Assert.h"

#include <bit>
#include <cmath>

namespace Engine {

	namespace Utils {

		// The high 64 bits of the 128-bit product a * b, portably.
		static uint64_t MultiplyHigh(uint64_t a, uint64_t b)
		{
			const uint64_t aLow = a & 0xffffffffull;
			const uint64_t aHigh = a >> 32;
			const uint64_t bLow = b & 0xffffffffull;
			const uint64_t bHigh = b >> 32;
			const uint64_t lowHigh = aLow * bHigh;
			const uint64_t highLow = aHigh * bLow;
			const uint64_t middle = ((aLow * bLow) >> 32) + (lowHigh & 0xffffffffull) + (highLow & 0xffffffffull);
			return aHigh * bHigh + (lowHigh >> 32) + (highLow >> 32) + (middle >> 32);
		}

	}

	Random::Random(uint64_t seed)
	{
		Seed(seed);
	}

	void Random::Seed(uint64_t seed)
	{
		// SplitMix64 (Steele, Lea and Flood): four consecutive outputs. Its mixing function is a bijection and the four
		// counters differ, so at most one output is zero and the state is valid for every seed.
		uint64_t counter = seed;
		for (uint64_t& word : m_State)
		{
			counter += 0x9e3779b97f4a7c15ull;
			uint64_t mixed = counter;
			mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ull;
			mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebull;
			word = mixed ^ (mixed >> 31);
		}
	}

	uint64_t Random::NextU64()
	{
		// xoshiro256** 1.0 (Blackman and Vigna).
		const uint64_t result = std::rotl(m_State[1] * 5, 7) * 9;
		const uint64_t shifted = m_State[1] << 17;
		m_State[2] ^= m_State[0];
		m_State[3] ^= m_State[1];
		m_State[1] ^= m_State[2];
		m_State[0] ^= m_State[3];
		m_State[2] ^= shifted;
		m_State[3] = std::rotl(m_State[3], 45);
		return result;
	}

	uint32_t Random::NextU32()
	{
		return static_cast<uint32_t>(NextU64() >> 32);
	}

	double Random::NextDouble()
	{
		return static_cast<double>(NextU64() >> 11) * 0x1.0p-53;
	}

	float Random::NextFloat()
	{
		return static_cast<float>(NextU64() >> 40) * 0x1.0p-24f;
	}

	int64_t Random::RangeInt(int64_t min, int64_t max)
	{
		ENGINE_CORE_ASSERT(min <= max, "Random::RangeInt needs min <= max, got [{}, {}]", min, max);

		// Lemire's nearly divisionless method, exactly as documented in Random.h. Unsigned arithmetic wraps, so the
		// full 64-bit range gives range == 0.
		const uint64_t range = static_cast<uint64_t>(max) - static_cast<uint64_t>(min) + 1;
		if (range == 0)
			return static_cast<int64_t>(static_cast<uint64_t>(min) + NextU64());

		uint64_t draw = NextU64();
		uint64_t low = draw * range;
		if (low < range)
		{
			const uint64_t threshold = (uint64_t{ 0 } - range) % range; // (2^64 - range) % range
			while (low < threshold)
			{
				draw = NextU64();
				low = draw * range;
			}
		}
		return static_cast<int64_t>(static_cast<uint64_t>(min) + Utils::MultiplyHigh(draw, range));
	}

	double Random::RangeDouble(double min, double max)
	{
		ENGINE_CORE_ASSERT(std::isfinite(min) && std::isfinite(max) && min <= max,
			"Random::RangeDouble needs finite bounds with min <= max, got [{}, {})", min, max);

		const double unit = NextDouble();
		if (min == max)
			return min;
		// The documented formula (Random.h): every operation is a separately rounded IEEE operation (no contraction).
		const double half = 0.5 * max - 0.5 * min;
		const double result = (min + half * unit) + half * unit;
		return result < max ? result : std::nextafter(max, min);
	}

	bool Random::NextBool(double probability)
	{
		return NextDouble() < std::clamp(probability, 0.0, 1.0);
	}

	void Random::SetState(const State& state)
	{
		ENGINE_CORE_ASSERT(state != State{}, "Random::SetState needs a state that is not all zero (xoshiro256** would only output 0)");
		m_State = state;
	}

}
