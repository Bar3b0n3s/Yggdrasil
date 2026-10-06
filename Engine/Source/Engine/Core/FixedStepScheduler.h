#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Time.h"

#include <cstdint>

namespace Engine {

	// Fixed-step loop parameters (Architecture §4.1, §4.2; Project Simulation.FixedHz and MaxStepsPerFrame, §6.1).
	struct FrameLoopConfig
	{
		// The highest accepted FixedHz. It keeps FixedDelta (1e-5 s at the bound) 10,000 times above
		// FixedStepScheduler::StepTolerance, so the step comparison never runs a step for an empty accumulator. The
		// project loader validates Simulation.FixedHz against [1, MaxFixedHz].
		static constexpr uint32_t MaxFixedHz = 100000;

		uint32_t FixedHz = 60;
		uint32_t MaxStepsPerFrame = 5;
		double MaxFrameDelta = 0.25; // seconds; longer frame deltas are clamped to this

		[[nodiscard]] constexpr double GetFixedDelta() const { return 1.0 / static_cast<double>(FixedHz); }
	};

	// The outcome of one frame. Steps FirstTick ... FirstTick + StepCount - 1 run this frame, in order.
	struct FrameSteps
	{
		uint32_t StepCount = 0;
		double Alpha = 0.0;          // interpolation factor in [0, 1]
		double DroppedSeconds = 0.0; // simulation seconds discarded this frame (reported by stats.get)
		uint64_t FirstTick = 0;      // the tick of the first step of this frame (the scheduler's tick before the frame)
	};

	// Decides how many fixed steps each frame runs (Architecture §4.2). Pure and deterministic: the same sequence of calls
	// gives the same results on every platform and configuration; it reads no clock. Not thread-safe (one owner).
	//
	// Advance(realDeltaSeconds, timeScale), with FixedDelta = 1 / FixedHz:
	//   1. clamped = min(realDeltaSeconds, MaxFrameDelta); the clamped-off part, (realDeltaSeconds - clamped) * timeScale,
	//      is dropped.
	//   2. accumulator += clamped * timeScale.
	//   3. While accumulator >= FixedDelta - StepTolerance and fewer than MaxStepsPerFrame steps ran: run a step,
	//      accumulator = max(0, accumulator - FixedDelta). The tolerance absorbs the rounding of deltas that are exact
	//      multiples of FixedDelta in decimal (3 x 1/60 must give 3 steps).
	//   4. If MaxStepsPerFrame steps ran and accumulator >= FixedDelta - StepTolerance still holds, the whole steps left
	//      in it are dropped: dropped += FixedDelta * floor((accumulator + StepTolerance) / FixedDelta), and the
	//      accumulator keeps only the remainder, clamped to [0, FixedDelta).
	//   5. Alpha = clamp(accumulator / FixedDelta, 0, 1).
	// DroppedSeconds is the sum of the dropped parts of steps 1 and 4.
	class FixedStepScheduler
	{
	public:
		// Absolute tolerance of the step comparison, in seconds.
		static constexpr double StepTolerance = 1e-9;

		// FixedHz must be in [1, FrameLoopConfig::MaxFixedHz], MaxStepsPerFrame > 0 and MaxFrameDelta finite and > 0
		// (asserted; the project loader validates the values it reads).
		explicit FixedStepScheduler(const FrameLoopConfig& config);

		// See the class comment. `realDeltaSeconds` and `timeScale` must be finite and >= 0 (asserted; clocks and
		// play.setTimeScale guarantee it); a time scale of 0 runs no steps and keeps Alpha.
		[[nodiscard]] FrameSteps Advance(double realDeltaSeconds, double timeScale);

		// Runs exactly `stepCount` steps without touching the accumulator: Alpha = 1, nothing dropped. Used with
		// ManualClock (one step per frame) and for lockstep play.step requests (§4.2, §13.6).
		[[nodiscard]] FrameSteps StepExactly(uint32_t stepCount);

		// Back to tick 0 with an empty accumulator (a new play session).
		void Reset();

		// The number of steps run since construction or the last Reset; the tick of the next step.
		[[nodiscard]] uint64_t GetTick() const { return m_Tick; }
		[[nodiscard]] double GetFixedDelta() const { return m_FixedDelta; }
		[[nodiscard]] const FrameLoopConfig& GetConfig() const { return m_Config; }
		// The SimStep of `tick` for this scheduler's FixedDelta.
		[[nodiscard]] SimStep GetSimStep(uint64_t tick) const { return SimStep::FromTick(tick, m_FixedDelta); }
	private:
		FrameLoopConfig m_Config;
		double m_FixedDelta = 0.0;
		double m_Accumulator = 0.0; // simulation seconds not yet stepped
		uint64_t m_Tick = 0;
	};

}
