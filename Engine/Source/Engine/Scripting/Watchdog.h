#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>

namespace Engine {

	// Separate from Engine::Watchdog in Automation/Protocol. Own-thread deadline policy (§11.1); samples are seconds
	// from the private VM adapter's monotonic clock, lua_clock in production. Explicit samples make tests exact.
	// Wall time bounds unsafe execution only; it never supplies simulation time or advances a simulation tick.
	class ScriptWatchdog
	{
	public:
		ScriptWatchdog();
		~ScriptWatchdog();
		ScriptWatchdog(const ScriptWatchdog&) = delete;
		ScriptWatchdog& operator=(const ScriptWatchdog&) = delete;

		// Opens a protected-call scope. The first entry starts nowSeconds + budgetMs / 1000; every nested entry
		// inherits that deadline, even when it requests a larger budget. InvalidArgument for zero budget, non-finite
		// or negative time, or deadline overflow. InvalidState for a backwards sample. Failure does not enter a scope.
		// Each successful entry needs one Leave on every exit (the private ScriptCall wrapper owns that RAII).
		[[nodiscard]] Status Enter(uint32_t budgetMs, double nowSeconds);
		// LIFO, after the protected call/resume unwinds. Asserts on unmatched Leave. The final Leave ends the deadline;
		// the next task/test resume starts fresh. A nested call may never reset the outer timeout latch.
		void Leave();

		// gc >= 0 returns false immediately, without changing state, even for expired/invalid samples. Otherwise
		// true at or beyond the deadline, latched until the outermost Leave. Invalid/backwards samples fail closed.
		// An idle watchdog returns false. This policy does not itself raise: the VM adapter emits ScriptError::Timeout.
		[[nodiscard]] bool CheckInterrupt(int gc, double nowSeconds) noexcept;
		[[nodiscard]] uint32_t GetDepth() const noexcept;
		[[nodiscard]] double GetDeadlineSeconds() const noexcept; // zero when idle

		// configuredMs >= 10 or InvalidArgument. Dist applies max(configuredMs, 5000) except when isTestRun is true;
		// Debug/Release and suite overrides are exact. Load-time imports instead enter their fixed 250 ms budget.
		[[nodiscard]] static Result<uint32_t> ResolveCallbackBudget(uint32_t configuredMs, bool isTestRun);
	private:
		struct State;
		Scope<State> m_State{};
	};

}
