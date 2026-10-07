#include "TestsPCH.h"

#include "Engine/Platform/Process.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Socket.h"
#include "Support/ChildOutput.h"
#include "Support/ChildProcess.h"
#include "Support/DeathTest.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <charconv>
#include <cstdlib>
#include <system_error>

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
		Test::BlockUntilKilled();
	}

	// Prints the working directory this child started in, then returns: the child of the working-directory test.
	ENGINE_DEATH_TEST("Platform/PrintsWorkingDirectory")
	{
		std::error_code error;
		const std::filesystem::path directory = std::filesystem::current_path(error);
		ENGINE_CORE_WARN("Working directory: [{}]", error ? std::string("unknown") : Test::PathToUtf8(directory));
	}

	// Prints whether a debugger is attached to this child, then returns: the child of the debugger test.
	ENGINE_DEATH_TEST("Platform/PrintsDebuggerAttached")
	{
		ENGINE_CORE_WARN("Debugger attached: [{}]", Process::IsDebuggerAttached());
	}

	// The number in --child-argument; nullopt when it is not one.
	template<typename Number>
	static std::optional<Number> ParseChildArgument()
	{
		const std::string& text = Test::GetTestOptions().ChildArgument;
		Number value{};
		const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
			return std::nullopt;
		return value;
	}

	// Connects to the test's listener at the port in --child-argument and waits until the test closes the connection: a
	// process that a child started, which outlives the child and holds its standard output and standard error.
	ENGINE_DEATH_TEST("Platform/HoldsOutputUntilReleased")
	{
		const std::optional<uint16_t> port = ParseChildArgument<uint16_t>();
		if (!port.has_value())
		{
			ENGINE_CORE_ERROR("The output holder needs a port as --child-argument");
			return;
		}
		Result<Socket> connection = Socket::Connect(*port, std::chrono::seconds(30));
		if (!connection.has_value())
		{
			ENGINE_CORE_ERROR("The output holder cannot connect: {}", connection.error());
			return;
		}
		std::array<std::byte, 1> buffer{};
		const Result<size_t> received = connection->Receive(buffer, std::chrono::seconds(60));
		ENGINE_CORE_WARN("Output holder released: [{}]", received.has_value() ? "closed" : ErrorCodeToString(received.error().GetCode()));
	}

	// Starts HoldsOutputUntilReleased with this process's standard output and standard error, then returns at once: the
	// child of the Run test whose output outlives it.
	ENGINE_DEATH_TEST("Platform/StartsOutputHolder")
	{
		const std::vector<std::string> arguments = {
			"--death-test=Platform/HoldsOutputUntilReleased",
			"--child-argument=" + Test::GetTestOptions().ChildArgument,
			Test::ChildProcessOption,
		};
		const Status started = Test::StartProcessInheritingOutput(Test::GetTestOptions().ExecutablePath, arguments);
		if (!started.has_value())
			ENGINE_CORE_ERROR("The output holder did not start: {}", started.error());
	}

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)
	// Prints whether the descriptor named by --child-argument is open in this child: the child of the inheritance test.
	ENGINE_DEATH_TEST("Platform/PrintsDescriptorState")
	{
		const std::optional<int> descriptor = ParseChildArgument<int>();
		ENGINE_CORE_WARN("Descriptor open: [{}]", descriptor.has_value() ? (Test::IsDescriptorOpen(*descriptor) ? "true" : "false") : "no descriptor given");
	}
