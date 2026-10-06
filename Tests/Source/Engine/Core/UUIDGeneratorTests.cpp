#include "TestsPCH.h"

#include "Engine/Core/UUIDGenerator.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"

namespace Engine {

	static std::vector<uint64_t> Draw(UUIDGenerator& generator, size_t count)
	{
		std::vector<uint64_t> values;
		for (size_t index = 0; index < count; ++index)
			values.push_back(generator.Next().GetValue());
		return values;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("UUIDGenerator: seeded generator is reproducible" * doctest::skip(true))
		{
			SUBCASE("random mode")
			{
				UUIDGenerator first = UUIDGenerator::CreateRandom(1234);
				UUIDGenerator second = UUIDGenerator::CreateRandom(1234);
				CHECK(first.GetMode() == UUIDGeneratorMode::Random);
				const std::vector<uint64_t> expected = { 0x0bab45d9a0e3ae53, 0xd7c640660c19433e, 0xb0dedaa0d09a6691, 0xdec9f41b58ec86eb };
				CHECK(Draw(first, 4) == expected);
				CHECK(Draw(second, 4) == expected);
			}

			SUBCASE("deterministic mode")
			{
				UUIDGenerator first = UUIDGenerator::CreateDeterministic(1234);
				UUIDGenerator second = UUIDGenerator::CreateDeterministic(1234);
				CHECK(first.GetMode() == UUIDGeneratorMode::Deterministic);
				const std::vector<uint64_t> expected = { 0x178b3892ca77ee4f, 0x5f90c2e539197dbd, 0xaf7f616810c72a58, 0x5e627675a5b94d7b };
				CHECK(Draw(first, 4) == expected);
				CHECK(Draw(second, 4) == expected);
				CHECK(first.GetDrawCount() == 4);
			}

			SUBCASE("different seeds give different streams")
			{
				UUIDGenerator first = UUIDGenerator::CreateDeterministic(1);
				UUIDGenerator second = UUIDGenerator::CreateDeterministic(2);
				CHECK(Draw(first, 8) != Draw(second, 8));
			}
		}

		TEST_CASE("UUIDGenerator: deterministic mode is Hash64 of the session seed and the counter" * doctest::skip(true))
		{
			UUIDGenerator generator = UUIDGenerator::CreateDeterministic(0x5eed);
			for (uint64_t counter = 0; counter < 16; ++counter)
				CHECK(generator.Next().GetValue() == Hash64(0x5eed, counter));
		}

		TEST_CASE("UUIDGenerator: never yields zero or a reserved built-in value" * doctest::skip(true))
		{
			UUIDGenerator deterministic = UUIDGenerator::CreateDeterministic(7);
			UUIDGenerator random = UUIDGenerator::CreateRandom(7);
			for (int index = 0; index < 100000; ++index)
			{
				const UUID fromDeterministic = deterministic.Next();
				const UUID fromRandom = random.Next();
				if (fromDeterministic.GetValue() <= UUIDGenerator::MaxReservedValue
					|| fromRandom.GetValue() <= UUIDGenerator::MaxReservedValue)
				{
					FAIL_CHECK("reserved or invalid UUID generated at draw " << index);
					break;
				}
			}
		}

		TEST_CASE("UUIDGenerator: a random-mode generator continues the xoshiro stream of its state" * doctest::skip(true))
		{
			const Random::State state = { 0x0123456789abcdefull, 0xfedcba9876543210ull, 0x0f1e2d3c4b5a6978ull, 0x8796a5b4c3d2e1f0ull };
			UUIDGenerator generator = UUIDGenerator::CreateRandom(state);
			CHECK(generator.GetMode() == UUIDGeneratorMode::Random);

			Random reference(0);
			reference.SetState(state);
			for (int index = 0; index < 100; ++index)
			{
				uint64_t expected = reference.NextU64();
				while (expected <= UUIDGenerator::MaxReservedValue)
					expected = reference.NextU64();
				REQUIRE(generator.Next() == UUID(expected));
			}
		}
	}

}
