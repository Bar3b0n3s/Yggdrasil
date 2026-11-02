#include "EnginePCH.h"
#include "Engine/Scripting/Watchdog.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace Engine {

	struct ScriptWatchdog::State
	{
		uint32_t Depth = 0;
		double Deadline = 0.0;
		double LastSample = 0.0;
		bool Expired = false;
		// The open host period (BeginHostTime): its protected depth, nesting, and the samples its excluded time counts from;
		// HostCounting turns false once a protected entry inside it has run Luau.
		uint32_t HostDepth = 0;
		uint32_t HostNesting = 0;
		double HostSince = 0.0;
		double HostCpuSince = 0.0;
		bool HostCounting = false;
		// The budget's CPU clock (SetCpuClock), empty when the samples are the only clock. The outermost entry's budget in
		// seconds and CPU sample, and the host periods' CPU time credited since: the CPU time a slice has used is a later
		// sample less CpuStart and CpuCredit.
		std::function<double()> CpuClock{};
		double Budget = 0.0;
		double CpuStart = 0.0;
		double CpuCredit = 0.0;
		double LastCpuSample = 0.0;

		// A CPU sample at or after every earlier one, or nullopt (fail closed) for a non-finite, negative or backwards one.
		std::optional<double> SampleCpu()
		{
			const double sample = CpuClock();
			if (!std::isfinite(sample) || sample < LastCpuSample)
				return std::nullopt;
			LastCpuSample = sample;
			return sample;
		}

		// At or past the wall deadline: with CPU budget left at `cpuSeconds`, moves the wall deadline to `nowSeconds` plus
		// what is left and returns true; false when the budget is spent.
		bool MoveDeadline(double nowSeconds, double cpuSeconds)
		{
			const double used = cpuSeconds - CpuStart - CpuCredit;
			if (!(used < Budget))
				return false;
			const double deadline = nowSeconds + (Budget - used);
			if (!std::isfinite(deadline))
				return false;
			Deadline = deadline;
			return true;
		}

		// An interrupt at or past the wall deadline: samples the CPU clock and moves the deadline as MoveDeadline does; false
		// (expire) without a CPU clock, for an invalid sample or when the budget is spent.
		bool SampleCpuAndMoveDeadline(double nowSeconds)
		{
			if (!CpuClock)
				return false;
			const auto cpu = SampleCpu();
			return cpu.has_value() && MoveDeadline(nowSeconds, *cpu);
		}

		// Moves the deadline later by the host time from HostSince to `nowSeconds`, and credits the host CPU time since
		// HostCpuSince.
		void CreditHostTime(double nowSeconds)
		{
			if (!HostCounting || Expired || !std::isfinite(nowSeconds) || nowSeconds < HostSince)
				return;
			const double extended = Deadline + (nowSeconds - HostSince);
			if (std::isfinite(extended))
				Deadline = extended;
			if (CpuClock)
			{
				if (const auto cpu = SampleCpu())
					CpuCredit += *cpu - HostCpuSince;
			}
		}
	};

	ScriptWatchdog::ScriptWatchdog()
		: m_State(CreateScope<State>())
	{
	}

	ScriptWatchdog::~ScriptWatchdog() = default;

	void ScriptWatchdog::SetCpuClock(std::function<double()> cpuSeconds)
	{
		ENGINE_CORE_ASSERT(m_State->Depth == 0, "a script watchdog's CPU clock changes only while it is idle");
		m_State->CpuClock = std::move(cpuSeconds);
		m_State->LastCpuSample = 0.0;
	}

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
			if (m_State->CpuClock)
			{
				const double cpu = m_State->CpuClock();
				if (!std::isfinite(cpu) || cpu < 0.0)
					return MakeError(ErrorCode::InvalidArgument, "script CPU clock requires finite nonnegative time");
				if (cpu < m_State->LastCpuSample)
					return MakeError(ErrorCode::InvalidState, "script CPU clock moved backwards");
				m_State->LastCpuSample = cpu;
				m_State->CpuStart = cpu;
				m_State->CpuCredit = 0.0;
			}
			m_State->Budget = static_cast<double>(budgetMs) / 1000.0;
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
		if (!m_State->HostCounting)
			return;
		m_State->LastSample = nowSeconds;
		if (!m_State->CpuClock)
			return;
		const auto cpu = m_State->SampleCpu();
		if (!cpu)
		{
			m_State->HostCounting = false;
			return;
		}
		m_State->HostCpuSince = *cpu;
		// Inside the period only a deadline passed before it fires, so one that passed with CPU budget left moves first.
		if (!m_State->Expired && nowSeconds >= m_State->Deadline)
			static_cast<void>(m_State->MoveDeadline(nowSeconds, *cpu));
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
		if (!std::isfinite(nowSeconds) || nowSeconds < m_State->LastSample)
			m_State->Expired = true;
		else
		{
			// The fast path is the one comparison: the CPU budget cannot be spent before the wall deadline passes.
			if (!m_State->Expired && nowSeconds >= m_State->Deadline && !m_State->SampleCpuAndMoveDeadline(nowSeconds))
				m_State->Expired = true;
			m_State->LastSample = nowSeconds;
		}
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
