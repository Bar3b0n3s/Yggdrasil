#include "TestsPCH.h"

#include "Support/TestTimeout.h"

#include "Support/ChildProcess.h"
#include "Support/TestOptions.h"

#include <condition_variable>
#include <mutex>

namespace Engine {

	// Blocks the calling test case until the watchdog ends the process. It never notifies, so the wait never ends.
	static void BlockForever()
	{
		std::mutex mutex;
		std::condition_variable never;
		std::unique_lock lock(mutex);
		never.wait(lock, []()
		{
			return false;
		});
	}

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not contract stubs: the targets of the watchdog tests below, which run them in a
		// child with --no-skip. They never finish on their own, so they belong to Test::ChildTargetSuite.
		TEST_CASE("TestTimeout: the watchdog target blocks until it is killed" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			BlockForever();
		}

		TEST_CASE("TestTimeout: the decorated target blocks past its own limit" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::timeout(0.5) * doctest::skip(true))
		{
			BlockForever();
		}

		TEST_CASE("TestTimeout: a hanging test case ends the run with exit code 5")
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

		TEST_CASE("TestTimeout: the doctest::timeout decorator replaces the default limit")
		{
			// The default limit (120 s) would outlast the 60 s wait below; only the case's own 0.5 s ends the child in time.
			const std::vector<std::string> arguments = {
				"--no-skip",
				"--test-case=TestTimeout: the decorated target blocks past its own limit",
			};
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == Test::TestTimeoutExitCode);
			CHECK(result->StandardError.contains("TestTimeout: the decorated target blocks past its own limit"));
			CHECK(result->StandardError.contains("0.5 s"));
		}
	}

}
