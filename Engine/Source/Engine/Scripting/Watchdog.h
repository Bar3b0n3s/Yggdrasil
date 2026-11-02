#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <functional>

namespace Engine {

	// Separate from Engine::Watchdog in Automation/Protocol. Own-thread deadline policy (§11.1, §11.2). A callback's budget
	// counts the CPU time of the thread that runs its VM, so no load from other threads or processes can time a callback
	// out, while a runaway loop, which spends that thread's CPU time, still does (Docs/Decisions/0039-script-budget-cpu-time.md).
	// The explicit samples are seconds of the VM's wall clock (lua_clock for the runtime VMs), the cheap fast path of every
	// interrupt: the wall clock never runs slower than the thread's CPU time, so the CPU budget cannot be spent before the
	// wall deadline passes. Only then does the watchdog sample its CPU clock (SetCpuClock; ThreadClock::GetCpuSeconds costs a
	// system call): with CPU budget left, the wall deadline moves to now plus what is left and the callback continues;
	// otherwise it expires. Without a CPU clock the samples are the budget's only clock: the load-time VMs' samples are the
	// import thread's CPU time already (ThreadClock::GetCpuSeconds, passed as LoadTimeVmSpecification::ClockSeconds by
	// ScriptImporter). Explicit samples and an injected CPU clock make tests exact. The clocks bound unsafe execution only;
	// they never supply simulation time or advance a simulation tick.
	class ScriptWatchdog
	{
	public:
		ScriptWatchdog();
		~ScriptWatchdog();
		ScriptWatchdog(const ScriptWatchdog&) = delete;
		ScriptWatchdog& operator=(const ScriptWatchdog&) = delete;

		// Installs the clock the budget counts, seconds of the owner thread's CPU time: finite, nonnegative and nondecreasing,
		// with captures that outlive the watchdog. Sampled only at an outermost Enter, at a host period's boundaries and when
		// an interrupt finds the wall deadline passed; never on a gc >= 0 interrupt. Idle only (asserts otherwise); an empty
		// clock makes the samples the only clock again.
		void SetCpuClock(std::function<double()> cpuSeconds);

		// Opens a protected-call scope. The first entry starts nowSeconds + budgetMs / 1000 and, with a CPU clock, samples it
		// as the start of the budget's CPU time; every nested entry inherits that deadline and budget, even when it requests a
		// larger budget. InvalidArgument for zero budget, non-finite or negative time (either clock), or deadline overflow.
		// InvalidState for a backwards sample (either clock). Failure does not enter a scope. Each successful entry needs one
		// Leave on every exit (the private ScriptCall wrapper owns that RAII).
		[[nodiscard]] Status Enter(uint32_t budgetMs, double nowSeconds);
		// LIFO, after the protected call/resume unwinds. Asserts on unmatched Leave. The final Leave ends the deadline;
		// the next task/test resume starts fresh. A nested call may never reset the outer timeout latch.
		void Leave();

		// Host operations inside a protected call (ADR 0022 "Integration"): a binding that blocks in host code which runs no
		// Luau, such as Test.Screenshot's capture or Test.ReloadScript's reimport and type check, brackets it with these, and
		// the deadline moves later by the seconds between the two samples, and the CPU time between the CPU clock's samples
		// at the same boundaries is credited, so host time never counts toward the callback budget while the script's own Luau
		// time does. A protected entry inside the period (Luau the host operation ran after all) ends the excluded time there:
		// only the time before it is credited, so Luau time always counts. Periods nest (only the outermost one credits).
		// Idle (depth zero), a latched timeout, or a non-finite or backwards sample (either clock) credits nothing; a latched
		// timeout stays latched. A period that begins past the wall deadline with CPU budget left first moves the deadline
		// as an interrupt would. Each BeginHostTime needs one EndHostTime, in the same protected scope.
		void BeginHostTime(double nowSeconds) noexcept;
		void EndHostTime(double nowSeconds) noexcept;

		// gc >= 0 returns false immediately, without changing state or sampling the CPU clock, even for expired/invalid
		// samples. Otherwise true once the budget is spent (at or beyond the wall deadline without a CPU clock; with one, when
		// the CPU time used since the outermost Enter, less the credited host time, reaches the budget), latched until the
		// outermost Leave. Invalid/backwards samples of either clock fail closed. An idle watchdog returns false. This policy
		// does not itself raise: the VM adapter emits ScriptError::Timeout.
		[[nodiscard]] bool CheckInterrupt(int gc, double nowSeconds) noexcept;
		[[nodiscard]] uint32_t GetDepth() const noexcept;
		[[nodiscard]] double GetDeadlineSeconds() const noexcept; // the wall deadline; zero when idle

		// configuredMs >= 10 or InvalidArgument. Dist applies max(configuredMs, 5000) except when isTestRun is true;
		// Debug/Release and suite overrides are exact. Load-time imports instead enter their fixed 250 ms budget.
		[[nodiscard]] static Result<uint32_t> ResolveCallbackBudget(uint32_t configuredMs, bool isTestRun);
	private:
		struct State;
		Scope<State> m_State{};
	};

}
