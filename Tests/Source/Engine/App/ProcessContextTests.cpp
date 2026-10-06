#include "TestsPCH.h"

#include "Engine/App/ProcessContext.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Process.h"
#include "Support/DeathTest.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

namespace Engine {

	static GlfwPlatform GetNativePlatform()
	{
#if defined(ENGINE_PLATFORM_WINDOWS)
		return GlfwPlatform::Win32;
#elif defined(ENGINE_PLATFORM_MACOS)
		return GlfwPlatform::Cocoa;
#elif defined(ENGINE_PLATFORM_LINUX)
		return GlfwPlatform::X11;
#endif
	}

	// The Tests main already created this process's context, so a second one is API misuse.
	ENGINE_DEATH_TEST("App/SecondProcessContext")
	{
		static_cast<void>(ProcessContext::Create({ .AppName = "Second", .Window = WindowMode::Headless, .LogToFile = false }));
	}

	// Prints the file settings the Tests main gave this child's ProcessContext, then returns.
	ENGINE_DEATH_TEST("App/ReportsChildProcessFiles")
	{
		const ProcessContext* context = ProcessContext::GetCurrent();
		if (context == nullptr)
		{
			ENGINE_CORE_ERROR("The child has no process context");
			return;
		}
		ENGINE_CORE_WARN("Child files: log file {}, crash reports {}", context->GetSpecification().LogToFile,
			context->GetSpecification().WriteCrashReports);
	}

