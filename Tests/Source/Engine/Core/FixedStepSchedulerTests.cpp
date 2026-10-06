#include "TestsPCH.h"

#include "Engine/Core/FixedStepScheduler.h"

namespace Engine {

	namespace {

		// One frame fed to the same scheduler, in order, and what it must produce (FixedStepScheduler.h rules 1-5).
		struct FrameRow
		{
			double RealDelta = 0.0;
			double TimeScale = 1.0;
			uint32_t StepCount = 0;
			double Alpha = 0.0;
			double DroppedSeconds = 0.0;
			uint64_t FirstTick = 0;
		};

	}

	static constexpr double Fixed = 1.0 / 60.0;

	// 60 Hz, at most 5 steps per frame, frame deltas clamped to 0.25 s. Rows run in sequence, so each one starts from the
	// accumulator the previous rows left behind.
	// clang-format off
	static const FrameRow FrameTable[] = {
		// realDelta     scale   steps alpha                dropped             firstTick
		{ 0.0,           1.0,    0,    0.0,                 0.0,                0 },  // an empty frame
		{ Fixed,         1.0,    1,    0.0,                 0.0,                0 },  // exactly one step
		{ Fixed / 2.0,   1.0,    0,    0.5,                 0.0,                1 },  // half a step accumulates
		{ Fixed / 2.0,   1.0,    1,    0.0,                 0.0,                1 },  // the other half completes it
		{ 2.0 * Fixed,   1.0,    2,    0.0,                 0.0,                2 },  // two steps
		{ 0.05,          1.0,    3,    0.0,                 0.0,                4 },  // 3 x 1/60 written in decimal
		{ Fixed / 4.0,   1.0,    0,    0.25,                0.0,                7 },  // zero-step frames ...
		{ Fixed / 4.0,   1.0,    0,    0.5,                 0.0,                7 },
		{ Fixed / 4.0,   1.0,    0,    0.75,                0.0,                7 },
		{ Fixed / 4.0,   1.0,    1,    0.0,                 0.0,                7 },  // ... until a whole step accumulated
		{ 0.1,           1.0,    5,    0.0,                 Fixed,              8 },  // 6 steps due, capped at 5: one dropped
		{ 0.25,          1.0,    5,    0.0,                 10.0 * Fixed,       13 }, // the cap drops 10 whole steps
		{ 0.5,           1.0,    5,    0.0,                 0.25 + 10.0 * Fixed, 18 }, // clamped to 0.25, then capped
		{ 1.0,           1.0,    5,    0.0,                 0.75 + 10.0 * Fixed, 23 }, // a one-second hitch
		{ Fixed,         0.0,    0,    0.0,                 0.0,                28 }, // paused: time scale 0
		{ 0.1,           0.0,    0,    0.0,                 0.0,                28 },
		{ Fixed,         0.5,    0,    0.5,                 0.0,                28 }, // slow motion
		{ Fixed,         0.5,    1,    0.0,                 0.0,                28 },
		{ Fixed,         2.0,    2,    0.0,                 0.0,                29 }, // fast forward
		{ Fixed,         3.0,    3,    0.0,                 0.0,                31 },
		{ Fixed,         5.0,    5,    0.0,                 0.0,                34 }, // exactly at the cap
		{ Fixed,         10.0,   5,    0.0,                 5.0 * Fixed,        39 }, // over the cap
		{ 0.01,          1.0,    0,    0.6,                 0.0,                44 }, // 100 Hz frames
		{ 0.01,          1.0,    1,    0.2,                 0.0,                44 },
		{ 0.01,          1.0,    0,    0.8,                 0.0,                45 },
		{ 1.0 / 144.0,   1.0,    1,    13.0 / 60.0,         0.0,                45 }, // 144 Hz frames
		{ 1.0 / 144.0,   1.0,    0,    38.0 / 60.0,         0.0,                46 },
		{ 1.0 / 144.0,   1.0,    1,    0.05,                0.0,                46 },
		{ 0.0,           1.0,    0,    0.05,                0.0,                47 }, // an empty frame keeps Alpha
		{ 0.02,          1.0,    1,    0.25,                0.0,                47 }, // 50 Hz frames
		{ 0.02,          1.0,    1,    0.45,                0.0,                48 },
		{ 0.02,          1.0,    1,    0.65,                0.0,                49 },
		{ 0.3,           0.5,    5,    0.15,                0.075,              50 }, // clamp and cap at half speed
		{ 0.2,           0.25,   3,    0.15,                0.0,                55 }, // quarter speed
		{ Fixed,         1.0,    1,    0.15,                0.0,                58 }, // the remainder carries over
	};
	// clang-format on

