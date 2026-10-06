#include "TestsPCH.h"

#include "Engine/Platform/CrashHandler.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Platform/Process.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

namespace Engine {

	// Runs `Tests --crash-child` with its user data in `directory` and returns the child's result.
	static Result<ProcessResult> RunCrashChild(const Test::TempDirectory& directory, std::vector<std::string> extraArguments = {})
	{
		std::vector<std::string> arguments = { "--crash-child", "--user-data-dir=" + Test::PathToUtf8(directory.GetPath()) };
		for (std::string& argument : extraArguments)
			arguments.push_back(std::move(argument));
		return Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
	}

	// The text of the one crash report in <directory>/<ENGINE_PRODUCT_NAME>/Crashes.
	static Result<std::string> ReadOnlyCrashReport(const Test::TempDirectory& directory)
	{
		const std::filesystem::path crashes = directory.GetPath() / ENGINE_PRODUCT_NAME / "Crashes";
		ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> files, FileSystem::ListDirectory(crashes));
		std::vector<std::filesystem::path> reports;
		for (const std::filesystem::path& file : files)
		{
			const std::string name = Test::PathToUtf8(file.filename());
			if (name.starts_with("crash-") && name.ends_with(".txt"))
				reports.push_back(file);
		}
		if (reports.size() != 1)
			return MakeError(ErrorCode::NotFound, "expected one crash report in '{}', found {}", Test::PathToUtf8(crashes), reports.size());
		return FileSystem::ReadText(reports.front());
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("CrashHandler: child crash produces report and exit code 4" * doctest::skip(true))
		{
			Test::TempDirectory directory("CrashChild");
			const Result<ProcessResult> child = RunCrashChild(directory);
			REQUIRE(child.has_value());
			CHECK(child->ExitCode == ExitCode::Crash);
			CHECK(child->StandardError.contains("report written to"));

			const Result<std::string> report = ReadOnlyCrashReport(directory);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->starts_with("Crash report"));
			CHECK(report->contains("Reason: "));
			CHECK(report->contains(std::string("App: ") + ENGINE_PRODUCT_NAME));
			CHECK(report->contains("Build: "));
			CHECK(report->contains("Process: "));
			CHECK(report->contains("Breadcrumbs:"));
			CHECK(report->contains("FramePhase: Crash child"));
			CHECK(report->contains("Stack trace:"));
			CHECK(report->contains("Last log lines:"));
			// The child's log reached the report: ProcessContext logs every step it initialized.
			CHECK(report->contains("Process context: CrashHandler initialized"));
		}

		TEST_CASE("CrashHandler: a fatal error in a child writes a report naming its kind" * doctest::skip(true))
		{
			Test::TempDirectory directory("FatalChild");
			const Result<ProcessResult> child = RunCrashChild(directory, { "--child-argument=fatal-error" });
			REQUIRE(child.has_value());
			CHECK(child->ExitCode == ExitCode::Crash);
			CHECK(child->StandardError.contains("Fatal error (DeviceLost): Simulated device loss"));

			const Result<std::string> report = ReadOnlyCrashReport(directory);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost): Simulated device loss"));
			CHECK(report->contains("FramePhase: Crash child"));
		}

		TEST_CASE("CrashHandler: the Tests process has the handler installed" * doctest::skip(true))
		{
			CHECK(CrashHandler::IsInstalled());
			// Breadcrumbs are cheap and bounded: an overlong value is truncated, never rejected.
			CrashHandler::SetBreadcrumb(CrashBreadcrumb::AutomationMethod, std::string(CrashHandler::MaxBreadcrumbLength * 2, 'x'));
			CrashHandler::SetBreadcrumb(CrashBreadcrumb::AutomationMethod, {});
		}

		TEST_CASE("CrashHandler: CrashBreadcrumbToString names every breadcrumb" * doctest::skip(true))
		{
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::Scene) == "Scene");
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::PlayState) == "PlayState");
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::AutomationMethod) == "AutomationMethod");
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::ScriptCallback) == "ScriptCallback");
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::FramePhase) == "FramePhase");
		}
	}

}
