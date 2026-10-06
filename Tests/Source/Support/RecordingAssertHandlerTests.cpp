#include "TestsPCH.h"

#include "Support/RecordingAssertHandler.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Support/ChildProcess.h"
#include "Support/TestOptions.h"

#include <thread>

namespace Engine {

	static int ComputeRecordedAnswer()
	{
		return 41;
	}

	// Runs the ChildTargets case `name` alone in a child process.
	static Result<Test::ChildProcessResult> RunTargetCase(std::string_view name)
	{
		const std::vector<std::string> arguments = { "--no-skip", std::format("--test-case={}", name) };
		return Test::RunChildProcess(Test::GetTestOptions().ExecutablePath, arguments, std::chrono::seconds(60));
	}

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not contract stubs: the targets of the exit-code tests below, which run them in a
		// child with --no-skip. They terminate the process, so they belong to Test::ChildTargetSuite.
		TEST_CASE("RecordingAssertHandler: the target case fails an assertion" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			const int answer = ComputeRecordedAnswer();
			ENGINE_CORE_ASSERT(answer == 42, "The recorded answer is {}", answer);
		}

		TEST_CASE("RecordingAssertHandler: the worker target fails an assertion on another thread"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			std::thread worker([]()
			{
				const int answer = ComputeRecordedAnswer();
				ENGINE_ASSERT(answer == 42, "The worker's answer is {}", answer);
			});
			worker.join();
		}

		TEST_CASE("RecordingAssertHandler: a failed assert in a test case exits with code 4 and names the case")
		{
			const Result<Test::ChildProcessResult> result = RunTargetCase("RecordingAssertHandler: the target case fails an assertion");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("Assertion failed: answer == 42: The recorded answer is 41"));
			CHECK(result->StandardError.contains("[test case: RecordingAssertHandler: the target case fails an assertion]"));
			// doctest's report goes to the redirected, fully buffered stdout; FatalError flushes it before exiting.
			CHECK(result->StandardOutput.contains("The recorded answer is 41"));
		}

		TEST_CASE("RecordingAssertHandler: an assert on another thread is recorded for the running case")
		{
			constexpr std::string_view TargetName = "RecordingAssertHandler: the worker target fails an assertion on another thread";
			const Result<Test::ChildProcessResult> result = RunTargetCase(TargetName);
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("The worker's answer is 41"));
			CHECK(result->StandardError.contains(std::format("[test case: {}]", TargetName)));
			CHECK(result->StandardOutput.contains("The worker's answer is 41"));
		}

		TEST_CASE("RecordingAssertHandler: the Tests main installs it")
		{
			CHECK(GetAssertHandler() == &Test::RecordingAssertHandler);
		}
	}

}
