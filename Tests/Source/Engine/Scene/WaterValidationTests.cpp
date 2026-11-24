#include "TestsPCH.h"

#include "Engine/Scene/WaterValidation.h"

#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/WaterWaves.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/BuoyancyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// The water checks (M47, Docs/Decisions/0065-m47-water.md decision 3): WATER_INVALID for a Water that is not level or a
// second Ocean, WATER_WAVES_TOO_STEEP for waves that would fold, PHYSICS_BUOYANCY_INVALID for a Buoyancy that floats nothing.

namespace Engine {

	namespace {

		Entity AddWater(Scene& scene, std::string_view name, WaterShape shape)
		{
			Entity entity = scene.CreateEntity(name);
			WaterComponent water;
			water.Shape = shape;
			entity.AddComponent<WaterComponent>(water);
			return entity;
		}

		void SetPose(Entity entity, const glm::quat& rotation, const glm::vec3& scale)
		{
			entity.Patch<TransformComponent>([&rotation, &scale](TransformComponent& transform)
			{
				transform.Rotation = rotation;
				transform.Scale = scale;
			});
		}

		// The findings about `entity`, in order.
		std::vector<WaterSceneIssue> IssuesOf(const std::vector<WaterSceneIssue>& issues, ConstEntity entity)
		{
			std::vector<WaterSceneIssue> found;
			std::ranges::copy_if(issues, std::back_inserter(found), [id = entity.GetUUID()](const WaterSceneIssue& issue)
			{
				return issue.Entity == id;
			});
			return found;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("WaterValidation: a Water that is not a yaw and a positive scale, and a second Ocean, are WATER_INVALID")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity sea = AddWater(scene, "Sea", WaterShape::Ocean);
			SetPose(sea, glm::angleAxis(glm::radians(73.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(2.0f, 0.5f, 3.0f));
			const Entity pitched = AddWater(scene, "Pitched", WaterShape::Rectangle);
			SetPose(pitched, glm::angleAxis(glm::radians(20.0f), glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(1.0f));
			const Entity mirrored = AddWater(scene, "Mirrored", WaterShape::Rectangle);
			SetPose(mirrored, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
			// Sheared: a non-uniform scale under a turned child.
			Entity parent = scene.CreateEntity("Stretched");
			SetPose(parent, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(3.0f, 1.0f, 1.0f));
			const Entity sheared = AddWater(scene, "Sheared", WaterShape::Rectangle);
			REQUIRE(scene.SetParent(sheared, parent, std::nullopt, false));
			SetPose(sheared, glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(1.0f));
			const Entity second = AddWater(scene, "Second", WaterShape::Ocean);
			// A pitched third Ocean: both of its causes, one finding each (no field, and Shape).
			const Entity third = AddWater(scene, "Third", WaterShape::Ocean);
			SetPose(third, glm::angleAxis(glm::radians(-15.0f), glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(1.0f));
			// A disabled third Ocean is not reported.
			const Entity off = AddWater(scene, "Off", WaterShape::Ocean);
			off.SetActive(false);

			const std::vector<WaterSceneIssue> issues = FindWaterIssues(scene, nullptr);
			CHECK(IssuesOf(issues, sea).empty());
			for (const Entity entity : { pitched, mirrored, sheared })
			{
				CAPTURE(entity.GetName());
				const std::vector<WaterSceneIssue> found = IssuesOf(issues, entity);
				REQUIRE(found.size() == 1);
				CHECK(found[0].Code == WaterInvalidCode);
				CHECK(found[0].Severity == DiagnosticSeverity::Warning);
				CHECK(found[0].Component == "Water");
				CHECK(found[0].Field.empty());
			}
			const std::vector<WaterSceneIssue> extra = IssuesOf(issues, second);
			REQUIRE(extra.size() == 1);
			CHECK(extra[0].Code == WaterInvalidCode);
			CHECK(extra[0].Field == "Shape");
			CHECK(extra[0].Message.find("'Sea'") != std::string::npos);
			const std::vector<WaterSceneIssue> both = IssuesOf(issues, third);
			REQUIRE(both.size() == 2);
			CHECK(both[0].Code == WaterInvalidCode);
			CHECK(both[0].Field.empty());
			CHECK(both[1].Code == WaterInvalidCode);
			CHECK(both[1].Field == "Shape");
			CHECK(IssuesOf(issues, off).empty());
			CHECK(issues.size() == 6);
		}

		TEST_CASE("WaterValidation: waves that would fold the surface are WATER_WAVES_TOO_STEEP, naming the factor")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity storm = AddWater(scene, "Storm", WaterShape::Ocean);
			// One wave: Steepness 1 x k (2 pi / 10) x Amplitude 1 x WaveScale 2 = 1.2566, past 0.9.
			storm.Patch<WaterComponent>([](WaterComponent& water)
			{
				water.Waves = { WaterWave{ .Wavelength = 10.0f, .Amplitude = 1.0f, .Steepness = 1.0f } };
				water.WaveScale = 2.0f;
			});
			const std::vector<WaterSceneIssue> issues = FindWaterIssues(scene, nullptr);
			REQUIRE(issues.size() == 1);
			CHECK(issues[0].Code == WaterWavesTooSteepCode);
			CHECK(issues[0].Field == "Waves");
			const double factor = MaxWaterSteepnessSum / ComputeWaterSteepnessSum(storm.GetComponent<WaterComponent>().Waves, 2.0);
			CHECK(issues[0].Message.find(std::format("{:.3f}", factor)) != std::string::npos);
			// At WaveScale 1 the sum is 0.628: nothing to report.
			storm.Patch<WaterComponent>([](WaterComponent& water)
			{
				water.WaveScale = 1.0f;
			});
			CHECK(FindWaterIssues(scene, nullptr).empty());
		}

		TEST_CASE("WaterValidation: a Buoyancy on no Dynamic body with a solid collider, or naming no Water, is PHYSICS_BUOYANCY_INVALID")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity pool = AddWater(scene, "Pool", WaterShape::Rectangle);
			const PhysicsLayerTable layers;
			const auto addFloater = [&scene](std::string_view name, float x, BodyType type)
			{
				Entity entity = Test::AddBoxBody(scene, name, glm::vec3(x, 0.0f, 0.0f), glm::vec3(0.5f), type);
				entity.AddComponent<BuoyancyComponent>();
				return entity;
			};
			const Entity boat = addFloater("Boat", 0.0f, BodyType::Dynamic);
			const Entity kinematic = addFloater("Kinematic", 3.0f, BodyType::Kinematic);
			const Entity ghost = addFloater("Ghost", 6.0f, BodyType::Dynamic);
			ghost.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Entity bare = scene.CreateEntity("Bare");
			bare.AddComponent<RigidBodyComponent>();
			bare.AddComponent<BuoyancyComponent>();
			const Entity aimed = addFloater("Aimed", 9.0f, BodyType::Dynamic);
			aimed.Patch<BuoyancyComponent>([&pool](BuoyancyComponent& buoyancy)
			{
				buoyancy.Water = pool.GetUUID();
			});
			const Entity lost = addFloater("Lost", 12.0f, BodyType::Dynamic);
			lost.Patch<BuoyancyComponent>([&boat](BuoyancyComponent& buoyancy)
			{
				buoyancy.Water = boat.GetUUID(); // an entity, but no Water
			});

			const std::vector<WaterSceneIssue> issues = FindWaterIssues(scene, &layers);
			CHECK(IssuesOf(issues, boat).empty());
			CHECK(IssuesOf(issues, aimed).empty());
			const auto expect = [&issues](ConstEntity entity, std::string_view component, std::string_view field)
			{
				CAPTURE(entity.GetName());
				const std::vector<WaterSceneIssue> found = IssuesOf(issues, entity);
				REQUIRE(found.size() == 1);
				CHECK(found[0].Code == PhysicsBuoyancyInvalidCode);
				CHECK(found[0].Severity == DiagnosticSeverity::Warning);
				CHECK(found[0].Component == component);
				CHECK(found[0].Field == field);
			};
			expect(kinematic, "RigidBody", "Type");
			expect(ghost, "Buoyancy", "");
			expect(bare, "Buoyancy", "");
			expect(lost, "Buoyancy", "Water");
			CHECK(issues.size() == 4);
			// Without the project's layers, the bodies are not composed: only the Water reference is checked.
			const std::vector<WaterSceneIssue> unlayered = FindWaterIssues(scene, nullptr);
			REQUIRE(unlayered.size() == 1);
			CHECK(unlayered[0].Entity == lost.GetUUID());
		}
	}

}
