#include "EnginePCH.h"
#include "Engine/Platform/ThreadClock.h"

// ThreadClock on Windows: the calling thread's kernel and user times from GetThreadTimes, in 100 ns units.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Assert.h"

	#include <windows.h>

namespace Engine {

	double ThreadClock::GetCpuSeconds()
	{
		FILETIME creation{};
		FILETIME exit{};
		FILETIME kernel{};
		FILETIME user{};
		// The pseudo handle of the calling thread always has THREAD_QUERY_LIMITED_INFORMATION, so this cannot fail.
		const BOOL queried = GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user);
		ENGINE_CORE_VERIFY(queried != FALSE, "GetThreadTimes failed for the calling thread (error {})", GetLastError());
		const auto ticks = [](const FILETIME& time)
		{
			return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | static_cast<uint64_t>(time.dwLowDateTime);
		};
		return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;
	}

}

#endif
