#include "TestsPCH.h"

#include "Support/RecordingAssertHandler.h"

#include "Support/ChildProcess.h"
#include "Support/TestOptions.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"

namespace Engine {

	static int ComputeRecordedAnswer()
	{
		return 41;
	}

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not a contract stub: the target of the exit-code test below, which runs it in a
		// child with --no-skip. It terminates the process, so it belongs to Test::ChildTargetSuite.
		TEST_CASE("RecordingAssertHandler: the target case fails an assertion" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			const int answer = ComputeRecordedAnswer();
			ENGINE_CORE_ASSERT(answer == 42, "The recorded answer is {}", answer);
		}

		TEST_CASE("RecordingAssertHandler: a failed assert in a test case exits with code 4 and names the case"
			* doctest::skip(true))
		{
			const std::vector<std::string> arguments = {
				"--no-skip",
				"--test-case=RecordingAssertHandler: the target case fails an assertion",
			};
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("Assertion failed: answer == 42: The recorded answer is 41"));
			CHECK(result->StandardError.contains("[test case: RecordingAssertHandler: the target case fails an assertion]"));
			// doctest's report goes to the redirected, fully buffered stdout; FatalError flushes it before exiting.
			CHECK(result->StandardOutput.contains("The recorded answer is 41"));
		}

		TEST_CASE("RecordingAssertHandler: the Tests main installs it" * doctest::skip(true))
		{
			CHECK(GetAssertHandler() == &Test::RecordingAssertHandler);
		}
	}

}
