#include "TestsPCH.h"

#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/RecordingAssertHandler.h"
#include "Support/TestOptions.h"
#include "Support/TestTimeout.h"

// doctest guards its implementation separately from its interface, so including the header again after
// DOCTEST_CONFIG_IMPLEMENT compiles the test runner into this translation unit only.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

// The Tests main (Architecture §15.2), in the order Support/TestOptions.h documents. Exit codes follow the §4.1 table:
// doctest's result (0 or 1), 2 for a malformed engine option, 3 when the log cannot be initialized, and in a death-test
// child whatever RunDeathTestBody returns or the body's own termination (4 for an assert).

namespace Engine {

	namespace Test {

		// The UsageError exit code of the §4.1 table (App/ExitCode.h arrives with M2).
		constexpr int UsageErrorExitCode = 2;

		static int RunTests(int argc, char** argv)
		{
			const Result<TestOptions> options = ParseTestOptions(argc, argv);
			if (!options.has_value())
			{
				ENGINE_CORE_ERROR("Invalid command line: {}", options.error()); // before Initialize: the fallback logger (stderr)
				return UsageErrorExitCode;
			}
			SetTestOptions(*options);

			// The console sink writes to stderr, where death-test parents look for assert messages; doctest owns stdout.
			const Status logInitialized = Log::Initialize(LogSpecification());
			if (!logInitialized.has_value())
			{
				// Without the log, ExpectLog would see no entries and undeclared errors would pass unnoticed.
				ENGINE_CORE_CRITICAL("Cannot initialize the log: {}", logInitialized.error());
				return FatalInitFailedExitCode;
			}
			Profiler::Initialize();
			InstallRecordingAssertHandler();

			int exitCode = 0;
			if (!options->DeathTest.empty())
			{
				exitCode = RunDeathTestBody(options->DeathTest);
			}
			else
			{
				InstallExpectLogListener();
				StartTestTimeoutWatchdog(options->DefaultTimeoutSeconds);
				{
					doctest::Context context;
					context.applyCommandLine(argc, argv);
					exitCode = context.run();
				}
				StopTestTimeoutWatchdog();
				UninstallExpectLogListener();
			}

			Profiler::Shutdown();
			Log::Shutdown();
			return exitCode;
		}

	}

}

int main(int argc, char** argv)
{
	return Engine::Test::RunTests(argc, argv);
}
