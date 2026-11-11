#include "TestsPCH.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The Tests main's exit watch (Architecture §15.2): the process ends only after doctest's run, so an exit during it
// leaves a crash report naming the exiting thread, its stack and the running test case.

namespace Engine {

	// The name of the child-process target the portable case below runs.
	static constexpr std::string_view PassingTarget = "TestMain: the target passes";
	// What the faulting target logs before it faults, followed by its thread's id.
	static constexpr std::string_view FaultingThreadLine = "Raising an access violation on thread ";

	// Runs the child-process target `target` in a Tests child whose user-data root is `userData`, waiting at most a minute.
	static Result<ProcessResult> RunTarget(std::string_view target, const Test::TempDirectory& userData)
	{
		return Process::Run(Test::MakeTestsChildSpecification({
								"--no-skip",
								std::format("--test-case={}", target),
								"--user-data-dir=" + Test::PathToUtf8(userData.GetPath()),
							}),
			std::chrono::seconds(60));
	}

	// The crash reports the child whose user-data root is `userData` wrote, in name order.
	static std::vector<std::filesystem::path> ListReports(const Test::TempDirectory& userData)
	{
		return Test::ListCrashFiles(userData / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
	}

#if defined(ENGINE_PLATFORM_WINDOWS)
	// The names of the child-process targets the Windows-only cases below run (with the reader of the faulting one's
	// minidump, inside the same block, so a build for another platform has no unused function or constant: M23's milestone
	// review).
	static constexpr std::string_view ExitingTarget = "TestMain: the target calls exit(1) on another thread";
	static constexpr std::string_view ExitProcessTarget = "TestMain: the target ends the process with std::_Exit(1)";
	static constexpr std::string_view FaultingTarget = "TestMain: the target raises an access violation on another thread";

	// The thread a minidump's exception stream names: the ThreadId that begins the MINIDUMP_EXCEPTION_STREAM of the stream
	// directory's ExceptionStream entry (type 6), read from the little-endian file; nullopt for a file that is not a
	// minidump or has no such stream.
	static std::optional<uint32_t> ReadMinidumpExceptionThreadId(std::span<const std::byte> dump)
	{
		const auto read = [dump](size_t offset) -> std::optional<uint32_t>
		{
			if (offset > dump.size() || dump.size() - offset < sizeof(uint32_t))
				return std::nullopt;
			uint32_t value = 0;
			std::memcpy(&value, dump.data() + offset, sizeof(value));
			return value;
		};
		constexpr uint32_t Signature = 0x504d444du; // "MDMP"
		constexpr uint32_t ExceptionStream = 6;
		constexpr size_t DirectoryEntrySize = 12; // StreamType, DataSize, Rva
		const std::optional<uint32_t> streams = read(8);
		const std::optional<uint32_t> directory = read(12);
		if (read(0) != Signature || !streams.has_value() || !directory.has_value())
			return std::nullopt;
		for (uint32_t index = 0; index < *streams; ++index)
		{
			const size_t entry = size_t{ *directory } + size_t{ index } * DirectoryEntrySize;
			if (read(entry) == ExceptionStream)
			{
				const std::optional<uint32_t> location = read(entry + 8);
				return location.has_value() ? read(*location) : std::nullopt;
			}
		}
		return std::nullopt;
	}
#endif

	TEST_SUITE("Support")
	{
		// Permanently skipped by design, not contract stubs: the targets of the exit-watch tests below, which run them in a
		// child with --no-skip. Two end their process, so all three belong to Test::ChildTargetSuite.
		TEST_CASE("TestMain: the target calls exit(1) on another thread" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			// As a driver's thread would: the thread doctest runs the case on waits for it.
			std::thread exiting([]()
			{
				std::exit(1);
			});
			exiting.join();
		}

		TEST_CASE("TestMain: the target ends the process with std::_Exit(1)" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			// std::_Exit runs no exit() handler: it calls ExitProcess.
			std::_Exit(1);
		}

		TEST_CASE("TestMain: the target passes" * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			CHECK(Test::GetTestOptions().IsChildProcess());
		}

		TEST_CASE("TestMain: the target raises an access violation on another thread" * doctest::test_suite(Test::ChildTargetSuite)
			* doctest::skip(true))
		{
			// As a driver's thread faulted in R-GPUCRASH's runs (Docs/Backlog.md §3): no code of the thread handles it.
			std::thread faulting([]()
			{
				ENGINE_CORE_INFO("{}{}", FaultingThreadLine, std::format("{}", std::this_thread::get_id()));
				CrashHandler::SimulateCrash();
			});
			faulting.join();
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		TEST_CASE("TestMain: an exit() during the doctest run leaves a crash report naming the running test case on Windows")
		{
			Test::TempDirectory userData("ExitDuringRun");
			const Result<ProcessResult> child = RunTarget(ExitingTarget, userData);
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode != 0);
			CHECK(child->StandardError.contains("Unexpected exit (exit()) on thread "));

			// The exit watch's report is the first: what the C runtime's exit does after it cannot change it.
			const std::vector<std::filesystem::path> reports = ListReports(userData);
			REQUIRE_FALSE(reports.empty());
			const Result<std::string> report = FileSystem::ReadText(reports.front());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			INFO("report: ", *report);
			CHECK(report->contains("\nReason: Unexpected exit (exit()) on thread "));
			CHECK(report->contains(", not the thread that began the exit watch\n"));
			CHECK(report->contains(std::format("\n  TestCase: {}\n", ExitingTarget)));
			CHECK(report->contains("\nStack trace:\n  #0 0x"));
		}

		// R-GPUCRASH (Docs/Backlog.md §3; Docs/Decisions/0034-m23-contract.md, "Landing"): doctest's fatal-condition handler
		// takes an unhandled structured exception during a case ahead of the engine's crash handler, prints CRASHED and calls
		// std::exit on the faulting thread, so the exit watch reports that exit: its report names the faulting thread and its
		// stack down through the exception's dispatch to the faulting frame, and the minidump beside it names the same thread.
		TEST_CASE("TestMain: an access violation on another thread during the doctest run leaves a report naming that thread on Windows")
		{
			Test::TempDirectory userData("FaultDuringRun");
			const Result<ProcessResult> child = RunTarget(FaultingTarget, userData);
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode != 0);
			CHECK(child->StandardOutput.contains("CRASHED"));
			CHECK(child->StandardError.contains("Unexpected exit (exit()) on thread "));

			const std::vector<std::filesystem::path> reports = ListReports(userData);
			REQUIRE_FALSE(reports.empty());
			const Result<std::string> report = FileSystem::ReadText(reports.front());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			INFO("report: ", *report);
			// The faulting thread's id, from the line it logged before the fault (the report's last log lines).
			const size_t logged = report->find(FaultingThreadLine);
			REQUIRE(logged != std::string::npos);
			const size_t idStart = logged + FaultingThreadLine.size();
			uint32_t faultingThread = 0;
			const std::from_chars_result parsed = std::from_chars(report->data() + idStart, report->data() + report->size(), faultingThread);
			REQUIRE(parsed.ec == std::errc{});
			CHECK(report->contains(std::format("\nReason: Unexpected exit (exit()) on thread {}, not the thread that began the exit watch\n",
				faultingThread)));
			CHECK(report->contains(std::format("\n  TestCase: {}\n", FaultingTarget)));
			const size_t stack = report->find("\nStack trace:\n");
			const size_t logLines = report->find("\nLast log lines:", stack);
			REQUIRE(stack != std::string::npos);
			REQUIRE(logLines != std::string::npos);
			CHECK(std::string_view(*report).substr(stack, logLines - stack).contains("SimulateCrash"));

			std::filesystem::path dump = reports.front();
			dump.replace_extension(".dmp");
			const Result<Buffer> dumpBytes = FileSystem::ReadFile(dump);
			REQUIRE_MESSAGE(dumpBytes.has_value(), dumpBytes.error().ToString());
			CHECK(ReadMinidumpExceptionThreadId(*dumpBytes) == faultingThread);
		}

		TEST_CASE("TestMain: an ExitProcess during the doctest run leaves a crash report naming its exit code on Windows")
		{
			Test::TempDirectory userData("ExitProcessDuringRun");
			const Result<ProcessResult> child = RunTarget(ExitProcessTarget, userData);
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == 1);

			const std::vector<std::filesystem::path> reports = ListReports(userData);
			REQUIRE(reports.size() == 1);
			const Result<std::string> report = FileSystem::ReadText(reports.front());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			INFO("report: ", *report);
			CHECK(report->contains("\nReason: Unexpected exit (ExitProcess) with exit code 1 on thread "));
			CHECK(report->contains(", the thread that began the exit watch\n"));
			CHECK(report->contains(std::format("\n  TestCase: {}\n", ExitProcessTarget)));
			CHECK(report->contains("\nStack trace:\n  #0 0x"));
		}
#endif

		TEST_CASE("TestMain: a doctest run that ends normally leaves no crash report")
		{
			Test::TempDirectory userData("RunEndsNormally");
			const Result<ProcessResult> child = RunTarget(PassingTarget, userData);
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == 0);
			CHECK_FALSE(child->StandardError.contains("Unexpected exit"));
			CHECK(ListReports(userData).empty());
		}
	}

}
