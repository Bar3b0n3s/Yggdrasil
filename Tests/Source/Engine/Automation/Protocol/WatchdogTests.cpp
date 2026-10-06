#include "TestsPCH.h"

#include "Engine/Automation/Protocol/Watchdog.h"

#include <thread>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("Watchdog: a heartbeat older than the threshold reports the stall" * doctest::skip(true))
		{
			// Time is passed in, so the test is exact and reads no clock (CodeStyle §14).
			const std::chrono::steady_clock::time_point start{};
			Watchdog watchdog(std::chrono::milliseconds(5000), start);
			CHECK_FALSE(watchdog.GetStall(start + std::chrono::milliseconds(5000)).has_value());
			CHECK(watchdog.GetStall(start + std::chrono::milliseconds(5001)) == std::chrono::milliseconds(5001));

			watchdog.Heartbeat(start + std::chrono::milliseconds(6000));
			CHECK_FALSE(watchdog.GetStall(start + std::chrono::milliseconds(10000)).has_value());
			CHECK(watchdog.GetStall(start + std::chrono::milliseconds(11500)) == std::chrono::milliseconds(5500));
			CHECK(watchdog.GetStallThreshold() == std::chrono::milliseconds(5000));
		}

		TEST_CASE("Watchdog: the phase marker defaults to Idle and is readable from another thread" * doctest::skip(true))
		{
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::time_point{});
			CHECK(watchdog.GetPhase() == "Idle");
			watchdog.SetPhase("Automation:debug.stall");
			std::string seen;
			std::thread reader([&watchdog, &seen]()
			{
				seen = watchdog.GetPhase();
			});
			reader.join();
			CHECK(seen == "Automation:debug.stall");
			watchdog.SetPhase({});
			CHECK(watchdog.GetPhase() == "Idle");
		}

		TEST_CASE("Watchdog: long phases are cut at a UTF-8 boundary" * doctest::skip(true))
		{
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::time_point{});
			std::string phase(Watchdog::MaxPhaseLength - 1, 'x');
			phase += "\xC3\xA9\xC3\xA9"; // two 2-byte characters across the limit
			watchdog.SetPhase(phase);
			const std::string kept = watchdog.GetPhase();
			CHECK(kept.size() <= Watchdog::MaxPhaseLength);
			CHECK(kept == std::string(Watchdog::MaxPhaseLength - 1, 'x'));
		}

		TEST_CASE("WatchdogPhaseScope: sets a phase and restores the previous one" * doctest::skip(true))
		{
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::time_point{});
			watchdog.SetPhase("Pump");
			{
				const WatchdogPhaseScope scope(&watchdog, "Automation:entity.create");
				CHECK(watchdog.GetPhase() == "Automation:entity.create");
			}
			CHECK(watchdog.GetPhase() == "Pump");
			const WatchdogPhaseScope noWatchdog(nullptr, "ignored");
		}
	}

}
