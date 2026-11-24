#include "TestsPCH.h"

#include "Engine/Scene/WaterQueries.h"

#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string_view>

// Which water holds a point (M47, Docs/Decisions/0065-m47-water.md decision 3): the effectively enabled Rectangle whose
// volume holds it with the highest surface, ties by canonical order, else the scene's one Ocean; a Rectangle's volume masks
// the Ocean, so a cave lake below the sea is dry above its surface; a water's height follows its entity's Translation.

namespace Engine {

	namespace {

		// An entity `name` with a Water of `shape` at `translation`, turned `yawDegrees` about +Y and scaled by `scale`.
		Entity AddWater(Scene& scene, std::string_view name, WaterShape shape, const glm::vec3& translation, float yawDegrees = 0.0f,
			const glm::vec3& scale = glm::vec3(1.0f))
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([&translation, yawDegrees, &scale](TransformComponent& transform)
			{
				transform.Translation = translation;
				transform.Rotation = glm::angleAxis(glm::radians(yawDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
				transform.Scale = scale;
			});
			WaterComponent water;
			water.Shape = shape;
			entity.AddComponent<WaterComponent>(water);
			return entity;
		}

		// Sets the Rectangle's size, depth and headroom.
		void ShapeRectangle(Entity entity, const glm::vec2& size, float depth, float headroom)
		{
			entity.Patch<WaterComponent>([&size, depth, headroom](WaterComponent& water)
			{
				water.Size = size;
				water.Depth = depth;
				water.Headroom = headroom;
			});
		}

		// The id of the water that holds `point` at time 0, or the null UUID.
		UUID WaterAt(const Scene& scene, const glm::dvec3& point)
		{
			const std::optional<SceneWaterSample> sample = SampleSceneWater(scene, point, 0.0);
			return sample.has_value() ? sample->Water : UUID();
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("WaterQueries: the topmost Rectangle holds a point, ties by canonical order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity sea = AddWater(scene, "Sea", WaterShape::Ocean, glm::vec3(0.0f, 0.0f, 0.0f));
			// Two overlapping pools at heights 2 and 3 (volumes from 1 m below to 4 m above), and a third pool level with the
			// second, later in canonical order.
			const Entity low = AddWater(scene, "Low", WaterShape::Rectangle, glm::vec3(0.0f, 2.0f, 0.0f));
			const Entity high = AddWater(scene, "High", WaterShape::Rectangle, glm::vec3(4.0f, 3.0f, 0.0f));
			const Entity level = AddWater(scene, "Level", WaterShape::Rectangle, glm::vec3(8.0f, 3.0f, 0.0f));
			for (const Entity pool : { low, high, level })
				ShapeRectangle(pool, glm::vec2(10.0f), 1.0f, 4.0f);

			// Only the low pool's footprint: the low pool, at a point above its surface too (dry, within its headroom).
			CHECK(WaterAt(scene, glm::dvec3(-3.0, 1.5, 0.0)) == low.GetUUID());
			CHECK(WaterAt(scene, glm::dvec3(-3.0, 5.0, 0.0)) == low.GetUUID());
			// Where the low and high pools overlap, the high one's surface is higher.
			CHECK(WaterAt(scene, glm::dvec3(2.0, 2.5, 0.0)) == high.GetUUID());
			const std::optional<SceneWaterSample> overlap = SampleSceneWater(scene, glm::dvec3(2.0, 2.5, 0.0), 0.0);
			REQUIRE(overlap.has_value());
			CHECK(overlap->Height == 3.0);
			CHECK(overlap->Depth == 0.5);
			// Where the high and level pools overlap at equal heights, the earlier one in canonical order.
			CHECK(WaterAt(scene, glm::dvec3(6.0, 2.5, 0.0)) == high.GetUUID());
			// Under the low pool's floor (more than 1 m below its base) and beyond its headroom: the sea.
			CHECK(WaterAt(scene, glm::dvec3(-3.0, 0.5, 0.0)) == sea.GetUUID());
			CHECK(WaterAt(scene, glm::dvec3(-3.0, 6.5, 0.0)) == sea.GetUUID());
			// Outside every footprint: the sea; a disabled pool holds nothing.
			CHECK(WaterAt(scene, glm::dvec3(40.0, 2.5, 0.0)) == sea.GetUUID());
			high.SetActive(false);
			CHECK(WaterAt(scene, glm::dvec3(2.0, 2.5, 0.0)) == low.GetUUID());
			CHECK(WaterAt(scene, glm::dvec3(6.0, 2.5, 0.0)) == level.GetUUID());
			// No Ocean: nothing outside the pools.
			sea.SetActive(false);
			CHECK_FALSE(SampleSceneWater(scene, glm::dvec3(40.0, 2.5, 0.0), 0.0).has_value());
		}

		TEST_CASE("WaterQueries: a Rectangle's volume masks the Ocean, so a cave below the sea is dry")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity sea = AddWater(scene, "Sea", WaterShape::Ocean, glm::vec3(0.0f));
			// S05's grotto: a lake 30 m below the sea's level in a 20 m chamber, its Headroom up to the chamber's ceiling.
			const Entity lake = AddWater(scene, "Grotto", WaterShape::Rectangle, glm::vec3(100.0f, -30.0f, 50.0f), 35.0f);
			ShapeRectangle(lake, glm::vec2(20.0f, 20.0f), 4.0f, 12.0f);

			// In the cave's air above the lake, 25 m under the sea's level: the lake's, dry (depth negative), not the sea.
			const glm::dvec3 air(100.0, -25.0, 50.0);
			const std::optional<SceneWaterSample> cave = SampleSceneWater(scene, air, 0.0);
			REQUIRE(cave.has_value());
			CHECK(cave->Water == lake.GetUUID());
			CHECK(cave->Depth == doctest::Approx(-5.0));
			const SceneWaters waters = CollectSceneWaters(scene);
			REQUIRE(waters.Ocean.has_value());
			CHECK_FALSE(IsUnderwater(waters.Rectangles.front(), air, 0.0));
			CHECK(IsUnderwater(*waters.Ocean, air, 0.0)); // the sea's own surface, masking aside
			// In the lake: under its surface.
			const std::optional<SceneWaterSample> swim = SampleSceneWater(scene, glm::dvec3(104.0, -31.0, 47.0), 0.0);
			REQUIRE(swim.has_value());
			CHECK(swim->Water == lake.GetUUID());
			CHECK(swim->Depth == doctest::Approx(1.0));
			// Above the ceiling (the rock over the cave) and below the lake's floor: the sea again.
			CHECK(WaterAt(scene, glm::dvec3(100.0, -17.0, 50.0)) == sea.GetUUID());
			CHECK(WaterAt(scene, glm::dvec3(100.0, -35.0, 50.0)) == sea.GetUUID());
			// The footprint turns with the yaw: a point 12 m along the lake's local +X lies beyond its 10 m half extent.
			const glm::quat yaw = glm::angleAxis(glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::dvec3 outside = glm::dvec3(100.0, -25.0, 50.0) + glm::dvec3(yaw * glm::vec3(12.0f, 0.0f, 0.0f));
			const glm::dvec3 inside = glm::dvec3(100.0, -25.0, 50.0) + glm::dvec3(yaw * glm::vec3(9.0f, 0.0f, 9.0f));
			CHECK(WaterAt(scene, outside) == sea.GetUUID());
			CHECK(WaterAt(scene, inside) == lake.GetUUID());
			// A scale on X doubles the footprint along local X: the point 12 m out is now inside.
			lake.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f, 1.0f, 1.0f);
			});
			CHECK(WaterAt(scene, outside) == lake.GetUUID());
			const ResolvedWater resolved = ResolveWater(lake);
			CHECK_FALSE(SampleWater(resolved, glm::dvec3(100.0, -25.0, 80.0), 0.0).has_value());
			CHECK(SampleWater(resolved, glm::dvec3(100.0, -25.0, 55.0), 0.0).has_value());
		}

