#include "TestsPCH.h"

#include "Support/DeathTest.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Support/ChildProcess.h"
#include "Support/ExpectLog.h"
#include "Support/TestOptions.h"

namespace Engine {

	static int ComputeAnswer()
	{
		return 41;
	}

	// "<Module>/<CamelCaseName>": two non-empty parts of letters and digits, each starting with an upper-case letter.
	static bool IsWellFormedDeathTestName(std::string_view name)
	{
		const size_t slash = name.find('/');
		if (slash == std::string_view::npos)
			return false;
		const auto isPart = [](std::string_view part)
		{
			return !part.empty() && part.front() >= 'A' && part.front() <= 'Z' && std::ranges::all_of(part, [](char character)
			{
				return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
					|| (character >= '0' && character <= '9');
			});
		};
		return isPart(name.substr(0, slash)) && isPart(name.substr(slash + 1));
	}

	ENGINE_DEATH_TEST("Support/AssertsWithMessage")
	{
		const int answer = ComputeAnswer();
		ENGINE_CORE_ASSERT(answer == 42, "The answer is {}, not 42", answer);
	}

	ENGINE_DEATH_TEST("Support/ReturnsWithoutDying")
	{
	}

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not a contract stub: the target of the ENGINE_CHECK_DEATH failure test below, which
		// runs it in a child with --no-skip. It fails by design, so it belongs to Test::ChildTargetSuite.
		TEST_CASE("DeathTest: the failing check target expects a death that does not happen"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			ENGINE_CHECK_DEATH("Support/ReturnsWithoutDying", "text the child never writes");
		}

		TEST_CASE("DeathTest: registered names are listed sorted and found")
		{
			const std::vector<std::string> names = Test::GetDeathTestNames();
			CHECK(std::ranges::is_sorted(names));
			CHECK(std::ranges::find(names, "Support/AssertsWithMessage") != names.end());
			CHECK(std::ranges::find(names, "Core/AssertFires") != names.end());
			CHECK(Test::FindDeathTest("Support/ReturnsWithoutDying") != nullptr);
			CHECK(Test::FindDeathTest("Support/NoSuchTest") == nullptr);
		}

		TEST_CASE("DeathTest: every registered name is unique and has the form Module/Name")
		{
			const std::vector<std::string> names = Test::GetDeathTestNames();
			CHECK(std::ranges::adjacent_find(names) == names.end());
			for (const std::string& name : names)
			{
				INFO("death test name: ", name);
				CHECK(IsWellFormedDeathTestName(name));
			}
		}

		TEST_CASE("DeathTest: the child exits with code 4 and the assert message on stderr")
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Support/AssertsWithMessage");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("The answer is 41, not 42"));
			CHECK(result->StandardError.contains("Assertion failed: answer == 42"));
			// The child runs no test case, so the recording handler reports the assertion without a test-case name.
			CHECK_FALSE(result->StandardError.contains("[test case:"));

			ENGINE_CHECK_DEATH("Support/AssertsWithMessage", "The answer is 41, not 42");
		}

		TEST_CASE("DeathTest: a body that returns makes the child exit with code 1")
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Support/ReturnsWithoutDying");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == 1);
			CHECK(result->StandardError.contains("returned without dying"));
		}

		TEST_CASE("DeathTest: an unknown name is NotFound without spawning a child")
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Support/NoSuchTest");
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::NotFound);

			Test::ExpectLog unknown(LogLevel::Error, "No death test is registered as 'Support/NoSuchTest'");
			CHECK(Test::RunDeathTestBody("Support/NoSuchTest") == 2);
		}

		TEST_CASE("DeathTest: an unknown name passed to the child exits with code 2")
		{
			const std::vector<std::string> arguments = { "--death-test=Support/NoSuchTest" };
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == 2);
			CHECK(result->StandardError.contains("No death test is registered as 'Support/NoSuchTest'"));
		}

		TEST_CASE("DeathTest: ENGINE_CHECK_DEATH fails at the caller's line when the child exits without the expected death")
		{
			const std::vector<std::string> arguments = {
				"--no-skip",
				"--test-case=DeathTest: the failing check target expects a death that does not happen",
			};
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			INFO("child stdout: ", result->StandardOutput);
			CHECK(result->ExitCode == 1);
			CHECK(result->StandardOutput.contains("exited with code 1 instead of 4"));
			CHECK(result->StandardOutput.contains("does not contain \"text the child never writes\""));
			CHECK(result->StandardOutput.contains("DeathTestTests.cpp"));
		}
	}

}
