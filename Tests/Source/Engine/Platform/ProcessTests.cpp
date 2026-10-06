#include "TestsPCH.h"

#include "Engine/Platform/Process.h"

#include "Engine/Core/Log.h"
#include "Support/DeathTest.h"
#include "Support/TestOptions.h"

#include <condition_variable>
#include <cstdlib>
#include <mutex>

namespace Engine {

	// The name of the quoting target below: quotes, spaces, a backslash before a quote and a trailing backslash, the
	// characters a Windows command line must escape. No comma, '*' or '?', which doctest's --test-case filter interprets.
	static constexpr const char* QuotingTargetName = "Process: the quoting target \"x y\" a\\b\\\"c\\";

	// The variable the environment test sets for its child.
	static constexpr const char* EnvironmentVariableName = "ENGINE_PROCESS_TEST_VARIABLE";

	// The value of the environment variable `name` in this process; nullopt when it is not set.
	static std::optional<std::string> ReadEnvironmentVariable(const std::string& name)
	{
#if defined(ENGINE_PLATFORM_WINDOWS)
		// The MSVC runtime deprecates getenv (C4996); getenv_s reports the size including the terminator.
		size_t size = 0;
		if (getenv_s(&size, nullptr, 0, name.c_str()) != 0 || size == 0)
			return std::nullopt;
		std::string value(size, '\0');
		if (getenv_s(&size, value.data(), value.size(), name.c_str()) != 0 || size == 0)
			return std::nullopt;
		value.resize(size - 1);
		return value;
#else
		const char* value = std::getenv(name.c_str());
		if (value == nullptr)
			return std::nullopt;
		return std::string(value);
#endif
	}

	// Prints the value of EnvironmentVariableName as this child sees it, then returns: the child of the environment test.
	ENGINE_DEATH_TEST("Platform/PrintsEnvironmentVariable")
	{
		const std::optional<std::string> value = ReadEnvironmentVariable(EnvironmentVariableName);
		ENGINE_CORE_WARN("Environment variable: [{}]", value.has_value() ? *value : std::string("unset"));
	}

	// Announces itself on stderr, then blocks until killed: the child of the Spawn/WaitForOutput/Kill test.
	ENGINE_DEATH_TEST("Platform/AnnouncesAndHangs")
	{
		ENGINE_CORE_WARN("Process test child is ready");
		std::mutex mutex;
		std::condition_variable never;
		std::unique_lock lock(mutex);
		never.wait(lock, []()
		{
			return false;
		});
	}

