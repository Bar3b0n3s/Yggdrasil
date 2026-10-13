#include "TestsPCH.h"

#include "Engine/Automation/Protocol/Watchdog.h"

#include <thread>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("Watchdog: a heartbeat older than the threshold reports the stall")
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

		TEST_CASE("Watchdog: the phase marker defaults to Idle and is readable from another thread")
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

		TEST_CASE("Watchdog: long phases are cut at a UTF-8 boundary")
		{
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::time_point{});
			std::string phase(Watchdog::MaxPhaseLength - 1, 'x');
			phase += "\xC3\xA9\xC3\xA9"; // two 2-byte characters across the limit
			watchdog.SetPhase(phase);
			const std::string kept = watchdog.GetPhase();
			CHECK(kept.size() <= Watchdog::MaxPhaseLength);
			CHECK(kept == std::string(Watchdog::MaxPhaseLength - 1, 'x'));
		}

		TEST_CASE("WatchdogPhaseScope: sets a phase and restores the previous one")
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

		TEST_CASE("Watchdog: a script phase is reported over the base phase until it is cleared")
		{
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::time_point{});
			watchdog.SetScriptPhase("Script:Board.OnFixedUpdate");
			CHECK(watchdog.GetPhase() == "Script:Board.OnFixedUpdate");
			{
				// A play.step's pending-operation scope around the callback neither hides nor erases the script phase.
				const WatchdogPhaseScope scope(&watchdog, "Play:step 3/600");
				CHECK(watchdog.GetPhase() == "Script:Board.OnFixedUpdate");
				watchdog.SetScriptPhase({});
				CHECK(watchdog.GetPhase() == "Play:step 3/600");
				watchdog.SetScriptPhase("Script:Board.OnUpdate");
			}
			CHECK(watchdog.GetPhase() == "Script:Board.OnUpdate");
			watchdog.SetScriptPhase({});
			CHECK(watchdog.GetPhase() == "Idle");
		}

		TEST_CASE("FormatScriptPhase: names the script file stem and the callback")
		{
			std::string phase;
			FormatScriptPhase(phase, "Assets/Scripts/Board.luau", "OnFixedUpdate");
			CHECK(phase == "Script:Board.OnFixedUpdate");
			FormatScriptPhase(phase, "Assets/Spin.v2.luau", "Task");
			CHECK(phase == "Script:Spin.v2.Task");
			FormatScriptPhase(phase, {}, "Test.Case");
			CHECK(phase == "Script:Test.Case");
			FormatScriptPhase(phase, "Assets/Scripts/Board.luau", {});
			CHECK(phase == "Script:Board");
		}
	}

}
