#pragma once

#include <chrono>
#include <thread>

// Waiting in tests for what another thread or process does (an I/O thread closing a socket, a server queueing a request):
// polls a condition until it holds, yielding between polls.
//     CHECK(Test::WaitUntil([&server]() { return server.GetClients().empty(); }));
// The bound is only a failure bound: a condition that never holds fails the check after it instead of hanging until the
// per-test timeout. No passing test depends on it, so the outcome never depends on timing (Docs/Decisions/
// 0008-m4-decisions.md decision 15). A spin count would make the wait depend on CPU speed and scheduler load instead.

namespace Engine {

	namespace Test {

		// Generous for a loaded 2-vCPU CI runner, and below the default per-test timeout.
		inline constexpr std::chrono::seconds DefaultWaitBound{ 30 };

		// True once `condition()` returns true; false when it still does not after `bound`. `condition` runs at least once.
		template<typename Condition>
		[[nodiscard]] bool WaitUntil(Condition&& condition, std::chrono::steady_clock::duration bound = DefaultWaitBound)
		{
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + bound;
			while (!condition())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				std::this_thread::yield();
			}
			return true;
		}

	}

}
