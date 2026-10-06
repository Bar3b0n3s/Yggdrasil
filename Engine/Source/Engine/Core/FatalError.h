#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <string_view>

// Fatal environment failures (Architecture §4.6, "Fatal environment" row) and the terminal step of every failed
// assertion (§4.5). Core owns the function because Core code needs it (the JobSystem worker catch-all, §4.6 item 6;
// the assert failure path); the effects that need higher layers (autosave, crash report, message box) are injected by
// ProcessContext (M2) through SetFatalErrorHandler.

namespace Engine {

	enum class FatalErrorKind : uint8_t
	{
		Assert,             // a failed ENGINE_*_ASSERT / VERIFY / UNREACHABLE (§4.5)
		UnhandledException, // a catch-all boundary caught an exception (JobSystem worker, EntryPoint)
		OutOfMemory,        // host (std::bad_alloc) or GPU memory exhausted
		DeviceLost,         // VK_ERROR_DEVICE_LOST (§8.14)
		GpuHang,            // a bounded GPU wait expired (§8.1)
		Gpu,                // any other unrecoverable GPU error
		InitFailed          // a required environment capability is missing at startup (no Vulkan loader or device)
	};

	// Process exit codes used by FatalError. They are the InitFailed and Crash values of the §4.1 exit-code table, which
	// App/ExitCode.h (M2) defines in full and must keep equal to these.
	inline constexpr int FatalInitFailedExitCode = 3;
	inline constexpr int FatalCrashExitCode = 4;

	// Called by FatalError before the process exits, on the failing thread: flush application state that must survive
	// (editor autosave, which never touches the GPU), write the crash report, show a message box when windowed. It must
	// not throw and must not call FatalError (a nested call skips the handler).
	using FatalErrorHandler = void (*)(FatalErrorKind kind, std::string_view message);

	// Installs the process-wide handler (process-level state, Architecture §3 rule 5) and returns the previous one;
	// nullptr installs none. Thread-safe.
	FatalErrorHandler SetFatalErrorHandler(FatalErrorHandler handler);

	// Terminates the process: logs "Fatal error (<kind>): <message>" at Critical level, runs the installed handler (at
	// most once per process), flushes every log sink and the C stdio streams (std::fflush(nullptr), so buffered stdout,
	// such as a test report redirected to a file or pipe, is not lost) and exits with GetFatalErrorExitCode(kind)
	// without running static destructors (std::_Exit, which flushes nothing itself). Thread-safe: when several threads
	// fail at once, the first one performs the shutdown and the others block until the process exits. Never unwinds.
	[[noreturn]] void FatalError(FatalErrorKind kind, std::string_view message);

	// FatalInitFailedExitCode (3) for InitFailed, FatalCrashExitCode (4) for every other kind.
	[[nodiscard]] int GetFatalErrorExitCode(FatalErrorKind kind);

	// The enumerator name: "Assert", "UnhandledException", ...
	[[nodiscard]] std::string_view FatalErrorKindToString(FatalErrorKind kind);

}
