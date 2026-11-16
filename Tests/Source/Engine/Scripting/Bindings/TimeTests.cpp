#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <vector>

namespace Engine {

	namespace Test {

		void RunTimeBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			fixture.Frame = { .Phase = InputPhase::Step, .Tick = 25, .Frame = 12, .DeltaTime = 0.125, .FixedDeltaTime = 0.125, .TimeScale = 2.0, .InterpolationAlpha = 0.25f };
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(Time.GetDeltaTime() == 0.125 and Time.GetFixedDeltaTime() == 0.125)
assert(Time.GetTick() == 25 and Time.GetFrameCount() == 12 and Time.GetTime() == 3.125)
assert(Time.GetTimeScale() == 2 and Time.GetInterpolationAlpha() == 1)
local real = Time.GetRealTime()
assert(type(real) == "number" and real == real and math.abs(real) < math.huge)
Time.SetTimeScale(100)
assert(Time.GetTimeScale() == 100)
Time.SetTimeScale(0)
return Time.GetTimeScale() == 0
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.Frame.TimeScale == 0.0);
			CHECK(fixture.ExternalMutations.size() == 1);
			fixture.Frame.Phase = InputPhase::Frame;
			fixture.Frame.DeltaTime = 0.0625;
			const auto frame = fixture.Evaluate("return Time.GetDeltaTime() == 0.0625 and Time.GetInterpolationAlpha() == 0.25 and Time.GetTime() == 3.125");
			REQUIRE(frame.has_value());
			CHECK(frame->Value.Get() == Json(true));
			// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decisions 1 and 2): the world pause reaches the host, and the
			// world's clock is the session's less the ticks the host says the pause held.
			fixture.WorldPaused = false;
			fixture.Frame.WorldHeldTicks = 5;
			const auto world = fixture.Evaluate(R"(
assert(not Time.IsWorldPaused())
Time.PauseWorld("Finish")
assert(Time.IsWorldPaused())
Time.ResumeWorld()
assert(not Time.IsWorldPaused())
Time.PauseWorld()
assert(Time.GetTick() == 20 and Time.GetTime() == 2.5)
return Time.IsWorldPaused() and Time.GetSessionTick() == 25 and Time.GetSessionTime() == 3.125
)");
			REQUIRE(world.has_value());
			CHECK(world->Value.Get() == Json(true));
			CHECK(fixture.WorldPaused == std::optional<bool>(true));
			CHECK(fixture.WorldSounds == WorldPauseSounds::Pause);
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Time")
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls > 0);
				}
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("TimeBindings: simulation and phase values come from the host and count every member")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunTimeBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("TimeBindings: invalid scales and read-only writes have no effect")
		{
			for (const bool readOnly : { false, true })
			{
				Test::ScriptTestFixture fixture({ .ReadOnly = readOnly });
				REQUIRE(fixture.Start().has_value());
				const auto result = fixture.Evaluate(R"(
for _, value in {-1, 101, math.huge, -math.huge, 0/0, "2", true} do
	assert(not pcall(Time.SetTimeScale, value))
end
return Time.GetTimeScale() == 1
)");
				REQUIRE(result.has_value());
				CHECK(result->Value.Get() == Json(true));
				if (readOnly)
				{
					const auto refused = fixture.Evaluate("return not pcall(Time.SetTimeScale, 2)");
					REQUIRE(refused.has_value());
					CHECK(refused->Value.Get() == Json(true));
				}
				CHECK(fixture.Frame.TimeScale == 1.0);
				CHECK(fixture.ExternalMutations.empty());
			}
		}
	}

}
