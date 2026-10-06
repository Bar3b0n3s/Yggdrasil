#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/FatalError.h"

#include <string_view>

// Process exit codes (Architecture §4.1). CI, the MCP bridge, the exporter and the Tests binary rely on them, so the
// values never change.

namespace Engine {

	// The exit codes as int constants, because RequestExit and RunApplication deal in plain process exit codes (a
	// script's Application.Quit(code) may pass any value).
	struct ExitCode
	{
		ExitCode() = delete;

		static constexpr int Success = 0;
		static constexpr int Failed = 1;     // tests, validation, --expect-no-errors or a replay expectation failed
		static constexpr int UsageError = 2; // bad command line
		static constexpr int InitFailed = 3; // no Vulkan loader or device, project failed to load or is locked, file missing
		static constexpr int Crash = 4;      // fatal error, unhandled exception, signal, assert in a non-debugger run
		static constexpr int Timeout = 5;    // --timeout or watchdog expiry
	};

	// FatalError (Core) exits with these values itself (ADR 0003 decision 2).
	static_assert(ExitCode::InitFailed == FatalInitFailedExitCode, "App/ExitCode.h and Core/FatalError.h disagree on InitFailed");
	static_assert(ExitCode::Crash == FatalCrashExitCode, "App/ExitCode.h and Core/FatalError.h disagree on Crash");

	// The name of a documented exit code ("Success", "Failed", "UsageError", "InitFailed", "Crash", "Timeout"); empty for
	// any other value.
	[[nodiscard]] constexpr std::string_view ExitCodeToString(int exitCode)
	{
		switch (exitCode)
		{
			case ExitCode::Success:    return "Success";
			case ExitCode::Failed:     return "Failed";
			case ExitCode::UsageError: return "UsageError";
			case ExitCode::InitFailed: return "InitFailed";
			case ExitCode::Crash:      return "Crash";
			case ExitCode::Timeout:    return "Timeout";
			default:                   return {};
		}
	}

}
