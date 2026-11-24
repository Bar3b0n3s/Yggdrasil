#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/WaterQueries.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The water's script surface (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §11.5): the Water proxy's
// SampleHeight, SampleNormal, SampleVelocity and IsUnderwater read the receiver's own surface and Physics.SampleWater the
// rule's, at the world's time by default (the world tick, held by the world pause, times the fixed delta) or at a given one;
// reads only, in every run mode and in a read-only evaluation.

namespace Engine {

	namespace {

		// A sea at y = -0.5 with a two-wave swell, and a pool at y = 4 over 6 x 6 m at (20, 4, 0) with a current.
		void AddWaters(Scene& scene, bool ocean)
		{
			if (ocean)
			{
				Entity sea = scene.CreateEntity("Sea");
				sea.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(0.0f, -0.5f, 0.0f);
				});
				WaterComponent water;
				water.Waves = { WaterWave{ .Direction = 15.0f, .Wavelength = 18.0f, .Amplitude = 0.6f, .Steepness = 0.8f },
					WaterWave{ .Direction = 100.0f, .Wavelength = 5.0f, .Amplitude = 0.1f, .Steepness = 0.5f, .Speed = 2.0f, .Phase = 30.0f } };
				sea.AddComponent<WaterComponent>(water);
			}
			Entity pool = scene.CreateEntity("Pool");
			pool.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(20.0f, 4.0f, 0.0f);
			});
			WaterComponent water;
			water.Shape = WaterShape::Rectangle;
			water.Size = glm::vec2(6.0f);
			water.Depth = 2.0f;
			water.Headroom = 3.0f;
			water.Current = glm::vec2(0.5f, 0.0f);
			pool.AddComponent<WaterComponent>(water);
		}

		std::string FormatVector(const glm::dvec3& value)
		{
			return std::format("vector.create({}, {}, {})", value.x, value.y, value.z);
		}

		void CheckWaterScript(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const Result<ScriptEvaluation> result = fixture.Evaluate(source);
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
		}

	}

	namespace Test {

		// The water bindings in `mode` (ScriptApiRegistryTests.cpp's coverage scenario): every method and Physics.SampleWater
		// against the queries' own answers, at the world's time (a session 150 ticks in whose world was paused for 30, so 2 s
		// at 60 Hz) and at a given time.
		void RunWaterBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			Scene& scene = fixture.GetScene();
			AddWaters(scene, true);
			fixture.Frame.Tick = 150;
			fixture.Frame.WorldHeldTicks = 30;
			fixture.Frame.FixedDeltaTime = 1.0 / 60.0;
			REQUIRE(fixture.Start());
			const uint64_t revision = scene.GetRevision();

			const SceneWaters waters = CollectSceneWaters(scene);
			REQUIRE(waters.Ocean.has_value());
			const glm::dvec3 point(3.25, 0.0, -1.5);
			const std::optional<SceneWaterSample> now = SampleWater(*waters.Ocean, point, 2.0);
			const std::optional<SceneWaterSample> early = SampleWater(*waters.Ocean, point, 0.5);
			REQUIRE(now.has_value());
			REQUIRE(early.has_value());
			REQUIRE(std::abs(now->Height - early->Height) > 1.0e-3); // the times are told apart
			const glm::dvec3 below(point.x, now->Height - 0.25, point.z);
			const glm::dvec3 above(point.x, now->Height + 0.25, point.z);
			const glm::dvec3 inPool(21.0, 3.5, 1.0);
			const std::optional<SceneWaterSample> pool = SampleSceneWater(waters, inPool, 2.0);
			REQUIRE(pool.has_value());
			CheckWaterScript(fixture, std::format(R"(
local sea = Scene.FindByName("Sea").Water
local pool = Scene.FindByName("Pool").Water
local function near(a, b) return math.abs(a - b) < 1e-4 end
local function nearVector(a, b) return vector.magnitude(a - b) < 1e-4 end
local point = {}
-- The world's time by default, a given time otherwise.
assert(near(sea:SampleHeight(point), {}), "height now: " .. sea:SampleHeight(point))
assert(near(sea:SampleHeight(point, 0.5), {}))
assert(nearVector(sea:SampleNormal(point), {}))
assert(nearVector(sea:SampleNormal(point, 0.5), {}))
assert(nearVector(sea:SampleVelocity(point), {}))
assert(near(Time.GetTime(), 2))
assert(sea:IsUnderwater({}) and not sea:IsUnderwater({}))
assert(sea:IsUnderwater({}, 2.0))
-- The pool's own surface: nil outside its footprint, its current in its velocity.
assert(pool:SampleHeight(point) == nil and pool:SampleNormal(point) == nil and pool:SampleVelocity(point) == nil)
assert(near(pool:SampleHeight({}), 4))
assert(nearVector(pool:SampleVelocity({}), vector.create(0.5, 0, 0)))
assert(not pool:IsUnderwater(point))
-- The rule: the sea over open water, the pool inside its volume.
local sample = Physics.SampleWater(point)
assert(sample ~= nil and sample.Water == Scene.FindByName("Sea"))
assert(near(sample.Height, {}) and near(sample.Depth, {}))
assert(nearVector(sample.Normal, {}) and nearVector(sample.Velocity, {}))
local later = Physics.SampleWater(point, 0.5)
assert(near(later.Height, {}))
local inPool = Physics.SampleWater({})
assert(inPool.Water == Scene.FindByName("Pool") and near(inPool.Depth, {}))
-- Bad arguments are errors.
assert(not pcall(function() return sea:SampleHeight(vector.create(0 / 0, 0, 0)) end))
assert(not pcall(function() return sea:SampleHeight(point, 0 / 0) end))
assert(not pcall(function() return Physics.SampleWater(point, math.huge) end))
assert(not pcall(function() return Physics.SampleWater("here") end))
return true
)",
										  FormatVector(point), now->Height, early->Height, FormatVector(now->Normal), FormatVector(early->Normal), FormatVector(now->Velocity), FormatVector(below), FormatVector(above), FormatVector(below), FormatVector(inPool), FormatVector(inPool), now->Height, now->Depth, FormatVector(now->Normal), FormatVector(now->Velocity), early->Height, FormatVector(inPool), pool->Depth));
			// Reads only.
			CHECK(scene.GetRevision() == revision);
			CHECK(fixture.ExternalMutations.empty());
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("WaterBindings: Water:SampleHeight and Physics.SampleWater read the surface at the world's time")
		{
			for (const RunModes mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				CAPTURE(static_cast<int>(mode));
				std::vector<ScriptApiCoverage> snapshots;
				Test::RunWaterBindingsCoverage(mode, snapshots);
				REQUIRE(snapshots.size() == 1);
				for (const auto& member : snapshots.front().Members)
				{
					if ((member.Owner == "Water" && member.Kind == ScriptApiMemberKind::Method) || (member.Owner == "Physics" && member.Member == "SampleWater"))
					{
						CAPTURE(member.Owner);
						CAPTURE(member.Member);
						CHECK(member.Calls > 0);
					}
				}
			}
		}

		// Reads only: a read-only evaluation (an edit scene's, script.eval outside play) samples at time 0; with no Ocean,
		// Physics.SampleWater is nil outside the pool.
		TEST_CASE("WaterBindings: a read-only evaluation samples the waters at time 0")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			AddWaters(fixture.GetScene(), false);
			REQUIRE(fixture.Start());
			CheckWaterScript(fixture, R"(
local pool = Scene.FindByName("Pool").Water
assert(pool:SampleHeight(vector.create(20, 0, 0)) == 4)
assert(pool:IsUnderwater(vector.create(20, 3, 0)))
assert(Physics.SampleWater(vector.create(0, 0, 0)) == nil)
assert(Physics.SampleWater(vector.create(20, 6, 0)).Depth == -2)
return true
)");
		}
	}

}
