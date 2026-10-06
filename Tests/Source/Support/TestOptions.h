#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Platform/Process.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The Tests binary's own command-line options (Architecture §15.2), next to doctest's. The Tests main
// (Tests/Source/TestMain.cpp) does, in order:
//   1. ParseTestOptions; a malformed option exits with code 2 (UsageError).
//   2. Log::Initialize with the console sink (stderr); a failure exits with code 3 (InitFailed), because without the
//      log ExpectLog would see no entries. Then Profiler::Initialize and InstallRecordingAssertHandler.
//   3. With --death-test=<name>: RunDeathTestBody(name) (DeathTest.h) instead of doctest, then shut down the profiler and
//      the log and return its exit code.
//   4. Otherwise: InstallExpectLogListener, StartTestTimeoutWatchdog, run doctest with the remaining arguments, stop the
//      watchdog, UninstallExpectLogListener, shut down the profiler and the log, and return doctest's result.
// M2 replaces the log and profiler start of step 2 with a ProcessContext (App/ProcessContext.h: headless,
// ENGINE_PRODUCT_NAME, UserDataDirectory as the user-data root) and adds two child modes: --windowed-child
// (WindowedChild.h: a native-platform ProcessContext runs one ChildTargets test case) and --crash-child
// (CrashHandler::SimulateCrash). Which process writes which file follows IsChildProcess(), never a guess from the mode:
//   - the log file <UserData>/<AppName>/Logs/Tests.log only when !IsChildProcess() (LogToFile), so two processes never
//     write and rotate one Tests.log; a child logs to the console, which its parent captures;
//   - crash reports when !IsChildProcess() or UserDataDirectory is set (WriteCrashReports): a child writes them only into
//     the directory its parent gave it (the crash-child tests), never into the user's real folder, so the expected deaths
//     of death tests and recording-assert-handler targets leave no report behind.
// M5 adds --require-gpu and the golden-image options, each with its own field here.

namespace Engine {

	namespace Test {

		// The suite of test cases that exist only to be run in a child process by another test (the targets of the
		// watchdog, recording-assert-handler and windowed-child tests). They are permanently skipped because they hang,
		// terminate the process or need a windowed process by design, so a manual `Tests --no-skip` run excludes them:
		// --test-suite-exclude=ChildTargets.
		inline constexpr const char* ChildTargetSuite = "ChildTargets";

		// The option every Tests process started by another Tests process carries (TestOptions::ChildProcess). The parent
		// side always appends it: RunChildProcess (ChildProcess.h), RunWindowedChild (WindowedChild.h) and
		// MakeTestsChildSpecification (below), the only ways tests start the Tests executable.
		inline constexpr const char* ChildProcessOption = "--child-process";

		struct TestOptions
		{
			// The running Tests executable as an absolute path (Process::GetCurrentExecutablePath, through
			// GetCurrentExecutablePath in ChildProcess.h), used to spawn child processes.
			std::filesystem::path ExecutablePath{};
			// --death-test=<name>: run the body of the death test <name> in this process and exit (child mode).
			std::string DeathTest{};
			// --windowed-child=<test case>: run that one ChildTargets test case in a process whose ProcessContext uses
			// GLFW's native platform, and exit with doctest's result (child mode, WindowedChild.h).
			std::string WindowedChild{};
			// --crash-child: after the ProcessContext installed the crash handler, set the FramePhase breadcrumb to
			// "Crash child" and crash through CrashHandler::SimulateCrash (child mode; Roadmap M2 "Tests --crash-child writes a
			// crash report and exits 4"). With --child-argument=fatal-error it calls FatalError(FatalErrorKind::DeviceLost,
			// "Simulated device loss") instead, the fatal path that ProcessContext's fatal-error handler reports.
			bool CrashChild = false;
			// --child-process (ChildProcessOption): another Tests process started this one, whatever it runs (a child mode,
			// a ChildTargets case through --no-skip --test-case, a --list-test-cases run). See the file comment for the files
			// such a process writes.
			bool ChildProcess = false;
			// --user-data-dir=<absolute path>: the user-data root for this process (ProcessContextSpecification::
			// UserDataRoot), so a child's logs and crash reports go to its parent's temporary directory. Empty: the OS's.
			std::filesystem::path UserDataDirectory{};
			// --child-argument=<text>: free text for the body a child mode runs, such as the lock file a child must hold.
			std::string ChildArgument{};
			// --test-timeout=<seconds>: the per-test-case limit for cases without a doctest::timeout decorator (> 0).
			double DefaultTimeoutSeconds = 120.0;

			// True for --death-test, --windowed-child and --crash-child.
			[[nodiscard]] bool IsChildMode() const { return !DeathTest.empty() || !WindowedChild.empty() || CrashChild; }

			// True for --child-process and for every child mode (a child mode started by hand is still a child: its output
			// goes to the terminal, not to Tests.log).
			[[nodiscard]] bool IsChildProcess() const { return ChildProcess || IsChildMode(); }
		};

		// Parses the engine options in argv[1..argc) and sets ExecutablePath from GetCurrentExecutablePath(), falling back
		// to argv[0] made absolute only when the OS query fails; arguments that are not engine options (doctest's) are
		// ignored. Errors: InvalidArgument naming the malformed option (an empty --death-test, --windowed-child or
		// --user-data-dir value, a relative --user-data-dir, a value given to --crash-child or --child-process, more than
		// one child mode, a non-positive or non-numeric --test-timeout), Io when neither the OS nor argv[0] yields an
		// absolute path.
		[[nodiscard]] Result<TestOptions> ParseTestOptions(int argc, const char* const* argv);

		// The options of this run: set once by the main before any test runs (process-level state of the Tests binary), then
		// read-only. GetTestOptions before SetTestOptions returns default options.
		void SetTestOptions(TestOptions options);
		[[nodiscard]] const TestOptions& GetTestOptions();

		// The executable of another project of this build, next to the Tests project in the output tree:
		// <bin>/<OutputDir>/<project>/<project>[.exe] beside <bin>/<OutputDir>/Tests/ (Editor, Runtime). The Tests project
		// depends on both, so building Tests builds them. Errors: NotFound naming the expected path, with a hint naming
		// `python Scripts/Build.py --project <project>`.
		[[nodiscard]] Result<std::filesystem::path> GetBuiltExecutablePath(std::string_view project);

		// The specification that starts the running Tests executable as a child through Platform::Process:
		// GetTestOptions().ExecutablePath with `arguments` followed by ChildProcessOption, in the parent's working
		// directory and environment.
		[[nodiscard]] ProcessSpecification MakeTestsChildSpecification(std::vector<std::string> arguments);

	}

}
