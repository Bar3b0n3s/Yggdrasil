#include "EnginePCH.h"
#include "Engine/Scripting/Watchdog.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {

	struct ScriptWatchdog::State
	{
		uint32_t Depth = 0;
		double Deadline = 0.0;
		double LastSample = 0.0;
		bool Expired = false;
		// The open host period (BeginHostTime): its protected depth, nesting, and the sample its excluded time counts from;
		// HostCounting turns false once a protected entry inside it has run Luau.
		uint32_t HostDepth = 0;
		uint32_t HostNesting = 0;
		double HostSince = 0.0;
		bool HostCounting = false;

		// Moves the deadline later by the host time from HostSince to `nowSeconds`.
		void CreditHostTime(double nowSeconds)
		{
			if (!HostCounting || Expired || !std::isfinite(nowSeconds) || nowSeconds < HostSince)
				return;
			const double extended = Deadline + (nowSeconds - HostSince);
			if (std::isfinite(extended))
				Deadline = extended;
		}
	};

	ScriptWatchdog::ScriptWatchdog()
		: m_State(CreateScope<State>())
	{
	}

	ScriptWatchdog::~ScriptWatchdog() = default;

	Status ScriptWatchdog::Enter(uint32_t budgetMs, double nowSeconds)
	{
		if (budgetMs == 0 || !std::isfinite(nowSeconds) || nowSeconds < 0.0)
			return MakeError(ErrorCode::InvalidArgument, "script deadline requires a positive budget and finite nonnegative time");
		if (nowSeconds < m_State->LastSample || m_State->Depth == std::numeric_limits<uint32_t>::max())
			return MakeError(ErrorCode::InvalidState, "script deadline clock moved backwards or nesting overflowed");
		const double deadline = nowSeconds + static_cast<double>(budgetMs) / 1000.0;
		if (!std::isfinite(deadline) || deadline <= nowSeconds)
			return MakeError(ErrorCode::InvalidArgument, "script deadline is outside the clock's representable range");
		if (m_State->Depth == 0)
		{
			m_State->Deadline = deadline;
			m_State->Expired = false;
		}
		else if (m_State->HostNesting != 0 && m_State->HostCounting)
		{
			// Luau inside a host period: the host time so far is excluded, everything from here on counts.
			m_State->CreditHostTime(nowSeconds);
			m_State->HostCounting = false;
		}
		m_State->LastSample = nowSeconds;
		++m_State->Depth;
		return {};
	}

	void ScriptWatchdog::Leave()
	{
		ENGINE_CORE_VERIFY(m_State->Depth > 0, "unmatched script deadline exit");
		if (--m_State->Depth == 0)
		{
			m_State->Deadline = 0.0;
			m_State->Expired = false;
			m_State->HostNesting = 0;
			m_State->HostCounting = false;
		}
		else if (m_State->HostNesting != 0 && m_State->Depth < m_State->HostDepth)
		{
			// The scope that opened the period has unwound without ending it; nothing more is excluded.
			m_State->HostNesting = 0;
			m_State->HostCounting = false;
		}
	}

	void ScriptWatchdog::BeginHostTime(double nowSeconds) noexcept
	{
		if (m_State->Depth == 0)
			return;
		if (m_State->HostNesting++ != 0)
			return;
		m_State->HostDepth = m_State->Depth;
		m_State->HostSince = nowSeconds;
		m_State->HostCounting = std::isfinite(nowSeconds) && nowSeconds >= m_State->LastSample;
		if (m_State->HostCounting)
			m_State->LastSample = nowSeconds;
	}

	void ScriptWatchdog::EndHostTime(double nowSeconds) noexcept
	{
		if (m_State->Depth == 0 || m_State->HostNesting == 0)
			return;
		if (--m_State->HostNesting != 0)
			return;
		m_State->CreditHostTime(nowSeconds);
		m_State->HostCounting = false;
		if (std::isfinite(nowSeconds) && nowSeconds >= m_State->LastSample)
			m_State->LastSample = nowSeconds;
	}

	bool ScriptWatchdog::CheckInterrupt(int gc, double nowSeconds) noexcept
	{
		if (gc >= 0 || m_State->Depth == 0)
			return false;
		// Inside a counting host period the deadline moves with the clock: only a timeout latched before it fires.
		if (m_State->HostNesting != 0 && m_State->HostCounting && !m_State->Expired && std::isfinite(nowSeconds) && nowSeconds >= m_State->LastSample)
		{
			m_State->LastSample = nowSeconds;
			if (m_State->HostSince >= m_State->Deadline)
				m_State->Expired = true;
			return m_State->Expired;
		}
		if (!std::isfinite(nowSeconds) || nowSeconds < m_State->LastSample || nowSeconds >= m_State->Deadline)
			m_State->Expired = true;
		if (std::isfinite(nowSeconds) && nowSeconds >= m_State->LastSample)
			m_State->LastSample = nowSeconds;
		return m_State->Expired;
	}

	uint32_t ScriptWatchdog::GetDepth() const noexcept
	{
		return m_State->Depth;
	}

	double ScriptWatchdog::GetDeadlineSeconds() const noexcept
	{
		return m_State->Deadline;
	}

	Result<uint32_t> ScriptWatchdog::ResolveCallbackBudget(uint32_t configuredMs, bool isTestRun)
	{
		if (configuredMs < 10)
			return MakeError(ErrorCode::InvalidArgument, "script callback budget must be at least 10 ms");
#if defined(ENGINE_DIST)
		return isTestRun ? configuredMs : std::max(configuredMs, 5000u);
#else
		static_cast<void>(isTestRun);
		return configuredMs;
#endif
	}

}
