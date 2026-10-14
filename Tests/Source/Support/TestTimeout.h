#pragma once

#include <doctest/doctest.h>

#include <optional>
#include <string>

// Per-test timeout (Architecture §15.2): a watchdog that ends the run when a test case hangs, so CI reports a timeout
// instead of waiting for the job limit.

namespace Engine {

	namespace Test {

		// The process exit code of a timed-out run (§4.1 table: Timeout).
		inline constexpr int TestTimeoutExitCode = 5;

		// Starts the watchdog thread (the Tests main, once, after SetTestOptions). Every test case gets a limit: the one it
		// sets with SetTestCaseTimeLimit, otherwise `defaultSeconds` (--test-timeout), either one multiplied by
		// GetTestOptions().TimeoutScale (--test-timeout-scale, for the sanitizer and long fuzz runs of the hardening stage,
		// Docs/Decisions/0024-m15-contract.md decision 13) and counted from the start of the case. A case still running at
		// its limit makes the watchdog log a Critical entry naming the case and the scaled limit, flush the log and exit the
		// process with TestTimeoutExitCode. The watchdog is the only per-case limit: it ignores doctest::timeout, which
		// doctest checks itself once a case ends and which --test-timeout-scale cannot scale, so no test case carries that
		// decorator (a --list-test-cases run logs an Error naming each one, GetDoctestTimeoutFailure, which the case
		// "TestTimeout: no test case carries a doctest::timeout decorator" checks). The limit is measured with the steady
		// clock; no test logic depends on it. `defaultSeconds` must be > 0 and the scale >= 1 (asserted; ParseTestOptions
		// refuses others).
		void StartTestTimeoutWatchdog(double defaultSeconds);

		// Stops and joins the watchdog. Idempotent.
		void StopTestTimeoutWatchdog();

		// Replaces the running test case's limit with `seconds` (> 0 and finite, asserted), multiplied by
		// --test-timeout-scale like the default limit and counted from the start of the case, so the time the case ran
		// before the call counts against it. For the cases that need another limit than the default, which call it first
		// in their body (again in every subcase run, which changes nothing). Outside a test case the watchdog watches (a
		// child mode that runs no doctest case, or before StartTestTimeoutWatchdog) it does nothing.
		void SetTestCaseTimeLimit(double seconds);

		// The Error a --list-test-cases run logs for `testCase` when it carries a doctest::timeout decorator, naming the case
		// and SetTestCaseTimeLimit; nullopt for a case without one. Pure.
		[[nodiscard]] std::optional<std::string> GetDoctestTimeoutFailure(const doctest::TestCaseData& testCase);

	}

}
