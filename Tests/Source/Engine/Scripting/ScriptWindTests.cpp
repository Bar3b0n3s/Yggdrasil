#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/WindComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/Wind.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/ScriptTestFixture.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

// The wind's script surface (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3J): Wind:Sample
// reads SampleWind (Scene/Wind.h) at a world point, at the given time or the world's clock.

namespace Engine {

	namespace {

		WindComponent MakeBreeze()
		{
			WindComponent wind;
			wind.Direction = glm::vec3(0.0f, 0.0f, -2.0f);
			wind.Strength = 7.0f;
			wind.GustStrength = 2.5f;
			wind.GustFrequency = 0.5f;
			wind.Turbulence = 0.4f;
			return wind;
		}

		std::string FormatVector(const glm::dvec3& value)
		{
			const glm::vec3 single(value);
			return std::format("vector.create({}, {}, {})", single.x, single.y, single.z);
		}

		void CheckWindScript(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const Result<ScriptEvaluation> result = fixture.Evaluate(source);
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
		}

		// The coverage scenario in `mode`, whose API coverage it adds to `snapshots` ("ScriptApi: every registered function,
		// method, property, operator and constructor has a test").
		void WindCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			Scene& scene = fixture.GetScene();
			Entity entity = scene.CreateEntity("Weather");
			const WindComponent breeze = MakeBreeze();
			entity.AddComponent<WindComponent>(breeze);
			REQUIRE(fixture.Start());
			const uint64_t revision = scene.GetRevision();

			// At an explicit time, the vector of SampleWind (rounded to the script's single-precision vector); without one, at
			// the world's clock, which is 0 before the first tick.
			const glm::dvec3 point(4.0, 1.0, -25.0);
			const glm::dvec3 atTime = SampleWind(breeze, point, 13.75);
			const glm::dvec3 atStart = SampleWind(breeze, point, 0.0);
			CheckWindScript(fixture, std::format(R"(
local wind = Scene.FindByName("Weather").Wind
local function nearVector(a, b) return vector.magnitude(a - b) < 1e-5 end
assert(nearVector(wind:Sample({}, 13.75), {}))
assert(nearVector(wind:Sample({}), {}))
assert(wind:Sample({}, nil) == wind:Sample({}))
assert(not pcall(function() return wind:Sample(5) end))
return true
)",
										 FormatVector(point), FormatVector(atTime), FormatVector(point), FormatVector(atStart), FormatVector(point), FormatVector(point)));
			// Reads only.
			CHECK(scene.GetRevision() == revision);
			CHECK(fixture.ExternalMutations.empty());
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	namespace Test {

		void RunWindBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			WindCoverage(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptWind: Sample reads the wind at a world point, at its time or the world's clock")
		{
			std::vector<ScriptApiCoverage> snapshots;
			WindCoverage(RunModes::Editor, snapshots);
		}

		// Reads only: a read-only evaluation (an edit scene's, script.eval outside play) samples the wind too.
		TEST_CASE("ScriptWind: a read-only evaluation samples the wind")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			Entity entity = fixture.GetScene().CreateEntity("Weather");
			entity.AddComponent<WindComponent>(MakeBreeze());
			REQUIRE(fixture.Start());
			const glm::dvec3 expected = SampleWind(MakeBreeze(), glm::dvec3(0.0), 2.0);
			CheckWindScript(fixture, std::format(R"(
local wind = Scene.FindByName("Weather").Wind
assert(vector.magnitude(wind:Sample(vector.zero, 2) - {}) < 1e-5)
return true
)",
										 FormatVector(expected)));
		}

		// W46.1's review: without a time, Sample reads the world's clock (Time.GetTime: the world's tick, the session's less
		// the ticks a world pause held it, times the fixed delta), so neither 0 nor the session's time, and a world pause
		// holds it across ticks.
		TEST_CASE("ScriptWind: Sample without a time reads the world's clock, which a world pause holds")
		{
			Test::ScriptTestFixture fixture;
			Entity entity = fixture.GetScene().CreateEntity("Weather");
			const WindComponent breeze = MakeBreeze();
			entity.AddComponent<WindComponent>(breeze);
			REQUIRE(fixture.Start());
			const glm::dvec3 point(4.0, 1.0, -25.0);
			// The sample at the world's clock after `tick` session ticks of which a world pause held `heldTicks`, checked against
			// `expected` (rounded to the script's single-precision vector) and against Sample at Time.GetTime.
			const auto checkSample = [&fixture, &point](uint64_t tick, uint64_t heldTicks, const glm::dvec3& expected)
			{
				fixture.Frame.Tick = tick;
				fixture.Frame.WorldHeldTicks = heldTicks;
				fixture.Frame.FixedDeltaTime = 1.0 / 60.0;
				CheckWindScript(fixture, std::format(R"(
local wind = Scene.FindByName("Weather").Wind
local sample = wind:Sample({})
assert(vector.magnitude(sample - {}) < 1e-5)
assert(vector.magnitude(sample - wind:Sample({}, Time.GetTime())) < 1e-6)
return true
)",
											 FormatVector(point), FormatVector(expected), FormatVector(point)));
			};

			// 600 session ticks of which a world pause held 135: the world's clock reads 465 / 60 s, not 0 and not 600 / 60 s
			// (each of which gives another velocity here).
			const glm::dvec3 world = SampleWind(breeze, point, 465.0 / 60.0);
			REQUIRE(glm::length(world - SampleWind(breeze, point, 0.0)) > 1e-3);
			REQUIRE(glm::length(world - SampleWind(breeze, point, 600.0 / 60.0)) > 1e-3);
			checkSample(600, 135, world);
			// 30 more ticks, all held by the pause: the sample holds.
			checkSample(630, 165, world);
			// 30 more after the world resumes: the clock moves on from where it held.
			const glm::dvec3 resumed = SampleWind(breeze, point, 495.0 / 60.0);
			REQUIRE(glm::length(resumed - world) > 1e-3);
			checkSample(660, 165, resumed);
			CHECK(fixture.Errors.empty());
		}
	}

}
