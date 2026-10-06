#pragma once

#include "Engine/Core/Base.h"

#include <array>
#include <cstdint>

namespace Engine {

	// Seeded pseudo-random numbers: xoshiro256** (Blackman and Vigna), the only randomness on the simulation path
	// (Architecture §4.12). A play session owns one stream seeded with Project.Simulation.Seed ^ Scene.Seed; Luau
	// math.random and Random.* draw from it. Every result is a pure function of the seed and the call sequence, identical
	// on every platform and configuration (integer arithmetic only; floating-point results are exact conversions).
	// A value type: copy it to fork a stream. Not thread-safe (one owner).
	class Random
	{
	public:
		using State = std::array<uint64_t, 4>;

		// Seed(seed).
		explicit Random(uint64_t seed);

		// Restarts the stream: the state becomes the next four outputs of SplitMix64 started at `seed`
		// (x += 0x9e3779b97f4a7c15; z = x; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9; z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
		// output z ^ (z >> 31)). Every seed, 0 included, gives a valid (non-zero) state.
		void Seed(uint64_t seed);

		// The next raw 64-bit output of xoshiro256**.
		[[nodiscard]] uint64_t NextU64();

		// The upper 32 bits of NextU64().
		[[nodiscard]] uint32_t NextU32();

		// Uniform in [0, 1): (NextU64() >> 11) * 2^-53.
		[[nodiscard]] double NextDouble();

		// Uniform in [0, 1): (NextU64() >> 40) * 2^-24.
		[[nodiscard]] float NextFloat();

		// Uniform integer in [min, max], both inclusive and without modulo bias, by Lemire's nearly divisionless method
		// (fixed here because the sequence is part of the determinism contract): range = max - min + 1 as uint64_t (0
		// for the full 64-bit range, which returns min + NextU64()); x = NextU64(); m = x * range as a 128-bit product;
		// if low64(m) < range, then while low64(m) < (2^64 - range) % range, draw x again and recompute m; the result
		// is min + high64(m). min <= max is a precondition (asserted); callers validate script and automation input.
		[[nodiscard]] int64_t RangeInt(int64_t min, int64_t max);

		// Uniform in [min, max) for min < max, finite for every finite pair; min when min == max (u is still drawn, so
		// every call consumes exactly one NextU64). Both must be finite and min <= max (asserted). Exactly, for min < max
		// and with u = NextDouble():
		//     half = 0.5 * max - 0.5 * min;   // finite even when max - min overflows
		//     r = (min + half * u) + half * u;
		//     if (!(r < max)) r = std::nextafter(max, min);   // rounding can reach max (or +Inf next to DBL_MAX)
		// (evaluated in that order without contraction, §2.2; std::nextafter is exact).
		[[nodiscard]] double RangeDouble(double min, double max);

		// true with probability `probability` (NextDouble() < probability); values outside [0, 1] are clamped.
		[[nodiscard]] bool NextBool(double probability = 0.5);

		// The raw generator state, for snapshots that must resume the exact stream (play-session copies, replays).
		[[nodiscard]] const State& GetState() const { return m_State; }
		// Restores a state obtained from GetState(). An all-zero state is invalid for xoshiro (asserted).
		void SetState(const State& state);
	private:
		State m_State{};
	};

}
