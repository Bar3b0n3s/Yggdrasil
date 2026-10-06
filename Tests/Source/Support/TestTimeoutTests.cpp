#include "TestsPCH.h"

#include "Support/TestTimeout.h"

#include "Support/ChildProcess.h"
#include "Support/TestOptions.h"

#include <condition_variable>
#include <mutex>

namespace Engine {

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not a contract stub: the target of the watchdog test below, which runs it in a
		// child with --no-skip. It never finishes on its own, so it belongs to Test::ChildTargetSuite.
		TEST_CASE("TestTimeout: the watchdog target blocks until it is killed" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			std::mutex mutex;
			std::condition_variable never;
			std::unique_lock lock(mutex);
			never.wait(lock, []()
			{
				return false;
			});
		}

		TEST_CASE("TestTimeout: a hanging test case ends the run with exit code 5" * doctest::skip(true))
		{
			const std::vector<std::string> arguments = {
				"--no-skip",
				"--test-case=TestTimeout: the watchdog target blocks until it is killed",
				"--test-timeout=1",
			};
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == Test::TestTimeoutExitCode);
			CHECK(result->StandardError.contains("TestTimeout: the watchdog target blocks until it is killed"));
		}
	}

}
