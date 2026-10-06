#include "TestsPCH.h"

#include "Support/TestCaseTracker.h"

#include "Engine/Core/Assert.h"
#include "Support/ChildProcess.h"
#include "Support/DeathTest.h"
#include "Support/TestOptions.h"

#include <thread>

namespace Engine {

	// A death-test child runs no doctest, so it has no running test case and a report goes nowhere. The assert message
	// carries both observations to the parent.
	ENGINE_DEATH_TEST("Support/NoTestCaseInDeathTestChild")
	{
		const bool isRunning = Test::GetRunningTestCase().has_value();
		const bool isReported = Test::ReportFailureToRunningTestCase(__FILE__, __LINE__, "reported outside a test case");
		ENGINE_CORE_ASSERT(false, "Running test case: {}, failure reported: {}", isRunning, isReported);
	}

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not a contract stub: the target of the cross-thread report test below, which runs
		// it in a child with --no-skip. It fails by design, so it belongs to Test::ChildTargetSuite.
		TEST_CASE("TestCaseTracker: the target reports a failure from another thread" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			bool isReported = false;
			std::thread worker([&isReported]()
			{
				isReported = Test::ReportFailureToRunningTestCase(__FILE__, __LINE__, "Failure reported by a worker thread");
			});
			worker.join();
			CHECK(isReported);
		}

		TEST_CASE("TestCaseTracker: names the running test case")
		{
			const std::optional<Test::RunningTestCase> running = Test::GetRunningTestCase();
			REQUIRE(running.has_value());
			CHECK(running->Name == "TestCaseTracker: names the running test case");
			CHECK(running->File.ends_with("TestCaseTrackerTests.cpp"));
			CHECK(running->Line > 0);
			CHECK(running->TimeoutSeconds == 0.0);
			CHECK(running->Serial > 0);
		}

		TEST_CASE("TestCaseTracker: reports the timeout decorator of the running case" * doctest::timeout(90.0))
		{
			const std::optional<Test::RunningTestCase> running = Test::GetRunningTestCase();
			REQUIRE(running.has_value());
			CHECK(running->TimeoutSeconds == 90.0);
		}

		TEST_CASE("TestCaseTracker: other threads see the same running test case")
		{
			const std::optional<Test::RunningTestCase> here = Test::GetRunningTestCase();
			std::optional<Test::RunningTestCase> there;
			std::thread worker([&there]()
			{
				there = Test::GetRunningTestCase();
			});
			worker.join();

			REQUIRE(here.has_value());
			REQUIRE(there.has_value());
			CHECK(there->Name == here->Name);
			CHECK(there->Serial == here->Serial);
		}

		TEST_CASE("TestCaseTracker: a failure reported from another thread fails the running case")
		{
			const std::vector<std::string> arguments = {
				"--no-skip",
				"--test-case=TestCaseTracker: the target reports a failure from another thread",
			};
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			INFO("child stdout: ", result->StandardOutput);
			CHECK(result->ExitCode == 1);
			CHECK(result->StandardOutput.contains("Failure reported by a worker thread"));
		}

		TEST_CASE("TestCaseTracker: a death-test child has no running test case")
		{
			ENGINE_CHECK_DEATH("Support/NoTestCaseInDeathTestChild", "Running test case: false, failure reported: false");
		}
	}

}
