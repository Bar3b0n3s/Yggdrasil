#include "TestsPCH.h"

#include "Engine/Core/Random.h"

#include "Support/DeathTest.h"

#include <cmath>
#include <limits>

namespace Engine {

	namespace {

		struct EdgeRow
		{
			double Min = 0.0;
			double Max = 0.0;
		};

	}

	// The documented RangeDouble formula for min < max (Random.h), applied to a known unit draw.
	static double ReferenceRangeDouble(double min, double max, double unit)
	{
		const double half = 0.5 * max - 0.5 * min;
		const double result = (min + half * unit) + half * unit;
		return result < max ? result : std::nextafter(max, min);
	}

	ENGINE_DEATH_TEST("Core/RandomZeroState")
	{
		Random random(1);
		random.SetState(Random::State{});
	}

	ENGINE_DEATH_TEST("Core/RandomRangeIntReversed")
	{
		Random random(1);
		static_cast<void>(random.RangeInt(5, 4));
	}

	ENGINE_DEATH_TEST("Core/RandomRangeDoubleNotFinite")
	{
		Random random(1);
		static_cast<void>(random.RangeDouble(0.0, std::numeric_limits<double>::infinity()));
	}

	ENGINE_DEATH_TEST("Core/RandomRangeDoubleReversed")
	{
		Random random(1);
		static_cast<void>(random.RangeDouble(2.0, 1.0));
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Random: SplitMix64 seeding and xoshiro256** match the reference sequence")
		{
			SUBCASE("xoshiro256** from the state {1, 2, 3, 4}")
			{
				Random random(0);
				random.SetState({ 1, 2, 3, 4 });
				CHECK(random.NextU64() == 11520ull);
				CHECK(random.NextU64() == 0ull);
				CHECK(random.NextU64() == 1509978240ull);
				CHECK(random.NextU64() == 1215971899390074240ull);
			}

			SUBCASE("seed 0")
			{
				Random random(0);
				const Random::State expected = {
					0xe220a8397b1dcdafull,
					0x6e789e6aa1b965f4ull,
					0x06c45d188009454full,
					0xf88bb8a8724c81ecull,
				};
				CHECK(random.GetState() == expected);
				CHECK(random.NextU64() == 0x99ec5f36cb75f2b4ull);
				CHECK(random.NextU64() == 0xbf6e1f784956452aull);
				CHECK(random.NextU64() == 0x1a5f849d4933e6e0ull);
			}

			SUBCASE("seed 42")
			{
				Random random(42);
				CHECK(random.NextU64() == 0x15780b2e0c2ec716ull);
				CHECK(random.NextU64() == 0x6104d9866d113a7eull);
				CHECK(random.NextU64() == 0xae17533239e499a1ull);
				CHECK(random.NextU64() == 0xecb8ad4703b360a1ull);
				CHECK(random.NextU64() == 0xfde6dc7fe2ec5e64ull);
			}
		}

		TEST_CASE("Random: NextU32, NextDouble and NextFloat are exact functions of NextU64")
		{
			Random random(42);
			Random reference(42);

			CHECK(random.NextU32() == static_cast<uint32_t>(reference.NextU64() >> 32));
			CHECK(random.NextDouble() == static_cast<double>(reference.NextU64() >> 11) * 0x1.0p-53);
			CHECK(random.NextFloat() == static_cast<float>(reference.NextU64() >> 40) * 0x1.0p-24f);

			Random seeded(42);
			CHECK(seeded.NextDouble() == 0.08386297105988216);
			CHECK(seeded.NextDouble() == 0.3789802506626686);
		}

		TEST_CASE("Random: RangeDouble follows the documented formula and consumes one draw")
		{
			Random random(42);
			Random reference(42);
			for (int index = 0; index < 1000; ++index)
			{
				const double unit = reference.NextDouble();
				REQUIRE(random.RangeDouble(-3.0, 5.0) == ReferenceRangeDouble(-3.0, 5.0, unit));
			}

			CHECK(random.RangeDouble(2.5, 2.5) == 2.5); // consumes a draw like every other call
			static_cast<void>(reference.NextU64());
			CHECK(random.GetState() == reference.GetState());
		}

		TEST_CASE("Random: RangeDouble stays below max and finite at the edges of the double range")
		{
			constexpr double Largest = std::numeric_limits<double>::max();
			const std::array<EdgeRow, 5> rows = {
				EdgeRow{ 1.0, std::nextafter(1.0, 2.0) }, // (max - min) * u can round up to max
				EdgeRow{ -1.0e308, 1.0e308 },             // max - min overflows to +Inf
				EdgeRow{ -Largest, Largest },
				EdgeRow{ 0.0, Largest }, // rounding next to DBL_MAX could overflow
				EdgeRow{ -Largest, -Largest / 2.0 },
			};
			for (const EdgeRow& row : rows)
			{
				Random random(123);
				for (int index = 0; index < 10000; ++index)
				{
					const double value = random.RangeDouble(row.Min, row.Max);
					INFO("range [", row.Min, ", ", row.Max, ") draw ", index, ": ", value);
					REQUIRE(std::isfinite(value));
					REQUIRE(value >= row.Min);
					REQUIRE(value < row.Max);
				}
			}

			// The one double in [1, nextafter(1, 2)) is 1.
			Random narrow(5);
			for (int index = 0; index < 1000; ++index)
				REQUIRE(narrow.RangeDouble(1.0, std::nextafter(1.0, 2.0)) == 1.0);
		}

		TEST_CASE("Random: RangeInt is inclusive and follows the documented sequence")
		{
			Random dice(42);
			std::vector<int64_t> rolls;
			for (int index = 0; index < 10; ++index)
				rolls.push_back(dice.RangeInt(1, 6));
			CHECK(rolls == std::vector<int64_t>{ 1, 3, 5, 6, 6, 5, 5, 6, 5, 4 });

			Random signedRange(7);
			std::vector<int64_t> values;
			for (int index = 0; index < 8; ++index)
				values.push_back(signedRange.RangeInt(-100, 100));
			CHECK(values == std::vector<int64_t>{ 40, -44, 68, 97, 99, 75, -88, -80 });

			Random single(3);
			CHECK(single.RangeInt(5, 5) == 5);
			CHECK(single.RangeInt(-7, -7) == -7);

			Random bounds(11);
			bool sawMin = false;
			bool sawMax = false;
			for (int index = 0; index < 10000; ++index)
			{
				const int64_t value = bounds.RangeInt(-2, 2);
				REQUIRE(value >= -2);
				REQUIRE(value <= 2);
				sawMin = sawMin || value == -2;
				sawMax = sawMax || value == 2;
			}
			CHECK(sawMin);
			CHECK(sawMax);
		}

		TEST_CASE("Random: RangeInt handles rejection-heavy ranges and the full 64-bit range")
		{
			constexpr int64_t Lowest = std::numeric_limits<int64_t>::min();
			constexpr int64_t Highest = std::numeric_limits<int64_t>::max();

			// range = 2^63 + 1: the rejection threshold is 2^63 - 1, so about half of the raw draws are redrawn.
			Random rejecting(2024);
			std::vector<int64_t> values;
			for (int index = 0; index < 8; ++index)
				values.push_back(rejecting.RangeInt(Lowest, 0));
			CHECK(values
				== std::vector<int64_t>{ -8558782517560793088, -7750253146861907403, -6964825331548575110, -2579363550537711120,
					-1088382908826051773, -5844622144122417222, -7471795543327304574, -8828121625229511036 });

			// The full range wraps to range == 0: the result is min + NextU64() (modulo 2^64).
			Random full(9);
			Random reference(9);
			for (int index = 0; index < 100; ++index)
				REQUIRE(full.RangeInt(Lowest, Highest) == static_cast<int64_t>(static_cast<uint64_t>(Lowest) + reference.NextU64()));
			Random fullAgain(9);
			CHECK(fullAgain.RangeInt(Lowest, Highest) == -9175715986142551968);

			// Bounds at the extremes of int64_t.
			Random extremes(4);
			for (int index = 0; index < 1000; ++index)
			{
				const int64_t high = extremes.RangeInt(Highest - 2, Highest);
				const int64_t low = extremes.RangeInt(Lowest, Lowest + 2);
				REQUIRE(high >= Highest - 2);
				REQUIRE(low <= Lowest + 2);
			}
		}

		TEST_CASE("Random: unit draws stay in [0, 1) and ranges in [min, max)")
		{
			Random random(99);
			for (int index = 0; index < 10000; ++index)
			{
				const double unitDouble = random.NextDouble();
				const float unitFloat = random.NextFloat();
				const double ranged = random.RangeDouble(-3.0, 5.0);
				REQUIRE(unitDouble >= 0.0);
				REQUIRE(unitDouble < 1.0);
				REQUIRE(unitFloat >= 0.0f);
				REQUIRE(unitFloat < 1.0f);
				REQUIRE(ranged >= -3.0);
				REQUIRE(ranged < 5.0);
			}
			CHECK(random.RangeDouble(2.5, 2.5) == 2.5);
		}

		TEST_CASE("Random: copies and restored states continue the same stream")
		{
			Random original(2024);
			static_cast<void>(original.NextU64());

			Random copy = original;
			const Random::State saved = original.GetState();
			const uint64_t next = original.NextU64();

			CHECK(copy.NextU64() == next);

			Random restored(0);
			restored.SetState(saved);
			CHECK(restored.NextU64() == next);
		}

		TEST_CASE("Random: Seed restarts the stream")
		{
			Random random(5);
			const uint64_t first = random.NextU64();
			static_cast<void>(random.NextU64());
			random.Seed(5);
			CHECK(random.NextU64() == first);
		}

		TEST_CASE("Random: NextBool clamps its probability")
		{
			Random random(8);
			for (int index = 0; index < 1000; ++index)
			{
				REQUIRE(random.NextBool(1.5));
				REQUIRE_FALSE(random.NextBool(-0.5));
				REQUIRE_FALSE(random.NextBool(0.0));
			}
		}

		TEST_CASE("Random: an all-zero state is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/RandomZeroState", "Random::SetState needs a state that is not all zero");
		}

		TEST_CASE("Random: RangeInt with min greater than max is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/RandomRangeIntReversed", "Random::RangeInt needs min <= max, got [5, 4]");
		}

		TEST_CASE("Random: RangeDouble with a non-finite or reversed range is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/RandomRangeDoubleNotFinite", "Random::RangeDouble needs finite bounds with min <= max, got [0, inf)");
			ENGINE_CHECK_DEATH("Core/RandomRangeDoubleReversed", "Random::RangeDouble needs finite bounds with min <= max, got [2, 1)");
		}
	}

}
