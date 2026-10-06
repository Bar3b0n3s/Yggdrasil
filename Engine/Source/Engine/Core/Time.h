#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>

// Time values passed explicitly to simulation and frame code (Architecture §4.2, §4.12). There is no global time:
// simulated state depends only on SimStep, never on the wall clock.

namespace Engine {

	// One fixed simulation step. Tick counts steps from 0 (the first step of a session is tick 0); Time is the
	// simulation time at the start of the step and always equals Tick * FixedDelta (use FromTick to build one).
	struct SimStep
	{
		uint64_t Tick = 0;
		double FixedDelta = 0.0; // seconds, 1 / FixedHz
		double Time = 0.0;       // seconds, Tick * FixedDelta

		[[nodiscard]] static constexpr SimStep FromTick(uint64_t tick, double fixedDelta)
		{
			return SimStep{ .Tick = tick, .FixedDelta = fixedDelta, .Time = static_cast<double>(tick) * fixedDelta };
		}
	};

	// One rendered frame. Frame values may depend on the wall clock and must never feed simulated state.
	struct FrameTime
	{
		double DeltaTime = 0.0;         // seconds since the previous frame, scaled by the time scale
		double UnscaledDeltaTime = 0.0; // seconds since the previous frame as the clock reported it
		double Alpha = 0.0;             // interpolation factor in [0, 1] between the last two simulated states (§5.2)
		uint64_t FrameIndex = 0;        // frames since the loop started, from 0
	};

}