	TEST_SUITE("Core")
	{
		TEST_CASE("FixedStepScheduler: table of frame deltas yields expected step counts and alpha" * doctest::skip(true))
		{
			static_assert(std::size(FrameTable) >= 30, "Roadmap M1 requires at least 30 rows");

			FixedStepScheduler scheduler(FrameLoopConfig{});
			for (size_t row = 0; row < std::size(FrameTable); ++row)
			{
				const FrameRow& expected = FrameTable[row];
				const FrameSteps steps = scheduler.Advance(expected.RealDelta, expected.TimeScale);

				INFO("row ", row, ": realDelta ", expected.RealDelta, ", timeScale ", expected.TimeScale);
				CHECK(steps.StepCount == expected.StepCount);
				CHECK(steps.FirstTick == expected.FirstTick);
				CHECK(steps.Alpha == doctest::Approx(expected.Alpha).epsilon(1e-9));
				CHECK(steps.DroppedSeconds == doctest::Approx(expected.DroppedSeconds).epsilon(1e-9));
				CHECK(scheduler.GetTick() == expected.FirstTick + expected.StepCount);
			}
			CHECK(scheduler.GetTick() == 59);
		}

		TEST_CASE("FixedStepScheduler: StepExactly runs the requested steps with Alpha 1" * doctest::skip(true))
		{
			FixedStepScheduler scheduler(FrameLoopConfig{});
			const FrameSteps half = scheduler.Advance(Fixed / 2.0, 1.0);
			CHECK(half.StepCount == 0);

			const FrameSteps manual = scheduler.StepExactly(1);
			CHECK(manual.StepCount == 1);
			CHECK(manual.Alpha == 1.0);
			CHECK(manual.DroppedSeconds == 0.0);
			CHECK(manual.FirstTick == 0);

			const FrameSteps lockstep = scheduler.StepExactly(7);
			CHECK(lockstep.StepCount == 7);
			CHECK(lockstep.FirstTick == 1);
			CHECK(scheduler.GetTick() == 8);

			// The accumulator was not touched: the half step from before completes on the next real frame.
			const FrameSteps next = scheduler.Advance(Fixed / 2.0, 1.0);
			CHECK(next.StepCount == 1);
			CHECK(next.FirstTick == 8);
		}

		TEST_CASE("FixedStepScheduler: Reset returns to tick 0 with an empty accumulator" * doctest::skip(true))
		{
			FixedStepScheduler scheduler(FrameLoopConfig{});
			static_cast<void>(scheduler.Advance(2.5 * Fixed, 1.0));
			CHECK(scheduler.GetTick() == 2);

			scheduler.Reset();
			CHECK(scheduler.GetTick() == 0);
			const FrameSteps steps = scheduler.Advance(Fixed / 2.0, 1.0);
			CHECK(steps.StepCount == 0);
			CHECK(steps.Alpha == doctest::Approx(0.5));
			CHECK(steps.FirstTick == 0);
		}

		TEST_CASE("FixedStepScheduler: honours the configured rate, step cap and frame clamp" * doctest::skip(true))
		{
			FrameLoopConfig config;
			config.FixedHz = 50;
			config.MaxStepsPerFrame = 3;
			config.MaxFrameDelta = 0.1;
			FixedStepScheduler scheduler(config);
			CHECK(scheduler.GetFixedDelta() == doctest::Approx(0.02));

			const FrameSteps clamped = scheduler.Advance(0.5, 1.0);
			CHECK(clamped.StepCount == 3);
			CHECK(clamped.DroppedSeconds == doctest::Approx(0.4 + 0.04)); // 0.4 clamped off, then 2 whole steps over the cap
			CHECK(clamped.Alpha == doctest::Approx(0.0).epsilon(1e-9));

			const FrameSteps normal = scheduler.Advance(0.03, 1.0);
			CHECK(normal.StepCount == 1);
			CHECK(normal.Alpha == doctest::Approx(0.5));
		}

		TEST_CASE("FixedStepScheduler: GetSimStep time is tick times the fixed delta" * doctest::skip(true))
		{
			const FixedStepScheduler scheduler(FrameLoopConfig{});
			const SimStep step = scheduler.GetSimStep(600);
			CHECK(step.Tick == 600);
			CHECK(step.FixedDelta == Fixed);
			CHECK(step.Time == 600.0 * Fixed);
		}

		TEST_CASE("FrameLoopConfig: defaults are 60 Hz, 5 steps per frame and a 0.25 s clamp")
		{
			constexpr FrameLoopConfig DefaultConfig;
			static_assert(DefaultConfig.FixedHz == 60);
			static_assert(DefaultConfig.MaxStepsPerFrame == 5);
			static_assert(DefaultConfig.MaxFrameDelta == 0.25);
			CHECK(DefaultConfig.GetFixedDelta() == Fixed);
		}
	}

}
