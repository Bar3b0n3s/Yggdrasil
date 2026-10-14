#include "TestsPCH.h"

#include "Support/TestTimeout.h"

#include "Support/ChildProcess.h"
#include "Support/TestOptions.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

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

		TEST_CASE("TestTimeout: the target with its own limit blocks past it" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			Test::SetTestCaseTimeLimit(0.5);
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

		TEST_CASE("TestTimeout: SetTestCaseTimeLimit replaces the default limit")
		{
			// The default limit (120 s) would outlast the 60 s wait below; only the case's own 0.5 s ends the child in time.
			const std::vector<std::string> arguments = {
				"--no-skip",
				"--test-case=TestTimeout: the target with its own limit blocks past it",
			};
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == Test::TestTimeoutExitCode);
			CHECK(result->StandardError.contains("TestTimeout: the target with its own limit blocks past it"));
			CHECK(result->StandardError.contains("exceeded its time limit of 0.5 s"));
		}

		TEST_CASE("TestTimeout: --test-timeout-scale multiplies a case's own limit and the default limit alike")
		{
			// The case's own 0.5 s limit becomes 1.5 s.
			const std::vector<std::string> ownLimitArguments = {
				"--no-skip",
				"--test-case=TestTimeout: the target with its own limit blocks past it",
				"--test-timeout-scale=3",
			};
			const Result<Test::ChildProcessResult> ownLimit = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath, ownLimitArguments,
				std::chrono::seconds(60));
			REQUIRE(ownLimit.has_value());
			CHECK(ownLimit->ExitCode == Test::TestTimeoutExitCode);
			CHECK(ownLimit->StandardError.contains("exceeded its time limit of 1.5 s"));

			// The default limit of 0.5 s becomes 1 s.
			const std::vector<std::string> defaultArguments = {
				"--no-skip",
				"--test-case=TestTimeout: the watchdog target blocks until it is killed",
				"--test-timeout=0.5",
				"--test-timeout-scale=2",
			};
			const Result<Test::ChildProcessResult> defaultLimit = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath, defaultArguments,
				std::chrono::seconds(60));
			REQUIRE(defaultLimit.has_value());
			CHECK(defaultLimit->ExitCode == Test::TestTimeoutExitCode);
			CHECK(defaultLimit->StandardError.contains("exceeded its time limit of 1 s"));
		}

		TEST_CASE("TestTimeout: a case with a doctest::timeout decorator is named with SetTestCaseTimeLimit as the remedy")
		{
			// doctest checks a decorated limit itself once the case ends, unscaled by --test-timeout-scale, so a listing names
			// such a case (no test case carries the decorator; a hand-built case record stands for one).
			doctest::TestCaseData decorated{};
			decorated.m_name = "Example: a decorated case";
			decorated.m_timeout = 600.0;
			const std::optional<std::string> failure = Test::GetDoctestTimeoutFailure(decorated);
			REQUIRE(failure.has_value());
			CHECK(failure->contains("Example: a decorated case"));
			CHECK(failure->contains("doctest::timeout(600)"));
			CHECK(failure->contains("Test::SetTestCaseTimeLimit(600)"));

			doctest::TestCaseData plain{};
			plain.m_name = "Example: a plain case";
			CHECK_FALSE(Test::GetDoctestTimeoutFailure(plain).has_value());
		}

		TEST_CASE("TestTimeout: no test case carries a doctest::timeout decorator")
		{
			// --test-timeout-scale covers every per-case limit only while the watchdog's are the only ones: a listing of every
			// test case, the skipped child targets included, logs an Error naming each decorated case (GetDoctestTimeoutFailure).
			const std::vector<std::string> arguments = { "--list-test-cases", "--no-skip" };
			const Result<Test::ChildProcessResult> listing = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath, arguments,
				std::chrono::seconds(120));
			REQUIRE(listing.has_value());
			CHECK(listing->ExitCode == 0);
			CHECK(listing->StandardOutput.contains("TestTimeout: no test case carries a doctest::timeout decorator"));
			CHECK_MESSAGE(!listing->StandardError.contains("carries doctest::timeout("), listing->StandardError);
		}
	}

}