	TEST_SUITE("Platform")
	{
		// Permanently skipped by design, not a contract stub: listed (never run) by the argument test below, whose
		// --test-case argument names it.
		TEST_CASE(QuotingTargetName * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
		}

		TEST_CASE("Process: captures exit code and output" * doctest::skip(true))
		{
			const ProcessSpecification list = Test::MakeTestsChildSpecification({ "--list-test-cases", "--test-case=Process:*" });
			const Result<ProcessResult> listed = Process::Run(list, std::chrono::seconds(60));
			REQUIRE(listed.has_value());
			CHECK(listed->ExitCode == 0);
			CHECK(listed->StandardOutput.contains("Process: captures exit code and output"));

			const ProcessSpecification returning = Test::MakeTestsChildSpecification({ "--death-test=Support/ReturnsWithoutDying" });
			const Result<ProcessResult> returned = Process::Run(returning, std::chrono::seconds(60));
			REQUIRE(returned.has_value());
			CHECK(returned->ExitCode == 1);
			CHECK(returned->StandardError.contains("returned without dying"));

			// Far more than a pipe holds on one stream while the other stays empty.
			const ProcessSpecification flooding = Test::MakeTestsChildSpecification({ "--death-test=Support/FloodsStandardError" });
			const Result<ProcessResult> flooded = Process::Run(flooding, std::chrono::seconds(60));
			REQUIRE(flooded.has_value());
			CHECK(flooded->StandardError.contains("line 1999 of 2000"));
			CHECK(flooded->StandardError.size() > 100000);
		}

		TEST_CASE("Process: arguments reach the child verbatim" * doctest::skip(true))
		{
			const std::vector<std::string> arguments = {
				"--no-skip",
				"--list-test-cases",
				std::string("--test-case=") + QuotingTargetName,
			};
			const Result<ProcessResult> listed = Process::Run(Test::MakeTestsChildSpecification(arguments), std::chrono::seconds(60));
			REQUIRE(listed.has_value());
			CHECK(listed->ExitCode == 0);
			CHECK(listed->StandardOutput.contains(QuotingTargetName));
		}

		TEST_CASE("Process: a child that outlives its timeout is killed and reported" * doctest::skip(true))
		{
			const Result<ProcessResult> hung = Process::Run(Test::MakeTestsChildSpecification({ "--death-test=Support/Hangs" }),
				std::chrono::milliseconds(500));
			REQUIRE_FALSE(hung.has_value());
			CHECK(hung.error().GetCode() == ErrorCode::Timeout);
		}

		TEST_CASE("Process: Spawn, WaitForOutput and Kill control a running child" * doctest::skip(true))
		{
			Result<Process> spawned = Process::Spawn(Test::MakeTestsChildSpecification({ "--death-test=Platform/AnnouncesAndHangs" }));
			REQUIRE(spawned.has_value());
			Process& child = *spawned;
			CHECK(child.GetId() != 0);
			CHECK(child.GetId() != Process::GetCurrentId());

			REQUIRE(child.WaitForOutput(ProcessStream::StandardError, "Process test child is ready", std::chrono::seconds(60)).has_value());
			CHECK_FALSE(child.HasExited());

			const Result<ProcessResult> early = child.Wait(std::chrono::milliseconds(0));
			REQUIRE_FALSE(early.has_value());
			CHECK(early.error().GetCode() == ErrorCode::Timeout);

			REQUIRE(child.Kill().has_value());
			CHECK(child.HasExited());
			CHECK(child.Kill().has_value()); // idempotent once the child is gone

			const Result<ProcessResult> result = child.Wait(std::chrono::seconds(10));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode != 0);
			CHECK(result->StandardError.contains("Process test child is ready"));

			const Result<ProcessResult> again = child.Wait(std::chrono::seconds(1));
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("Process: WaitForOutput reports a stream that ended without the text" * doctest::skip(true))
		{
			Result<Process> spawned = Process::Spawn(Test::MakeTestsChildSpecification({ "--death-test=Support/ReturnsWithoutDying" }));
			REQUIRE(spawned.has_value());
			const Status found = spawned->WaitForOutput(ProcessStream::StandardOutput, "never printed", std::chrono::seconds(60));
			REQUIRE_FALSE(found.has_value());
			CHECK(found.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("Process: environment variables reach the child on top of the parent's" * doctest::skip(true))
		{
			// Not set in this process, so the child inherits nothing for it.
			REQUIRE_FALSE(ReadEnvironmentVariable(EnvironmentVariableName).has_value());
			const Result<ProcessResult> inherited = Process::Run(
				Test::MakeTestsChildSpecification({ "--death-test=Platform/PrintsEnvironmentVariable" }), std::chrono::seconds(60));
			REQUIRE(inherited.has_value());
			CHECK(inherited->StandardError.contains("Environment variable: [unset]"));

			ProcessSpecification specification = Test::MakeTestsChildSpecification({ "--death-test=Platform/PrintsEnvironmentVariable" });
			specification.Environment = { { EnvironmentVariableName, "first" }, { EnvironmentVariableName, "second value" } };
			const Result<ProcessResult> overridden = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE(overridden.has_value());
			CHECK(overridden->StandardError.contains("Environment variable: [second value]")); // the later entry wins
			CHECK_FALSE(ReadEnvironmentVariable(EnvironmentVariableName).has_value());         // the parent is unchanged

			const std::array<std::pair<std::string, std::string>, 3> invalid = { {
				{ "", "value" },
				{ "NAME=WITH_EQUALS", "value" },
				{ std::string("NAME\0NUL", 8), "value" },
			} };
			for (const std::pair<std::string, std::string>& entry : invalid)
			{
				ProcessSpecification rejected = Test::MakeTestsChildSpecification({ "--death-test=Platform/PrintsEnvironmentVariable" });
				rejected.Environment = { entry };
				const Result<Process> spawned = Process::Spawn(rejected);
				REQUIRE_FALSE(spawned.has_value());
				CHECK(spawned.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("Process: a missing executable is NotFound" * doctest::skip(true))
		{
			const std::filesystem::path missing = Test::GetTestOptions().ExecutablePath.parent_path() / "NoSuchProgram";
			const Result<Process> spawned = Process::Spawn({ .Executable = missing });
			REQUIRE_FALSE(spawned.has_value());
			CHECK(spawned.error().GetCode() == ErrorCode::NotFound);

			const Result<Process> directory = Process::Spawn({ .Executable = missing.parent_path() });
			REQUIRE_FALSE(directory.has_value());
			CHECK(directory.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("Process: the current process reports its executable, ID and CPU time" * doctest::skip(true))
		{
			const Result<std::filesystem::path> executable = Process::GetCurrentExecutablePath();
			REQUIRE(executable.has_value());
			CHECK(executable->is_absolute());
			CHECK(*executable == Test::GetTestOptions().ExecutablePath);
			CHECK(executable->stem() == "Tests");

			CHECK(Process::GetCurrentId() != 0);
			CHECK(Process::GetCurrentId() == Process::GetCurrentId());

			const Result<double> before = Process::GetCurrentCpuSeconds();
			REQUIRE(before.has_value());
			CHECK(*before >= 0.0);
			// Work until the CPU clock moves (its resolution is coarse on some hosts); bounded by the test timeout.
			uint64_t accumulator = 0x9e3779b97f4a7c15ull;
			Result<double> after = before;
			while (after.has_value() && *after <= *before)
			{
				for (int round = 0; round < 1000000; ++round)
					accumulator = accumulator * 6364136223846793005ull + 1442695040888963407ull;
				after = Process::GetCurrentCpuSeconds();
			}
			REQUIRE(after.has_value());
			CHECK(*after > *before);
			CHECK(accumulator != 0);
		}
	}

}