	// Sets a fatal-error hook that logs, then fails with FatalError: the child of the hook test.
	ENGINE_DEATH_TEST("App/FatalErrorHookRuns")
	{
		ProcessContext* context = ProcessContext::GetCurrent();
		if (context == nullptr)
		{
			ENGINE_CORE_ERROR("The child has no process context");
			return;
		}
		std::string owner = "the editor";
		context->SetFatalErrorHook({
			.Function = [](void* userData, FatalErrorKind kind, std::string_view message)
		{
			const std::string& hookOwner = *static_cast<const std::string*>(userData);
			ENGINE_CORE_WARN("Fatal error hook ran: {} ({}) for {}", FatalErrorKindToString(kind), message, hookOwner);
		},
			.UserData = &owner,
		});
		FatalError(FatalErrorKind::DeviceLost, "Simulated device loss for the hook");
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

	TEST_SUITE("App")
	{
		// Runs only in a windowed child process (Support/WindowedChild.h).
		TEST_CASE("ProcessContext: a windowed child runs GLFW on the native platform"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			const ProcessContext* context = ProcessContext::GetCurrent();
			REQUIRE(context != nullptr);
			CHECK(context->GetSpecification().Window == WindowMode::Windowed);
			CHECK(context->GetGlfwPlatform() == GetNativePlatform());
			CHECK(GlfwLibrary::GetPlatform() == GetNativePlatform());
		}

		TEST_CASE("ProcessContext: initializes GLFW once with the chosen platform and tears down in reverse" * doctest::skip(true))
		{
			// This process: headless, every step done in order, GLFW on the null platform.
			const ProcessContext* context = ProcessContext::GetCurrent();
			REQUIRE(context != nullptr);
			CHECK(context->GetSpecification().Window == WindowMode::Headless);
			CHECK(context->GetGlfwPlatform() == GlfwPlatform::Null);
			const std::vector<ProcessContextStep> steps(context->GetSteps().begin(), context->GetSteps().end());
			const std::vector<ProcessContextStep> expected = {
				ProcessContextStep::Log,
				ProcessContextStep::Profiler,
				ProcessContextStep::CrashHandler,
				ProcessContextStep::Glfw,
			};
			CHECK(steps == expected);

			// A windowed child: the native platform, and its log shows initialization in order and teardown in reverse.
			const Result<Test::WindowedChildResult> child =
				Test::RunWindowedChild("ProcessContext: a windowed child runs GLFW on the native platform");
			REQUIRE(child.has_value());
			CHECK(child->ExitCode == 0);

			const std::array<std::string_view, 8> lines = {
				"Process context: Log initialized",
				"Process context: Profiler initialized",
				"Process context: CrashHandler initialized",
				"Process context: Glfw initialized",
				"Process context: shutting down Glfw",
				"Process context: shutting down CrashHandler",
				"Process context: shutting down Profiler",
				"Process context: shutting down Log",
			};
			size_t position = 0;
			for (const std::string_view line : lines)
			{
				CAPTURE(std::string(line));
				const size_t found = child->StandardError.find(line, position);
				REQUIRE(found != std::string::npos);
				position = found + line.size();
			}
		}

		TEST_CASE("ProcessContext: a second context in one process asserts" * doctest::skip(true))
		{
			ENGINE_CHECK_DEATH("App/SecondProcessContext", "a ProcessContext already exists");
		}

		TEST_CASE("ProcessContext: the user-data folders and the log file follow the app name" * doctest::skip(true))
		{
			const ProcessContext* context = ProcessContext::GetCurrent();
			REQUIRE(context != nullptr);
			CHECK(context->GetSpecification().AppName == ENGINE_PRODUCT_NAME);
			const UserDataPaths& paths = context->GetUserDataPaths();
			CHECK(paths.Root.filename() == ENGINE_PRODUCT_NAME);
			CHECK(paths.Logs == paths.Root / "Logs");
			CHECK(paths.Crashes == paths.Root / "Crashes");
			std::error_code error;
			CHECK(std::filesystem::is_directory(paths.Logs, error));
			CHECK(std::filesystem::is_directory(paths.Crashes, error));

			// The top-level Tests run logs to <UserData>/<AppName>/Logs/<exe>.log; child processes log to the console only.
			REQUIRE_FALSE(Test::GetTestOptions().IsChildProcess());
			CHECK(context->GetSpecification().LogToFile);
			CHECK(context->GetSpecification().WriteCrashReports);
			CHECK(context->GetLogFilePath() == paths.GetLogFile("Tests"));
		}

		TEST_CASE("ProcessContext: a Tests child writes no log file and crash reports only under the root its parent gave it"
			* doctest::skip(true))
		{
			const ProcessSpecification plain = Test::MakeTestsChildSpecification({ "--death-test=App/ReportsChildProcessFiles" });
			const Result<ProcessResult> plainChild = Process::Run(plain, std::chrono::seconds(60));
			REQUIRE(plainChild.has_value());
			CHECK(plainChild->StandardError.contains("Child files: log file false, crash reports false"));

			Test::TempDirectory directory("ChildProcessFiles");
			const ProcessSpecification redirected = Test::MakeTestsChildSpecification({
				"--death-test=App/ReportsChildProcessFiles",
				"--user-data-dir=" + Test::PathToUtf8(directory.GetPath()),
			});
			const Result<ProcessResult> redirectedChild = Process::Run(redirected, std::chrono::seconds(60));
			REQUIRE(redirectedChild.has_value());
			CHECK(redirectedChild->StandardError.contains("Child files: log file false, crash reports true"));
		}

		TEST_CASE("ProcessContext: the fatal-error hook runs before the crash report is written" * doctest::skip(true))
		{
			Test::TempDirectory directory("FatalErrorHook");
			const ProcessSpecification specification = Test::MakeTestsChildSpecification({
				"--death-test=App/FatalErrorHookRuns",
				"--user-data-dir=" + Test::PathToUtf8(directory.GetPath()),
			});
			const Result<ProcessResult> child = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE(child.has_value());
			CHECK(child->ExitCode == ExitCode::Crash);
			CHECK(child->StandardError.contains("Fatal error hook ran: DeviceLost (Simulated device loss for the hook) for the editor"));

			// The report holds the hook's log line, so the hook ran first.
			const Result<std::string> report = ReadOnlyCrashReport(directory);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost): Simulated device loss for the hook"));
			CHECK(report->contains("Fatal error hook ran: DeviceLost"));
		}

		TEST_CASE("ProcessContext: ProcessContextStepToString names every step and GetBuildDescription names the build"
			* doctest::skip(true))
		{
			CHECK(ProcessContextStepToString(ProcessContextStep::Log) == "Log");
			CHECK(ProcessContextStepToString(ProcessContextStep::Profiler) == "Profiler");
			CHECK(ProcessContextStepToString(ProcessContextStep::CrashHandler) == "CrashHandler");
			CHECK(ProcessContextStepToString(ProcessContextStep::Glfw) == "Glfw");

			const std::string_view build = GetBuildDescription();
#if defined(ENGINE_DEBUG)
			CHECK(build.starts_with("Debug "));
#elif defined(ENGINE_RELEASE)
			CHECK(build.starts_with("Release "));
#endif
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(build.ends_with(" windows-x86_64"));
#elif defined(ENGINE_PLATFORM_LINUX)
			CHECK(build.ends_with(" linux-x86_64"));
#elif defined(ENGINE_PLATFORM_MACOS)
			CHECK(build.ends_with(" macos-arm64"));
#endif
		}
	}

}
