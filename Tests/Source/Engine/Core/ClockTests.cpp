#include "TestsPCH.h"

#include "Engine/Core/Clock.h"

#include "Support/DeathTest.h"

#include <limits>

namespace Engine {

	ENGINE_DEATH_TEST("Core/ManualClockZeroDelta")
	{
		ManualClock clock(0.0);
		static_cast<void>(clock.Delta());
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("ScriptedClock: cycles its delta table")
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

		TEST_CASE("ScriptedClock: Create rejects an empty table and negative or non-finite deltas")
		{
			CHECK_FALSE(ScriptedClock::Create({}).has_value());

			const std::array<double, 2> negative = { 0.1, -0.01 };
			const Result<ScriptedClock> withNegative = ScriptedClock::Create(negative);
			REQUIRE_FALSE(withNegative.has_value());
			CHECK(withNegative.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(withNegative.error().GetMessageText().contains("frame delta 1 is -0.01"));

			const std::array<double, 1> infinite = { std::numeric_limits<double>::infinity() };
			CHECK_FALSE(ScriptedClock::Create(infinite).has_value());

			const std::array<double, 1> notANumber = { std::numeric_limits<double>::quiet_NaN() };
			CHECK_FALSE(ScriptedClock::Create(notANumber).has_value());

			const std::array<double, 4> laterNaN = { 0.1, 0.2, 0.3, std::numeric_limits<double>::quiet_NaN() };
			const Result<ScriptedClock> withLaterNaN = ScriptedClock::Create(laterNaN);
			REQUIRE_FALSE(withLaterNaN.has_value());
			CHECK(withLaterNaN.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(withLaterNaN.error().GetMessageText().contains("frame delta 3 is"));
		}

		TEST_CASE("ScriptedClock: keeps its own copy of the table")
		{
			std::vector<double> table = { 0.5, 0.25 };
			Result<ScriptedClock> created = ScriptedClock::Create(table);
			REQUIRE(created.has_value());
			table.assign({ 9.0, 9.0, 9.0 });
			CHECK(created->GetDeltas().size() == 2);
			CHECK(created->Delta() == 0.5);
			CHECK(created->Delta() == 0.25);
			CHECK(created->Delta() == 0.5);
			CHECK(created->GetCallCount() == 3);
		}

		TEST_CASE("ManualClock: every call returns the fixed delta")
		{
			ManualClock clock(1.0 / 60.0);
			CHECK(clock.GetKind() == ClockKind::Manual);
			CHECK(clock.GetFixedDelta() == 1.0 / 60.0);
			for (int index = 0; index < 5; ++index)
				CHECK(clock.Delta() == 1.0 / 60.0);
		}

		TEST_CASE("SystemClock: the first delta is 0 and later deltas are finite and non-negative")
		{
			SystemClock clock;
			CHECK(clock.GetKind() == ClockKind::System);
			CHECK(clock.Delta() == 0.0);
			const double next = clock.Delta();
			CHECK(next >= 0.0);
			CHECK(next < std::numeric_limits<double>::infinity());
		}

		TEST_CASE("ManualClock: a fixed delta that is not finite and positive is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/ManualClockZeroDelta", "ManualClock needs a finite fixed delta > 0, got 0");
		}
	}

}
