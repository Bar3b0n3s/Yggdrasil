#include "EnginePCH.h"
#include "Engine/Core/FatalError.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Private/Immortal.h"

#include <cstdio>
#include <cstdlib>

namespace Engine {

	// Process-level state (Architecture §3 rule 5). All three are constant-initialized and the mutex is never destroyed
	// (some standard libraries' std::mutex cannot be locked once destroyed), so FatalError works at any point of the
	// process lifetime, static initialization and destruction included.
	static std::atomic<FatalErrorHandler> s_FatalErrorHandler{ nullptr };
	// Locked by the first thread that fails and never released: every other failing thread blocks on it until that thread
	// ends the process.
	static constinit Utils::Immortal<std::mutex> s_FatalErrorMutex;
	// The number of FatalError calls active on this thread: 1 for the first one, 2 for a call made from inside it (by the
	// handler, or by an assertion while logging), more for a failure while handling that nested call.
	static thread_local int s_FatalErrorDepth = 0;

	FatalErrorHandler SetFatalErrorHandler(FatalErrorHandler handler)
	{
		return s_FatalErrorHandler.exchange(handler);
	}

	void FatalError(FatalErrorKind kind, std::string_view message)
	{
		const int depth = ++s_FatalErrorDepth;
		if (depth == 1)
			s_FatalErrorMutex.Value.lock();

		// Past the first nested level the logging itself is what fails, so only the C streams are flushed.
		if (depth <= 2)
			ENGINE_CORE_CRITICAL("Fatal error ({}): {}", FatalErrorKindToString(kind), message);

		// The handler runs at most once per process: only the first failing thread reaches this point at depth 1, and it
		// never returns. A FatalError called from inside the handler is a nested call and skips it.
		if (depth == 1)
		{
			if (const FatalErrorHandler handler = s_FatalErrorHandler.load())
				handler(kind, message);
		}

		if (depth <= 2)
			Log::Flush();
		// std::_Exit flushes nothing itself; buffered stdout (a test report redirected to a file or pipe) would be lost.
		std::fflush(nullptr);
		std::_Exit(GetFatalErrorExitCode(kind));
	}

	int GetFatalErrorExitCode(FatalErrorKind kind)
	{
		return kind == FatalErrorKind::InitFailed ? FatalInitFailedExitCode : FatalCrashExitCode;
	}

	std::string_view FatalErrorKindToString(FatalErrorKind kind)
	{
		switch (kind)
		{
			case FatalErrorKind::Assert:             return "Assert";
			case FatalErrorKind::UnhandledException: return "UnhandledException";
			case FatalErrorKind::OutOfMemory:        return "OutOfMemory";
			case FatalErrorKind::DeviceLost:         return "DeviceLost";
			case FatalErrorKind::GpuHang:            return "GpuHang";
			case FatalErrorKind::Gpu:                return "Gpu";
			case FatalErrorKind::InitFailed:         return "InitFailed";
		}

		ENGINE_CORE_ASSERT(false, "Unknown FatalErrorKind {}", std::to_underlying(kind));
		return "Unknown";
	}

}
