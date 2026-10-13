#pragma once

#include "Engine/Core/Base.h"

// The CPU time of the calling thread: GetThreadTimes on Windows, clock_gettime(CLOCK_THREAD_CPUTIME_ID) on Linux and
// macOS. A safety clock for work bounds that must not depend on how loaded the machine is: it advances only while the
// thread itself runs, never while it waits for I/O or a lock or is descheduled. The import-time script VM measures its
// 250 ms budget with it (Architecture §11.2, ADR 0022 "Integration"). Never simulation time, never on the simulation
// path (§4.12).

namespace Engine {

	class ThreadClock
	{
	public:
		ThreadClock() = delete;

		// Seconds of CPU time (user and kernel) the calling thread has used since it started: finite, nonnegative and
		// nondecreasing on one thread, so it satisfies the Sandbox safety-clock contract for work done on that thread.
		// Resolution is the platform's: Windows accounts thread time once per clock interrupt (about 15.6 ms). Thread-safe.
		[[nodiscard]] static double GetCpuSeconds();
	};

}
