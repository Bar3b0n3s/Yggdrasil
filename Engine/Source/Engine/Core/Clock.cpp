#include "EnginePCH.h"
#include "Engine/Core/Clock.h"

// M1 contract stub (Roadmap rule 3): stream B implements the clocks. Until then every clock reports 0 and
// ScriptedClock::Create fails with Unsupported.

namespace Engine {

	double SystemClock::Delta()
	{
		return 0.0;
	}

	ManualClock::ManualClock(double fixedDelta)
		: m_FixedDelta(fixedDelta)
	{
	}

	double ManualClock::Delta()
	{
		return 0.0;
	}

	ScriptedClock::ScriptedClock(std::vector<double> deltas)
		: m_Deltas(std::move(deltas))
	{
	}

	Result<ScriptedClock> ScriptedClock::Create(std::span<const double> /*deltas*/)
	{
		return MakeError(ErrorCode::Unsupported, "ScriptedClock::Create is an M1 contract stub");
	}

	double ScriptedClock::Delta()
	{
		return 0.0;
	}

}
