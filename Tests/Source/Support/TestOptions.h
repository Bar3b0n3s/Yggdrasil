#pragma once

#include "Engine/Core/Result.h"

#include <filesystem>
#include <string>

// The Tests binary's own command-line options (Architecture §15.2), next to doctest's. The Tests main
// (Tests/Source/TestMain.cpp) does, in order:
//   1. ParseTestOptions; a malformed option exits with code 2 (UsageError).
//   2. Log::Initialize with the console sink (stderr) and Profiler::Initialize; InstallRecordingAssertHandler.
//   3. With --death-test=<name>: return RunDeathTestBody(name) (DeathTest.h); doctest does not run.
//   4. Otherwise: InstallExpectLogListener, StartTestTimeoutWatchdog, run doctest with the remaining arguments, stop the
//      watchdog, shut down the profiler and the log, and return doctest's result.
// M2 adds --windowed-child=<name>, M5 --require-gpu and the golden-image options, each with its own field here.

namespace Engine {

	namespace Test {

		// The suite of test cases that exist only to be run in a child process by another test (the targets of the
		// watchdog and recording-assert-handler tests). They are permanently skipped because they hang or terminate the
		// process by design, so a manual `Tests --no-skip` run excludes them: --test-suite-exclude=ChildTargets.
		inline constexpr const char* ChildTargetSuite = "ChildTargets";

		struct TestOptions
		{
			// The running Tests executable as an absolute path (GetCurrentExecutablePath), used to spawn child processes.
			std::filesystem::path ExecutablePath;
			// --death-test=<name>: run the body of the death test <name> in this process and exit (child mode).
			std::string DeathTest;
			// --test-timeout=<seconds>: the per-test-case limit for cases without a doctest::timeout decorator (> 0).
			double DefaultTimeoutSeconds = 120.0;
		};

		// Parses the engine options in argv[1..argc) and sets ExecutablePath from GetCurrentExecutablePath(), falling back
		// to argv[0] made absolute only when the OS query fails; arguments that are not engine options (doctest's) are
		// ignored. Errors: InvalidArgument naming the malformed option (an empty --death-test name, a non-positive or
		// non-numeric --test-timeout), Io when neither the OS nor argv[0] yields an absolute path.
		[[nodiscard]] Result<TestOptions> ParseTestOptions(int argc, const char* const* argv);

		// The options of this run: set once by the main before any test runs (process-level state of the Tests binary), then
		// read-only. GetTestOptions before SetTestOptions returns default options.
		void SetTestOptions(TestOptions options);
		[[nodiscard]] const TestOptions& GetTestOptions();

	}

}
