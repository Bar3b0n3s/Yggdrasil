#include "TestsPCH.h"

#include "Engine/Core/Time.h"

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("SimStep: FromTick computes the time as tick times the fixed delta")
		{
			constexpr SimStep FirstStep = SimStep::FromTick(0, 1.0 / 60.0);
			static_assert(FirstStep.Tick == 0 && FirstStep.Time == 0.0);

			const SimStep later = SimStep::FromTick(900, 1.0 / 60.0);
			CHECK(later.Tick == 900);
			CHECK(later.FixedDelta == 1.0 / 60.0);
			CHECK(later.Time == 900.0 * (1.0 / 60.0));
		}

		TEST_CASE("FrameTime: defaults to an empty first frame")
		{
			constexpr FrameTime DefaultFrame;
			static_assert(DefaultFrame.DeltaTime == 0.0 && DefaultFrame.UnscaledDeltaTime == 0.0 && DefaultFrame.Alpha == 0.0
				&& DefaultFrame.FrameIndex == 0);
			CHECK(DefaultFrame.FrameIndex == 0);
		}
	}

}
