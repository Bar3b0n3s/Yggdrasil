#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// The crash handler (Architecture §4.13): process-level state (§3 rule 5) installed by ProcessContext right after the
// log. It catches what ordinary code cannot: on Windows an unhandled SEH exception (SetUnhandledExceptionFilter), the
// C runtime's abort and invalid-parameter paths, and in Debug builds the debug C runtime's assertion and error reports
// (failed parameter checks, MSVC STL checks), which would otherwise open a modal window; on POSIX SIGSEGV, SIGBUS,
// SIGILL, SIGFPE and SIGABRT (sigaction on an alternate signal stack, using only async-signal-safe calls such as write).
// It then writes a crash report, prints one line to stderr, "Crash: <reason>; report written to <path>" (or "...; no
// report written"), and ends the process with exit code 4 (ExitCode::Crash), like every other fatal path. On Windows the
// report is written by a reporter thread that the crash waits for only a bounded time, because the crash may have left
// the heap or loader lock held; when that time runs out the line says "...; incomplete report written to <path>" (or
// "...; no report written") and the process still exits with code 4.
// FatalError (asserts, device loss, ...) does not crash: ProcessContext's fatal-error handler calls WriteFatalErrorReport
// instead. The OS code lives in Platform/Windows/CrashHandlerWindows.cpp and Platform/Posix/CrashHandlerPosix.cpp.
//
// Report: a UTF-8 text file "crash-<seconds since the Unix epoch, UTC>-<pid>.txt" in the report directory
// (<UserData>/<AppName>/Crashes, §4.4), with these sections in order:
//     Crash report
//     Reason: <the signal or exception, or "Fatal error (<FatalErrorKind>): <message>">
//     App: <AppName>
//     Build: <BuildInfo>
//     Process: <pid>
//     Breadcrumbs:          one "  <CrashBreadcrumb>: <value>" line per breadcrumb, "(none)" when unset
//     Stack trace:          one frame per line, symbolized where the platform can
//     Last log lines:       up to LogLineCount entries of the log, oldest first
// On Windows a minidump (MiniDumpWriteDump) with the same name and the extension ".dmp" is written next to it. The log
// lines are copied, as each entry is logged, into a buffer preallocated by Install (a Log listener), because the log's
// own ring buffer is locked and cannot be read safely from a signal handler. Breadcrumbs are copied the same way.

namespace Engine {

	// What the process was doing (§4.13), named in every report.
	enum class CrashBreadcrumb : uint8_t
	{
		Scene,            // the open or playing scene
		PlayState,        // Edit, Play, Simulate or Paused
		AutomationMethod, // the automation method being executed
		ScriptCallback,   // the script callback being run ("Ball.OnFixedUpdate")
		FramePhase        // the frame-loop phase (§4.2)
	};

	inline constexpr size_t CrashBreadcrumbCount = 5;

	struct CrashHandlerSpecification
	{
		// Where reports go; Install creates it. Empty: crashes still end the process with exit code 4 and the stderr
		// line, but no report file is written (Tests children given no user-data root of their own, such as death tests,
		// whose deaths are expected).
		std::filesystem::path ReportDirectory{};
		std::string AppName{};
		// One line describing the build, written into reports, such as "Debug windows-x86_64".
		std::string BuildInfo{};
	};

	// Static access to the process's crash handler.
	class CrashHandler
	{
	public:
		// The number of log entries a report holds, and the bytes kept of each.
		static constexpr size_t LogLineCount = 256;
		static constexpr size_t MaxLogLineLength = 512;
		// The bytes kept of each breadcrumb value; longer values are truncated.
		static constexpr size_t MaxBreadcrumbLength = 256;

		CrashHandler() = delete;

		// Installs the OS handlers, preallocates the buffers, registers the log listener and creates the report directory.
		// Installing twice without Uninstall in between is a programmer error (asserted). Main thread. Errors: Io when the
		// report directory cannot be created or a handler cannot be installed; nothing stays installed on failure.
		[[nodiscard]] static Status Install(const CrashHandlerSpecification& specification);

		// Restores the previous OS handlers and removes the log listener. Idempotent. Main thread.
		static void Uninstall();

		[[nodiscard]] static bool IsInstalled();

		// Records what the process is doing; an empty value clears the breadcrumb. Thread-safe, cheap (a bounded copy, no
		// allocation), callable before Install (the value is kept) and after Uninstall (ignored).
		static void SetBreadcrumb(CrashBreadcrumb breadcrumb, std::string_view value);

		// Writes a report for a fatal error found by ordinary code, from ProcessContext's fatal-error handler: the Reason
		// line is "Fatal error (<kind>): <message>" and the stack trace is the caller's. Returns the report's path. Errors:
		// InvalidState when the handler is not installed or reports are disabled (empty ReportDirectory); Io.
		[[nodiscard]] static Result<std::filesystem::path> WriteFatalErrorReport(FatalErrorKind kind, std::string_view message);

		// Crashes the process the way a real fault does (an access violation raised with RaiseException on Windows,
		// SIGSEGV raised on POSIX), so the installed handler writes its report and exits with code 4. Used by the crash
		// child of the Tests binary (`Tests --crash-child`, Roadmap M2). Without an installed handler the OS ends the
		// process as it does any crash.
		[[noreturn]] static void SimulateCrash();
	};

	// The enumerator name ("Scene", "PlayState", ...).
	[[nodiscard]] std::string_view CrashBreadcrumbToString(CrashBreadcrumb breadcrumb);

}
