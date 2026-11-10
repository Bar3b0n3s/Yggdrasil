#include "TestsPCH.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The Tests main's exit watch (Architecture §15.2): the process ends only after doctest's run, so an exit during it
// leaves a crash report naming the exiting thread, its stack and the running test case.

namespace Engine {

	// The names of the child-process targets below.
	static constexpr std::string_view ExitingTarget = "TestMain: the target calls exit(1) on another thread";
	static constexpr std::string_view ExitProcessTarget = "TestMain: the target ends the process with std::_Exit(1)";
	static constexpr std::string_view PassingTarget = "TestMain: the target passes";

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
