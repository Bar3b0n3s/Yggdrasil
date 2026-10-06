#include "EnginePCH.h"
#include "Engine/Core/Clock.h"

#include "Engine/Core/Assert.h"

#include <chrono>
#include <cmath>

namespace Engine {

	double SystemClock::Delta()
	{
		const int64_t now =
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const int64_t previous = m_HasPrevious ? m_PreviousNanoseconds : now;
		m_PreviousNanoseconds = now;
		m_HasPrevious = true;
		// steady_clock never goes backwards; the clamp keeps the documented guarantee independent of that.
		return static_cast<double>(std::max<int64_t>(now - previous, 0)) * 1e-9;
	}

	ManualClock::ManualClock(double fixedDelta)
		: m_FixedDelta(fixedDelta)
	{
		ENGINE_CORE_ASSERT(std::isfinite(fixedDelta) && fixedDelta > 0.0, "ManualClock needs a finite fixed delta > 0, got {}",
			fixedDelta);
	}

	double ManualClock::Delta()
	{
		return m_FixedDelta;
	}

	ScriptedClock::ScriptedClock(std::vector<double> deltas)
		: m_Deltas(std::move(deltas))
	{
	}

	Result<ScriptedClock> ScriptedClock::Create(std::span<const double> deltas)
	{
		if (deltas.empty())
			return MakeError(ErrorCode::InvalidArgument, "ScriptedClock needs at least one frame delta");
		for (size_t index = 0; index < deltas.size(); ++index)
		{
			const double delta = deltas[index];
			if (!std::isfinite(delta) || delta < 0.0)
				return MakeError(ErrorCode::InvalidArgument, "ScriptedClock frame delta {} is {}; deltas must be finite and >= 0",
					index, delta);
		}
		return ScriptedClock(std::vector<double>(deltas.begin(), deltas.end()));
	}

	double ScriptedClock::Delta()
	{
		const double delta = m_Deltas[static_cast<size_t>(m_CallCount % m_Deltas.size())];
		++m_CallCount;
		return delta;
	}

}
