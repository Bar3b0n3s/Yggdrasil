#include "TestsPCH.h"

#include "Engine/Platform/SecureRandom.h"

namespace Engine {

	TEST_SUITE("Platform")
	{
		TEST_CASE("SecureRandom: fills the whole buffer and two draws differ" * doctest::skip(true))
		{
			// 64 bytes of zeros, or two equal draws, happen with probability 2^-512: a failure means a broken generator.
			const std::array<std::byte, 64> zeros{};
			std::array<std::byte, 64> first{};
			std::array<std::byte, 64> second{};
			REQUIRE(SecureRandom::Fill(first).has_value());
			REQUIRE(SecureRandom::Fill(second).has_value());
			CHECK(first != zeros);
			CHECK(first != second);

			// An empty buffer is fine.
			CHECK(SecureRandom::Fill({}).has_value());
		}

		TEST_CASE("SecureRandom: GenerateState gives a valid xoshiro state" * doctest::skip(true))
		{
			const Result<Random::State> state = SecureRandom::GenerateState();
			REQUIRE(state.has_value());
			const Random::State zero{};
			CHECK(*state != zero);

			// It seeds a Random stream directly.
			Random random(0);
			random.SetState(*state);
			CHECK(random.GetState() == *state);
		}
	}

}