#endif

	TEST_SUITE("Platform")
	{
		// Permanently skipped by design, not a contract stub: listed (never run) by the argument test below, whose
		// --test-case argument names it.
		TEST_CASE(QuotingTargetName * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
		}

		TEST_CASE("Process: captures exit code and output")
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

		TEST_CASE("Process: arguments reach the child verbatim")
		{
			// Built before the list: GCC 14 -O2 reports `std::string(...) + name` inside the initializer list as an
			// out-of-bounds memcpy (-Warray-bounds false positive).
			const std::string testCaseArgument = std::format("--test-case={}", QuotingTargetName);
			const std::vector<std::string> arguments = { "--no-skip", "--list-test-cases", testCaseArgument };
			const Result<ProcessResult> listed = Process::Run(Test::MakeTestsChildSpecification(arguments), std::chrono::seconds(60));
			REQUIRE(listed.has_value());
			CHECK(listed->ExitCode == 0);
			CHECK(listed->StandardOutput.contains(QuotingTargetName));
		}

		TEST_CASE("Process: a child that outlives its timeout is killed and reported")
		{
			const Result<ProcessResult> hung = Process::Run(Test::MakeTestsChildSpecification({ "--death-test=Support/Hangs" }),
				std::chrono::milliseconds(500));
			REQUIRE_FALSE(hung.has_value());
			CHECK(hung.error().GetCode() == ErrorCode::Timeout);
			CHECK(hung.error().GetMessageText().contains("ran longer than 500 ms and was killed"));
		}

		TEST_CASE("Process: Run reports output that a process the child started keeps open, without killing anything")
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE_MESSAGE(listener.has_value(), listener.error().ToString());
			// The child exits right after starting the holder, so the timeout only has to cover the child's start-up; Wait
			// then waits all of it for output that the holder keeps open.
			const ProcessSpecification specification = Test::MakeTestsChildSpecification({
				"--death-test=Platform/StartsOutputHolder",
				"--child-argument=" + std::to_string(listener->GetPort()),
			});
			const Result<ProcessResult> result = Process::Run(specification, std::chrono::seconds(5));

			// The holder ends once its connection closes; it is released before any check can stop the test.
			Result<Socket> holder = MakeError(ErrorCode::NotFound, "the child did not start the output holder");
			if (!result.has_value())
				holder = listener->Accept(std::chrono::seconds(30));
			if (holder.has_value())
				holder->Close();

			REQUIRE_FALSE(result.has_value());
			CHECK(holder.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Timeout);
			const std::string& message = result.error().GetMessageText();
			INFO("Run's error: ", message);
			CHECK(message.contains("exited, but a process it started kept its output open for 5000 ms"));
			CHECK_FALSE(message.contains("killed"));
		}

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)
		TEST_CASE("Process: a child inherits no descriptor of the parent beyond its standard streams")
		{
			// A descriptor without close-on-exec, as a library's fopen leaves it (spdlog's log file).
			Test::TempDirectory directory("ProcessDescriptors");
			const std::filesystem::path file = directory / "Inherited.txt";
			const std::string text = "parent descriptor";
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(text)).has_value());
			const Result<int> descriptor = Test::OpenInheritableDescriptor(file, 200);
			REQUIRE_MESSAGE(descriptor.has_value(), descriptor.error().ToString());
			REQUIRE(Test::IsDescriptorOpen(*descriptor));

			const ProcessSpecification specification = Test::MakeTestsChildSpecification({
				"--death-test=Platform/PrintsDescriptorState",
				"--child-argument=" + std::to_string(*descriptor),
			});
			const Result<ProcessResult> result = Process::Run(specification, std::chrono::seconds(60));
			Test::CloseDescriptor(*descriptor);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("child stderr: ", result->StandardError);
			CHECK(Test::FindBracketedValue(result->StandardError, "Descriptor open: ") == "false");
		}
