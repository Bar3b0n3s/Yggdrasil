#include "EnginePCH.h"
#include "Engine/Core/FixedStepScheduler.h"

#include "Engine/Core/Assert.h"

#include <cmath>

namespace Engine {

	FixedStepScheduler::FixedStepScheduler(const FrameLoopConfig& config)
		: m_Config(config)
	{
		ENGINE_CORE_ASSERT(config.FixedHz > 0, "FrameLoopConfig::FixedHz must be > 0");
		ENGINE_CORE_ASSERT(config.FixedHz <= FrameLoopConfig::MaxFixedHz, "FrameLoopConfig::FixedHz must be <= {}, got {}",
			FrameLoopConfig::MaxFixedHz, config.FixedHz);
		ENGINE_CORE_ASSERT(config.MaxStepsPerFrame > 0, "FrameLoopConfig::MaxStepsPerFrame must be > 0");
		ENGINE_CORE_ASSERT(std::isfinite(config.MaxFrameDelta) && config.MaxFrameDelta > 0.0,
			"FrameLoopConfig::MaxFrameDelta must be finite and > 0, got {}", config.MaxFrameDelta);

		// Computed after the checks: a FixedHz of 0 must not be divided by.
		m_FixedDelta = config.GetFixedDelta();
	}

	FrameSteps FixedStepScheduler::Advance(double realDeltaSeconds, double timeScale)
	{
		ENGINE_CORE_ASSERT(std::isfinite(realDeltaSeconds) && realDeltaSeconds >= 0.0,
			"FixedStepScheduler::Advance needs a finite frame delta >= 0, got {}", realDeltaSeconds);
		ENGINE_CORE_ASSERT(std::isfinite(timeScale) && timeScale >= 0.0,
			"FixedStepScheduler::Advance needs a finite time scale >= 0, got {}", timeScale);

		// The numbered steps are those of the class comment in FixedStepScheduler.h.
		FrameSteps steps;
		steps.FirstTick = m_Tick;

		// 1. Clamp the frame delta; the clamped-off part is dropped.
		const double clamped = std::min(realDeltaSeconds, m_Config.MaxFrameDelta);
		steps.DroppedSeconds = (realDeltaSeconds - clamped) * timeScale;

		// 2. Accumulate scaled time.
		m_Accumulator += clamped * timeScale;

		// 3. Run whole steps, at most MaxStepsPerFrame.
		const double threshold = m_FixedDelta - StepTolerance;
		while (m_Accumulator >= threshold && steps.StepCount < m_Config.MaxStepsPerFrame)
		{
			++steps.StepCount;
			m_Accumulator = std::max(0.0, m_Accumulator - m_FixedDelta);
		}

		// 4. At the cap, drop the whole steps still due and keep the remainder in [0, FixedDelta).
		if (steps.StepCount == m_Config.MaxStepsPerFrame && m_Accumulator >= threshold)
		{
			const double wholeSteps = std::floor((m_Accumulator + StepTolerance) / m_FixedDelta);
			steps.DroppedSeconds += m_FixedDelta * wholeSteps;
			m_Accumulator = std::clamp(m_Accumulator - m_FixedDelta * wholeSteps, 0.0, std::nextafter(m_FixedDelta, 0.0));
		}

		// 5. Interpolation factor.
		steps.Alpha = std::clamp(m_Accumulator / m_FixedDelta, 0.0, 1.0);
		m_Tick += steps.StepCount;
		return steps;
	}

	FrameSteps FixedStepScheduler::StepExactly(uint32_t stepCount)
	{
		FrameSteps steps;
		steps.StepCount = stepCount;
		steps.Alpha = 1.0;
		steps.FirstTick = m_Tick;
		m_Tick += stepCount;
		return steps;
	}

	void FixedStepScheduler::Reset()
	{
		m_Accumulator = 0.0;
		m_Tick = 0;
	}

}
