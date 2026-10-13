#include "EnginePCH.h"
#include "Engine/Platform/ThreadClock.h"

// ThreadClock on Linux and macOS: clock_gettime(CLOCK_THREAD_CPUTIME_ID), the calling thread's CPU time.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Assert.h"

	#include <time.h>

	#include <cerrno>

namespace Engine {

	double ThreadClock::GetCpuSeconds()
	{
		timespec time{};
		// Every supported Linux and macOS release has a per-thread CPU-time clock, so this cannot fail.
		const int queried = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
		ENGINE_CORE_VERIFY(queried == 0, "clock_gettime(CLOCK_THREAD_CPUTIME_ID) failed (errno {})", errno);
		return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
	}

}

#endif
