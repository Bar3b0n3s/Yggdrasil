#include "TestsPCH.h"

#include "Engine/Core/Clock.h"

#include <limits>

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("ScriptedClock: cycles its delta table" * doctest::skip(true))
		{
			const std::array<double, 3> table = { 0.0, 1.0 / 60.0, 0.1 };
			Result<ScriptedClock> created = ScriptedClock::Create(table);
			REQUIRE(created.has_value());
			ScriptedClock& clock = *created;
			CHECK(clock.GetKind() == ClockKind::Scripted);

			for (int cycle = 0; cycle < 3; ++cycle)
			{
				CHECK(clock.Delta() == 0.0);
				CHECK(clock.Delta() == 1.0 / 60.0);
				CHECK(clock.Delta() == 0.1);
			}
			CHECK(clock.GetCallCount() == 9);
			CHECK(clock.GetDeltas().size() == 3);
		}

		TEST_CASE("ScriptedClock: Create rejects an empty table and negative or non-finite deltas" * doctest::skip(true))
		{
			CHECK_FALSE(ScriptedClock::Create({}).has_value());

			const std::array<double, 2> negative = { 0.1, -0.01 };
			const Result<ScriptedClock> withNegative = ScriptedClock::Create(negative);
			REQUIRE_FALSE(withNegative.has_value());
			CHECK(withNegative.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(withNegative.error().GetMessageText().contains("1"));

			const std::array<double, 1> infinite = { std::numeric_limits<double>::infinity() };
			CHECK_FALSE(ScriptedClock::Create(infinite).has_value());

			const std::array<double, 1> notANumber = { std::numeric_limits<double>::quiet_NaN() };
			CHECK_FALSE(ScriptedClock::Create(notANumber).has_value());
		}

		TEST_CASE("ManualClock: every call returns the fixed delta" * doctest::skip(true))
		{
			ManualClock clock(1.0 / 60.0);
			CHECK(clock.GetKind() == ClockKind::Manual);
			CHECK(clock.GetFixedDelta() == 1.0 / 60.0);
			for (int index = 0; index < 5; ++index)
				CHECK(clock.Delta() == 1.0 / 60.0);
		}

		TEST_CASE("SystemClock: the first delta is 0 and later deltas are finite and non-negative" * doctest::skip(true))
		{
			SystemClock clock;
			CHECK(clock.GetKind() == ClockKind::System);
			CHECK(clock.Delta() == 0.0);
			const double next = clock.Delta();
			CHECK(next >= 0.0);
			CHECK(next < std::numeric_limits<double>::infinity());
		}
	}

}
