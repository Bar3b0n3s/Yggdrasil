#include "TestsPCH.h"

#include "Engine/Core/Random.h"

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

	TEST_SUITE("Core")
	{
		TEST_CASE("Random: SplitMix64 seeding and xoshiro256** match the reference sequence" * doctest::skip(true))
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

		TEST_CASE("Random: NextU32, NextDouble and NextFloat are exact functions of NextU64" * doctest::skip(true))
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

		TEST_CASE("Random: RangeDouble follows the documented formula and consumes one draw" * doctest::skip(true))
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

		TEST_CASE("Random: RangeDouble stays below max and finite at the edges of the double range" * doctest::skip(true))
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

		TEST_CASE("Random: RangeInt is inclusive and follows the documented sequence" * doctest::skip(true))
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

		TEST_CASE("Random: unit draws stay in [0, 1) and ranges in [min, max)" * doctest::skip(true))
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

		TEST_CASE("Random: copies and restored states continue the same stream" * doctest::skip(true))
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

		TEST_CASE("Random: Seed restarts the stream" * doctest::skip(true))
		{
			Random random(5);
			const uint64_t first = random.NextU64();
			static_cast<void>(random.NextU64());
			random.Seed(5);
			CHECK(random.NextU64() == first);
		}

		TEST_CASE("Random: NextBool clamps its probability" * doctest::skip(true))
		{
			Random random(8);
			for (int index = 0; index < 1000; ++index)
			{
				REQUIRE(random.NextBool(1.5));
				REQUIRE_FALSE(random.NextBool(-0.5));
				REQUIRE_FALSE(random.NextBool(0.0));
			}
		}
	}

}
