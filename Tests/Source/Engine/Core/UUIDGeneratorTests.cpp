#include "TestsPCH.h"

#include "Engine/Core/UUIDGenerator.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Support/DeathTest.h"

namespace Engine {

	static std::vector<uint64_t> Draw(UUIDGenerator& generator, size_t count)
	{
		std::vector<uint64_t> values;
		for (size_t index = 0; index < count; ++index)
			values.push_back(generator.Next().GetValue());
		return values;
	}

	ENGINE_DEATH_TEST("Core/UUIDGeneratorZeroState")
	{
		UUIDGenerator generator = UUIDGenerator::CreateRandom(Random::State{});
		static_cast<void>(generator.Next());
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("UUIDGenerator: seeded generator is reproducible")
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

		TEST_CASE("UUIDGenerator: deterministic mode is Hash64 of the session seed and the counter")
		{
			UUIDGenerator generator = UUIDGenerator::CreateDeterministic(0x5eed);
			for (uint64_t counter = 0; counter < 16; ++counter)
				CHECK(generator.Next().GetValue() == Hash64(0x5eed, counter));
		}

		TEST_CASE("UUIDGenerator: never yields zero or a reserved built-in value")
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

		TEST_CASE("UUIDGenerator: a random-mode generator continues the xoshiro stream of its state")
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

		TEST_CASE("UUIDGenerator: a reserved output is skipped and still counted as a draw")
		{
			// The first xoshiro256** output is rotl(s1 * 5, 7) * 9. With s1 = rotr(5 * 9^-1, 7) * 5^-1 (mod 2^64) it is 5, a
			// reserved built-in value.
			const Random::State state = { 0x0123456789abcdefull, 0xd8b60b60b60b60b6ull, 0x0f1e2d3c4b5a6978ull, 0x8796a5b4c3d2e1f0ull };
			Random reference(0);
			reference.SetState(state);
			REQUIRE(reference.NextU64() == 5);
			const uint64_t second = reference.NextU64();

			UUIDGenerator generator = UUIDGenerator::CreateRandom(state);
			CHECK(generator.Next() == UUID(second));
			CHECK(generator.GetDrawCount() == 2);
			CHECK(generator.Next() == UUID(reference.NextU64()));
			CHECK(generator.GetDrawCount() == 3);
		}

		TEST_CASE("UUIDGenerator: copies continue the same sequence")
		{
			UUIDGenerator original = UUIDGenerator::CreateDeterministic(99);
			static_cast<void>(original.Next());
			UUIDGenerator copy = original;
			CHECK(copy.Next() == original.Next());
			CHECK(copy.GetDrawCount() == original.GetDrawCount());

			UUIDGenerator random = UUIDGenerator::CreateRandom(99);
			UUIDGenerator randomCopy = random;
			CHECK(randomCopy.Next() == random.Next());
		}

		TEST_CASE("UUIDGenerator: CreateRandom with an all-zero state is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/UUIDGeneratorZeroState", "Random::SetState needs a state that is not all zero");
		}
	}

}