#endif

		TEST_CASE("Process: Spawn, WaitForOutput and Kill control a running child")
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
			CHECK(result->ExitCode == 128 + 9); // what SIGKILL gives on POSIX; Windows uses the same code
			CHECK(result->StandardError.contains("Process test child is ready"));

			const Result<ProcessResult> again = child.Wait(std::chrono::seconds(1));
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("Process: IsRunning reports the current process and a running child, and not a child that has exited")
		{
			CHECK(Process::IsRunning(Process::GetCurrentId()));
			CHECK_FALSE(Process::IsRunning(0));

			Result<Process> spawned = Process::Spawn(Test::MakeTestsChildSpecification({ "--death-test=Platform/AnnouncesAndHangs" }));
			REQUIRE(spawned.has_value());
			Process& child = *spawned;
			REQUIRE(child.WaitForOutput(ProcessStream::StandardError, "Process test child is ready", std::chrono::seconds(60)).has_value());
			const uint32_t id = child.GetId();
			CHECK(Process::IsRunning(id));

			// Kill waits until the child is gone (reaped on POSIX); this Process still holds its handle on Windows, which
			// keeps the ID valid, so IsRunning must look at the process's state, not only at whether the ID resolves.
			REQUIRE(child.Kill().has_value());
			CHECK_FALSE(Process::IsRunning(id));
			CHECK(child.Wait(std::chrono::seconds(10)).has_value());
		}

		TEST_CASE("Process: WaitForOutput reports a stream that ended without the text")
		{
			Result<Process> spawned = Process::Spawn(Test::MakeTestsChildSpecification({ "--death-test=Support/ReturnsWithoutDying" }));
			REQUIRE(spawned.has_value());
			const Status found = spawned->WaitForOutput(ProcessStream::StandardOutput, "never printed", std::chrono::seconds(60));
			REQUIRE_FALSE(found.has_value());
			CHECK(found.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("Process: environment variables reach the child on top of the parent's")
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

		TEST_CASE("Process: the child starts in the working directory, and a relative executable is the parent's")
		{
			Test::TempDirectory directory("ProcessWorkingDirectory");
			std::error_code error;
			const std::filesystem::path expected = std::filesystem::canonical(directory.GetPath(), error);
			REQUIRE_FALSE(error);

			// Relative to this process's working directory, which is not the child's.
			const std::filesystem::path relative = std::filesystem::relative(Test::GetTestOptions().ExecutablePath, error);
			REQUIRE_FALSE(error);
			REQUIRE_MESSAGE(!relative.empty(), "run the tests from a working directory on the drive of the Tests executable");
			REQUIRE(relative.is_relative());

			ProcessSpecification specification = Test::MakeTestsChildSpecification({ "--death-test=Platform/PrintsWorkingDirectory" });
			specification.Executable = relative;
			specification.WorkingDirectory = directory.GetPath();
			const Result<ProcessResult> result = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			const std::string reported = Test::FindBracketedValue(result->StandardError, "Working directory: ");
			REQUIRE_FALSE(reported.empty());
			const std::filesystem::path actual = std::filesystem::canonical(Test::PathFromUtf8(reported), error);
			REQUIRE_FALSE(error);
			CHECK(actual == expected);

			// A working directory that does not exist is the OS refusing to start the child.
			specification.WorkingDirectory = directory / "Missing";
			const Result<Process> missing = Process::Spawn(specification);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::Io);
		}

		TEST_CASE("Process: arguments that are not UTF-8 or contain NUL are rejected before anything starts")
		{
			const std::array<std::string, 2> invalid = { std::string("\xff\xfe"), std::string("before\0after", 12) };
			for (const std::string& argument : invalid)
			{
				const Result<Process> spawned = Process::Spawn(Test::MakeTestsChildSpecification({ "--list-test-cases", argument }));
				REQUIRE_FALSE(spawned.has_value());
				CHECK(spawned.error().GetCode() == ErrorCode::InvalidArgument);
			}

			ProcessSpecification badValue = Test::MakeTestsChildSpecification({ "--list-test-cases" });
			badValue.Environment = { { EnvironmentVariableName, std::string("bad\0value", 9) } };
			const Result<Process> withBadValue = Process::Spawn(badValue);
			REQUIRE_FALSE(withBadValue.has_value());
			CHECK(withBadValue.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Process: a moved Process keeps controlling its child")
		{
			Result<Process> spawned = Process::Spawn(Test::MakeTestsChildSpecification({ "--death-test=Support/ReturnsWithoutDying" }));
			REQUIRE(spawned.has_value());
			const uint32_t id = spawned->GetId();

			Process moved(std::move(*spawned));
			CHECK(moved.GetId() == id);

			// Assigning over a Process whose child still runs kills that child (it never outlives its Process).
			Result<Process> assigned = Process::Spawn(Test::MakeTestsChildSpecification({ "--death-test=Platform/AnnouncesAndHangs" }));
			REQUIRE(assigned.has_value());
			*assigned = std::move(moved);
			CHECK(assigned->GetId() == id);

			const Result<ProcessResult> result = assigned->Wait(std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == 1);
			CHECK(result->StandardError.contains("returned without dying"));
			CHECK(assigned->HasExited());
			CHECK(assigned->Kill().has_value()); // a child that already exited needs no killing
		}

		TEST_CASE("Process: a missing executable is NotFound")
		{
			const std::filesystem::path missing = Test::GetTestOptions().ExecutablePath.parent_path() / "NoSuchProgram";
			const Result<Process> spawned = Process::Spawn({ .Executable = missing });
			REQUIRE_FALSE(spawned.has_value());
			CHECK(spawned.error().GetCode() == ErrorCode::NotFound);

			const Result<Process> directory = Process::Spawn({ .Executable = missing.parent_path() });
			REQUIRE_FALSE(directory.has_value());
			CHECK(directory.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("Process: the current process reports its executable, ID and CPU time")
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

		TEST_CASE("Process: a child started by Process has no debugger attached")
		{
			// This process may run under a debugger; a child it starts never does.
			const Result<ProcessResult> result = Process::Run(
				Test::MakeTestsChildSpecification({ "--death-test=Platform/PrintsDebuggerAttached" }), std::chrono::seconds(60));
			REQUIRE(result.has_value());
			CHECK(Test::FindBracketedValue(result->StandardError, "Debugger attached: ") == "false");
		}
	}

}
