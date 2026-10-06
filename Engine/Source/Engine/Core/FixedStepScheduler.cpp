#include "EnginePCH.h"
#include "Engine/Core/FixedStepScheduler.h"

// M1 contract stub (Roadmap rule 3): stream B implements the stepping rules documented in FixedStepScheduler.h. Until
// then no frame runs a step.

namespace Engine {

	FixedStepScheduler::FixedStepScheduler(const FrameLoopConfig& config)
		: m_Config(config), m_FixedDelta(config.GetFixedDelta())
	{
	}

	FrameSteps FixedStepScheduler::Advance(double /*realDeltaSeconds*/, double /*timeScale*/)
	{
		FrameSteps steps;
		steps.FirstTick = m_Tick;
		return steps;
	}

	FrameSteps FixedStepScheduler::StepExactly(uint32_t /*stepCount*/)
	{
		FrameSteps steps;
		steps.FirstTick = m_Tick;
		return steps;
	}

	void FixedStepScheduler::Reset()
	{
	}

}
