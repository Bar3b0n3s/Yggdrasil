#include "TestsPCH.h"

#include "Engine/Scene/ColliderDebugDraw.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <array>
#include <set>
#include <tuple>
#include <utility>

// Collider visualization (Architecture §8.10; Roadmap M11 "collider debug draw (edit and play)"): the records, not pixels
// (M8 draws them; Docs/Decisions/0014-m11-decisions.md decision 16). The colour table is complete; the rest are skipped
// skeletons of the M11 contract: stream D implements the visualization and removes the skips.

namespace Engine {

	namespace {

		const ColliderDebugShape* FindShape(const std::vector<ColliderDebugShape>& shapes, UUID collider)
		{
			const auto found = std::find_if(shapes.begin(), shapes.end(), [collider](const ColliderDebugShape& shape)
			{
				return shape.Collider == collider;
			});
			return found == shapes.end() ? nullptr : &*found;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ColliderDebugDraw: every category has its own colour")
		{
			std::set<std::tuple<float, float, float, float>> colours;
			for (const ColliderDebugCategory category : { ColliderDebugCategory::Static, ColliderDebugCategory::Kinematic, ColliderDebugCategory::Dynamic,
					 ColliderDebugCategory::Sleeping, ColliderDebugCategory::Trigger, ColliderDebugCategory::Character, ColliderDebugCategory::Invalid })
			{
				const glm::vec4 colour = GetColliderDebugColor(category);
				CHECK(colour.a == 1.0f);
				colours.insert({ colour.r, colour.g, colour.b, colour.a });
			}
			CHECK(colours.size() == 7);
		}

		TEST_CASE("ColliderDebugDraw: an edit scene draws every collider with its body's category" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity ground = Test::AddGround(scene);
			const Entity crate = Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			const Entity platform = Test::AddBoxBody(scene, "Platform", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.1f, 1.0f), BodyType::Kinematic);
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(-5.0f, 1.0f, 0.0f), glm::vec3(1.0f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			const Entity loose = Test::AddBoxBody(scene, "Loose", glm::vec3(0.0f, 0.0f, 9.0f), glm::vec3(0.5f), std::nullopt);
			Entity player = scene.CreateEntity("Player");
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			Entity locked = Test::AddBoxBody(scene, "Locked", glm::vec3(9.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(locked, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});

			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr);
			CHECK(shapes.size() == 7);
			const std::array<std::pair<Entity, ColliderDebugCategory>, 7> expected = { {
				{ ground, ColliderDebugCategory::Static },
				{ crate, ColliderDebugCategory::Dynamic },
				{ platform, ColliderDebugCategory::Kinematic },
				{ zone, ColliderDebugCategory::Trigger },
				{ loose, ColliderDebugCategory::Static },
				{ player, ColliderDebugCategory::Character },
				{ locked, ColliderDebugCategory::Invalid },
			} };
			for (const auto& [entity, category] : expected)
			{
				INFO("collider ", entity.GetName());
				const ColliderDebugShape* shape = FindShape(shapes, entity.GetUUID());
				REQUIRE(shape != nullptr);
				CHECK(shape->Category == category);
				CHECK(shape->Color == GetColliderDebugColor(category));
			}
			const ColliderDebugShape* capsule = FindShape(shapes, player.GetUUID());
			REQUIRE(capsule != nullptr);
			CHECK(capsule->Type == ColliderDebugShapeType::Capsule);
		}

		TEST_CASE("ColliderDebugDraw: a play session tells sleeping Dynamic bodies apart" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Sleeper", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Faller", glm::vec3(5.0f, 50.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE(session.has_value());
			Test::RunTicks(**session, 120);
			const Scene& runtime = (*session)->GetScene();
			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(runtime, PhysicsLayerTable(), &(*session)->GetPhysics(), nullptr);
			const ColliderDebugShape* sleeper = FindShape(shapes, Test::GetEntityId(runtime, "/Sleeper"));
			const ColliderDebugShape* faller = FindShape(shapes, Test::GetEntityId(runtime, "/Faller"));
			REQUIRE(sleeper != nullptr);
			REQUIRE(faller != nullptr);
			CHECK(sleeper->Category == ColliderDebugCategory::Sleeping);
			CHECK(faller->Category == ColliderDebugCategory::Dynamic);
			// Play poses: the faller has fallen.
			CHECK(faller->Position.y < 45.0f);
		}

		TEST_CASE("ColliderDebugDraw: shapes carry world frames and scaled dimensions" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity parent = scene.CreateEntity("Parent");
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
				transform.Scale = glm::vec3(2.0f);
			});
			parent.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.5f, 0.25f, 0.5f), std::nullopt, parent);
			box.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.Offset = glm::vec3(0.0f, 1.0f, 0.0f);
			});
			Entity capsule = scene.CreateEntity("Capsule");
			capsule.AddComponent<CapsuleColliderComponent>(CapsuleColliderComponent{ .Radius = 0.25f, .HalfHeight = 1.0f });
			capsule.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1.0f, 3.0f, 1.0f);
			});
			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr);
			const ColliderDebugShape* boxShape = FindShape(shapes, box.GetUUID());
			REQUIRE(boxShape != nullptr);
			CHECK(boxShape->Type == ColliderDebugShapeType::Box);
			CHECK(boxShape->Body == parent.GetUUID());
			CHECK(Test::ApproxEqual(boxShape->Position, glm::vec3(12.0f, 2.0f, 0.0f)));
			CHECK(Test::ApproxEqual(boxShape->HalfExtents, glm::vec3(1.0f, 0.5f, 1.0f)));
			// A capsule under a non-uniform scale uses its largest component, as the body's shape does.
			const ColliderDebugShape* capsuleShape = FindShape(shapes, capsule.GetUUID());
			REQUIRE(capsuleShape != nullptr);
			CHECK(capsuleShape->Type == ColliderDebugShapeType::Capsule);
			CHECK(capsuleShape->Radius == doctest::Approx(0.75f));
			CHECK(capsuleShape->HalfHeight == doctest::Approx(3.0f));
		}

		TEST_CASE("ColliderDebugDraw: options leave out triggers and characters" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(0.0f), glm::vec3(1.0f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Entity player = scene.CreateEntity("Player");
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			static_cast<void>(Test::AddGround(scene));
			const std::vector<ColliderDebugShape> all = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr);
			CHECK(all.size() == 3);
			const std::vector<ColliderDebugShape> solidOnly =
				BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr, { .Triggers = false, .Characters = false });
			REQUIRE(solidOnly.size() == 1);
			CHECK(solidOnly[0].Collider == Test::GetEntityId(scene, "/Ground"));
		}
	}

}