		TEST_CASE("WaterQueries: a water's height follows its entity's Translation")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity sea = AddWater(scene, "Sea", WaterShape::Ocean, glm::vec3(0.0f, 1.25f, 0.0f));
			sea.Patch<WaterComponent>([](WaterComponent& water)
			{
				water.Waves = { WaterWave{ .Direction = 20.0f, .Wavelength = 30.0f, .Amplitude = 0.4f, .Steepness = 0.6f } };
			});
			const glm::dvec3 point(7.5, 0.0, -3.0);
			const std::optional<SceneWaterSample> before = SampleSceneWater(scene, point, 4.0);
			REQUIRE(before.has_value());
			// The tide: the base rises by 0.75 m, and with it every point of the surface.
			sea.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 2.0f;
			});
			const std::optional<SceneWaterSample> after = SampleSceneWater(scene, point, 4.0);
			REQUIRE(after.has_value());
			CHECK(after->Height - before->Height == doctest::Approx(0.75).epsilon(1e-9));
			CHECK(glm::length(after->Normal - before->Normal) < 1.0e-12);
			// Under a parent, the world height counts: the parent raised by 3 m raises the water by 3 m.
			Entity coast = scene.CreateEntity("Coast");
			REQUIRE(scene.SetParent(sea, coast, std::nullopt, false));
			coast.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 3.0f;
			});
			const std::optional<SceneWaterSample> raised = SampleSceneWater(scene, point, 4.0);
			REQUIRE(raised.has_value());
			CHECK(raised->Height - after->Height == doctest::Approx(3.0).epsilon(1e-9));
			// A calm water is its base exactly.
			sea.Patch<WaterComponent>([](WaterComponent& water)
			{
				water.WaveScale = 0.0f;
			});
			const std::optional<SceneWaterSample> calm = SampleSceneWater(scene, point, 4.0);
			REQUIRE(calm.has_value());
			CHECK(calm->Height == 5.0);
		}

		TEST_CASE("WaterQueries: only the first effectively enabled Ocean is the scene's")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity first = AddWater(scene, "First", WaterShape::Ocean, glm::vec3(0.0f, 1.0f, 0.0f));
			const Entity second = AddWater(scene, "Second", WaterShape::Ocean, glm::vec3(0.0f, 5.0f, 0.0f));
			CHECK(WaterAt(scene, glm::dvec3(0.0)) == first.GetUUID());
			const SceneWaters waters = CollectSceneWaters(scene);
			CHECK(waters.Rectangles.empty());
			REQUIRE(waters.Ocean.has_value());
			CHECK(waters.Ocean->Entity == first.GetUUID());
			first.SetActive(false);
			CHECK(WaterAt(scene, glm::dvec3(0.0)) == second.GetUUID());
		}
	}

}
