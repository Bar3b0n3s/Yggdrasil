#include "TestsPCH.h"

#include "Support/ChildProcess.h"

#include "Engine/Core/Log.h"
#include "Support/DeathTest.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

#include <system_error>

namespace Engine {

	// The name of the quoting target below: quotes, spaces, a backslash before a quote and a trailing backslash, the
	// characters a Windows command line must escape. No comma, '*' or '?', which doctest's --test-case filter interprets.
	static constexpr const char* QuotingTargetName = "ChildProcess: the quoting target \"a b\" c\\d\\\"e\\";

	// Blocks until killed: the child of the timeout test.
	ENGINE_DEATH_TEST("Support/Hangs")
	{
		Test::BlockUntilKilled();
	}

	// Writes far more to stderr than a pipe holds (about 160 KB) before it exits, while nothing reaches stdout.
	ENGINE_DEATH_TEST("Support/FloodsStandardError")
	{
		for (int line = 0; line < 2000; ++line)
			ENGINE_CORE_WARN("Flooding standard error with line {} of 2000 to fill the pipe buffer", line);
	}

	// Reports whether the parent marked this process with --child-process (TestOptions::ChildProcess), then returns.
	ENGINE_DEATH_TEST("Support/ReportsChildProcessFlag")
	{
		ENGINE_CORE_WARN("Child process flag: {}", Test::GetTestOptions().ChildProcess);
	}

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not a contract stub: listed (never run) by the quoting test below, whose
		// --test-case argument names it. It does nothing, but it exists only for that child, so it belongs to
		// Test::ChildTargetSuite.
		TEST_CASE(QuotingTargetName * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
		}

		TEST_CASE("ChildProcess: captures the exit code and both output streams")
		{
			const std::filesystem::path& executable = Test::GetTestOptions().ExecutablePath;

			const std::vector<std::string> listArguments = { "--list-test-cases", "--test-case=ChildProcess:*" };
			const Result<Test::ChildProcessResult> listed = Test::RunChildProcess(executable, listArguments,
				std::chrono::seconds(60));
			REQUIRE(listed.has_value());
			CHECK(listed->ExitCode == 0);
			CHECK(listed->StandardOutput.contains("ChildProcess: captures the exit code and both output streams"));

			const std::vector<std::string> deathArguments = { "--death-test=Support/ReturnsWithoutDying" };
			const Result<Test::ChildProcessResult> returned = Test::RunChildProcess(executable, deathArguments,
				std::chrono::seconds(60));
			REQUIRE(returned.has_value());
			CHECK(returned->ExitCode == 1);
			CHECK(returned->StandardError.contains("returned without dying"));
		}

		TEST_CASE("ChildProcess: a child that fills a pipe while the parent waits on the other one finishes")
		{
			const std::vector<std::string> floodArguments = { "--death-test=Support/FloodsStandardError" };
			const Result<Test::ChildProcessResult> flooded = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				floodArguments, std::chrono::seconds(60));
			REQUIRE(flooded.has_value());
			CHECK(flooded->ExitCode == 1);
			CHECK(flooded->StandardError.contains("line 0 of 2000"));
			CHECK(flooded->StandardError.contains("line 1999 of 2000"));
			CHECK(flooded->StandardError.size() > 100000);

			// The other direction: every test case name on stdout, also more than a pipe holds.
			const std::vector<std::string> listArguments = { "--no-skip", "--list-test-cases" };
			const Result<Test::ChildProcessResult> listed = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				listArguments, std::chrono::seconds(60));
			REQUIRE(listed.has_value());
			CHECK(listed->ExitCode == 0);
			CHECK(listed->StandardOutput.contains("Smoke: test runner starts"));
			CHECK(listed->StandardOutput.size() > 8192);
		}

		TEST_CASE("ChildProcess: arguments are passed verbatim without a shell")
		{
			const std::vector<std::string> arguments = { "--test-case=No case has this name; echo injected", "--list-test-cases" };
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == 0);
			CHECK_FALSE(result->StandardOutput.contains("injected\n"));
		}

		TEST_CASE("ChildProcess: quotes, spaces, backslashes and empty arguments arrive unchanged")
		{
			// The child lists the target only if its --test-case filter arrived exactly as written here.
			const std::vector<std::string> arguments = {
				"",
				"--no-skip",
				"--list-test-cases",
				std::format("--test-case={}", QuotingTargetName),
				"",
			};
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			INFO("child stdout: ", result->StandardOutput);
			CHECK(result->ExitCode == 0);
			CHECK(result->StandardOutput.contains(QuotingTargetName));
		}

		TEST_CASE("ChildProcess: the child is marked with --child-process")
		{
			const std::vector<std::string> arguments = { "--death-test=Support/ReportsChildProcessFlag" };
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->StandardError.contains("Child process flag: true"));
		}

		TEST_CASE("ChildProcess: a missing executable is NotFound")
		{
			Test::TempDirectory directory("ChildProcess");
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(directory / "Missing.exe", {},
				std::chrono::seconds(5));
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("ChildProcess: a directory is not an executable")
		{
			Test::TempDirectory directory("ChildProcess");
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(directory.GetPath(), {}, std::chrono::seconds(5));
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("ChildProcess: a child that outlives its timeout is killed")
		{
			const std::vector<std::string> arguments = { "--death-test=Support/Hangs" };
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::milliseconds(500));
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Timeout);
		}

		TEST_CASE("ChildProcess: GetCurrentExecutablePath names the running Tests binary")
		{
			const Result<std::filesystem::path> path = Test::GetCurrentExecutablePath();
			REQUIRE(path.has_value());
			CHECK(path->is_absolute());
			CHECK(path->stem() == "Tests");
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(*path, error));
			CHECK_FALSE(error);
		}
	}

}
