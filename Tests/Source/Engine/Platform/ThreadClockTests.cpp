#include "TestsPCH.h"
#include "Engine/Platform/ThreadClock.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <latch>
#include <thread>

namespace Engine {

	namespace {

		// Keeps the calling thread busy until its own CPU clock has advanced by `seconds`.
		void SpinFor(double seconds)
		{
			const double start = ThreadClock::GetCpuSeconds();
			std::atomic<uint64_t> spins{ 0 };
			while (ThreadClock::GetCpuSeconds() - start < seconds)
				spins.fetch_add(1, std::memory_order_relaxed);
		}

	}

	TEST_SUITE("Platform")
	{
		// ADR 0022 "Integration": the import-time script VM measures its budget with this clock, so a thread that waits (for
		// I/O, a lock or the scheduler) does not use up its budget while other threads keep the machine busy.
		TEST_CASE("ThreadClock: a thread's CPU clock advances with its own work and not while it waits")
		{
			const double before = ThreadClock::GetCpuSeconds();
			CHECK(std::isfinite(before));
			CHECK(before >= 0.0);
			SpinFor(0.05);
			CHECK(ThreadClock::GetCpuSeconds() - before >= 0.05);

			std::latch waiting(1);
			std::latch release(1);
			double waited = -1.0;
			std::jthread waiter([&waiting, &release, &waited]()
			{
				const double start = ThreadClock::GetCpuSeconds();
				waiting.count_down();
				release.wait();
				waited = ThreadClock::GetCpuSeconds() - start;
			});
			waiting.wait();
			// A fifth of a second of this thread's CPU time while the waiter is blocked.
			SpinFor(0.2);
			release.count_down();
			waiter.join();
			CHECK(waited >= 0.0);
			// At most the accounting granularity (about 15.6 ms on Windows) and the latch's brief spin.
			CHECK(waited < 0.1);
		}
	}

}
