#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/CrashHandler.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/RecordingAssertHandler.h"
#include "Support/TestOptions.h"
#include "Support/TestTimeout.h"
#include "Support/WindowedChild.h"

// doctest guards its implementation separately from its interface, so including the header again after
// DOCTEST_CONFIG_IMPLEMENT compiles the test runner into this translation unit only.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

// The Tests main (Architecture §15.2), in the order Support/TestOptions.h documents. Exit codes follow the §4.1 table:
// doctest's result (0 or 1), 2 for a malformed engine option, 3 when the ProcessContext cannot be created, and in a
// child mode whatever its body returns or the body's own termination (4 for an assert or a crash).

namespace Engine {

	namespace Test {

		// The --child-argument of --crash-child that selects the fatal-error path instead of a simulated crash.
		constexpr std::string_view CrashChildFatalErrorArgument = "fatal-error";

		// --crash-child: crash after the ProcessContext installed the crash handler (Roadmap M2). Returns only for a
		// --child-argument it does not know.
		static int RunCrashChild(const TestOptions& options)
		{
			CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "Crash child");
			if (options.ChildArgument.empty())
				CrashHandler::SimulateCrash();
			if (options.ChildArgument == CrashChildFatalErrorArgument)
				FatalError(FatalErrorKind::DeviceLost, "Simulated device loss");
			ENGINE_CORE_ERROR("--crash-child takes no --child-argument other than '{}', got '{}'", CrashChildFatalErrorArgument,
				options.ChildArgument);
			return ExitCode::UsageError;
		}

		// doctest with the test harness of a normal run: the ExpectLog listener and the per-test-case watchdog.
		static int RunDoctest(int argc, char** argv, const TestOptions& options)
		{
			InstallExpectLogListener();
			StartTestTimeoutWatchdog(options.DefaultTimeoutSeconds);
			int exitCode = 0;
			if (!options.WindowedChild.empty())
			{
				exitCode = RunWindowedChildTestCase(options.WindowedChild);
			}
			else
			{
				doctest::Context context;
				context.applyCommandLine(argc, argv);
				exitCode = context.run();
			}
			StopTestTimeoutWatchdog();
			UninstallExpectLogListener();
			return exitCode;
		}

		static int RunTests(int argc, char** argv)
		{
			const Result<TestOptions> options = ParseTestOptions(argc, argv);
			if (!options.has_value())
			{
				ENGINE_CORE_ERROR("Invalid command line: {}", options.error()); // before the log: the fallback logger (stderr)
				return ExitCode::UsageError;
			}
			SetTestOptions(*options);

			// The process level (§4.1): headless, except in a windowed child. Which process writes which file follows
			// IsChildProcess() (TestOptions.h): a child logs to the console its parent captures, and writes crash reports
			// only into the user-data root its parent gave it.
			Result<Scope<ProcessContext>> created = ProcessContext::Create({
				.AppName = ENGINE_PRODUCT_NAME,
				.Window = options->WindowedChild.empty() ? WindowMode::Headless : WindowMode::Windowed,
				.UserDataRoot = options->UserDataDirectory,
				.LogToFile = !options->IsChildProcess(),
				.WriteCrashReports = !options->IsChildProcess() || !options->UserDataDirectory.empty(),
			});
			if (!created.has_value())
			{
				// Without the log, ExpectLog would see no entries and undeclared errors would pass unnoticed.
				ENGINE_CORE_CRITICAL("Cannot initialize the process: {}", created.error());
				return ExitCode::InitFailed;
			}
			Scope<ProcessContext> processContext = std::move(*created);
			InstallRecordingAssertHandler();

			int exitCode = 0;
			if (!options->DeathTest.empty())
				exitCode = RunDeathTestBody(options->DeathTest);
			else if (options->CrashChild)
				exitCode = RunCrashChild(*options);
			else
				exitCode = RunDoctest(argc, argv, *options);

			processContext.reset();
			return exitCode;
		}

	}

}

int main(int argc, char** argv)
{
	return Engine::Test::RunTests(argc, argv);
}
