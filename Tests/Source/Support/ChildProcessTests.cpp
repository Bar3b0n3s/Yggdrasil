#include "TestsPCH.h"

#include "Support/ChildProcess.h"

#include "Engine/Core/FileSystem.h"
#include "Support/DeathTest.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

#include <condition_variable>
#include <mutex>

namespace Engine {

	// Blocks forever: the child of the timeout test. It never notifies, so the wait never ends.
	ENGINE_DEATH_TEST("Support/Hangs")
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
		TEST_CASE("ChildProcess: captures the exit code and both output streams" * doctest::skip(true))
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

		TEST_CASE("ChildProcess: arguments are passed verbatim without a shell" * doctest::skip(true))
		{
			const std::vector<std::string> arguments = { "--test-case=No case has this name; echo injected", "--list-test-cases" };
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == 0);
			CHECK_FALSE(result->StandardOutput.contains("injected\n"));
		}

		TEST_CASE("ChildProcess: a missing executable is NotFound" * doctest::skip(true))
		{
			Test::TempDirectory directory("ChildProcess");
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(directory / "Missing.exe", {},
				std::chrono::seconds(5));
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("ChildProcess: a child that outlives its timeout is killed" * doctest::skip(true))
		{
			const std::vector<std::string> arguments = { "--death-test=Support/Hangs" };
			const Result<Test::ChildProcessResult> result = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath,
				arguments, std::chrono::milliseconds(500));
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Timeout);
		}

		TEST_CASE("ChildProcess: GetCurrentExecutablePath names the running Tests binary" * doctest::skip(true))
		{
			const Result<std::filesystem::path> path = Test::GetCurrentExecutablePath();
			REQUIRE(path.has_value());
			CHECK(path->is_absolute());
			CHECK(path->stem() == "Tests");
			CHECK(FileSystem::Exists(*path));
		}
	}

}
