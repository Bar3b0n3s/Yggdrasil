#pragma once

// Per-test timeout (Architecture §15.2): a watchdog that ends the run when a test case hangs, so CI reports a timeout
// instead of waiting for the job limit.

namespace Engine {

	namespace Test {

		// The process exit code of a timed-out run (§4.1 table: Timeout).
		inline constexpr int TestTimeoutExitCode = 5;

		// Starts the watchdog thread (the Tests main, once). For every test case it arms a deadline: the case's
		// doctest::timeout(seconds) decorator when it has one, otherwise `defaultSeconds`. A case still running at its deadline
		// makes the watchdog log a Critical entry naming the case and the limit, flush the log and exit the process with
		// TestTimeoutExitCode. The deadline is measured with the steady clock; no test logic depends on it. `defaultSeconds`
		// must be > 0 (asserted).
		void StartTestTimeoutWatchdog(double defaultSeconds);

		// Stops and joins the watchdog. Idempotent.
		void StopTestTimeoutWatchdog();

	}

}
