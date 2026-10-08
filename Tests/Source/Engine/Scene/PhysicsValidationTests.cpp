#include "TestsPCH.h"

#include "Engine/Scene/PhysicsValidation.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <iterator>
#include <tuple>

// The physics checks of one scene (Architecture §13.7 PHYSICS_* codes; §5.3 "PHYSICS_ADJACENT_STATIC_BODIES ... fires when
// two implicit static bodies have world AABBs within 1 mm of each other"). Skipped skeletons of the M11 contract
// (Docs/Decisions/0014-m11-decisions.md): stream D implements the checks and removes the skips.

namespace Engine {

	namespace {

		std::vector<PhysicsDiagnostic> WithCode(const std::vector<PhysicsDiagnostic>& diagnostics, std::string_view code)
		{
			std::vector<PhysicsDiagnostic> found;
			std::copy_if(diagnostics.begin(), diagnostics.end(), std::back_inserter(found), [code](const PhysicsDiagnostic& diagnostic)
			{
				return diagnostic.Code == code;
			});
			return found;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsValidation: adjacent implicit static bodies are reported with their common parent as the fix target" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity track = scene.CreateEntity("Track");
			// Two collider-only pieces whose faces touch at x = 0.5, under a parent without a RigidBody.
			Entity first = Test::AddBoxBody(scene, "First", glm::vec3(0.0f), glm::vec3(0.5f), std::nullopt, track);
			Entity second = Test::AddBoxBody(scene, "Second", glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt, track);
			const std::vector<PhysicsDiagnostic> diagnostics = ValidateScenePhysics(scene, PhysicsLayerTable(), nullptr);
			const std::vector<PhysicsDiagnostic> adjacent = WithCode(diagnostics, PhysicsAdjacentStaticBodiesCode);
			REQUIRE(adjacent.size() == 1);
			CHECK(adjacent[0].Severity == DiagnosticSeverity::Warning);
			CHECK(adjacent[0].Entity == first.GetUUID());
			CHECK(adjacent[0].Subject == second.GetUUID().ToString());
			CHECK(adjacent[0].AutoFixable);
			CHECK(adjacent[0].FixTarget == track.GetUUID());
			CHECK_FALSE(adjacent[0].Hint.empty());
		}

		TEST_CASE("PhysicsValidation: two adjacent static roots are reported without a fix" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(scene, "Left", glm::vec3(0.0f), glm::vec3(0.5f), std::nullopt));
			static_cast<void>(Test::AddBoxBody(scene, "Right", glm::vec3(1.0005f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt));
			const std::vector<PhysicsDiagnostic> adjacent = WithCode(ValidateScenePhysics(scene, PhysicsLayerTable(), nullptr), PhysicsAdjacentStaticBodiesCode);
			REQUIRE(adjacent.size() == 1);
			CHECK_FALSE(adjacent[0].AutoFixable);
			CHECK_FALSE(adjacent[0].FixTarget.IsValid());
		}

		TEST_CASE("PhysicsValidation: a common parent with trigger colliders of its own is not a fix target" * doctest::skip(true))
		{
			// A Static RigidBody on the parent would gather the solid pieces together with its kill-zone trigger, and the
			// composition would refuse the whole track (PHYSICS_MIXED_TRIGGER).
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity track = Test::AddBoxBody(scene, "Track", glm::vec3(0.0f, -10.0f, 0.0f), glm::vec3(50.0f, 1.0f, 50.0f), std::nullopt);
			track.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddBoxBody(scene, "First", glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.5f), std::nullopt, track));
			static_cast<void>(Test::AddBoxBody(scene, "Second", glm::vec3(1.0f, 10.0f, 0.0f), glm::vec3(0.5f), std::nullopt, track));
			const std::vector<PhysicsDiagnostic> adjacent = WithCode(ValidateScenePhysics(scene, PhysicsLayerTable(), nullptr), PhysicsAdjacentStaticBodiesCode);
			REQUIRE(adjacent.size() == 1);
			CHECK(adjacent[0].FixTarget == track.GetUUID());
			CHECK_FALSE(adjacent[0].AutoFixable);
			CHECK(adjacent[0].Hint.contains("trigger"));
		}

		TEST_CASE("PhysicsValidation: static bodies farther apart than 1 mm, and shared compounds, are not adjacent" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(scene, "Left", glm::vec3(0.0f), glm::vec3(0.5f), std::nullopt));
			static_cast<void>(Test::AddBoxBody(scene, "Right", glm::vec3(1.002f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt));
			// The pattern the warning recommends: one Static RigidBody on the parent, so one compound.
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			static_cast<void>(Test::AddBoxBody(scene, "A", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.5f), std::nullopt, level));
			static_cast<void>(Test::AddBoxBody(scene, "B", glm::vec3(1.0f, 5.0f, 0.0f), glm::vec3(0.5f), std::nullopt, level));
			CHECK(WithCode(ValidateScenePhysics(scene, PhysicsLayerTable(), nullptr), PhysicsAdjacentStaticBodiesCode).empty());
		}

		TEST_CASE("PhysicsValidation: invalid shapes and the composition's codes are reported for an edit scene" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity mesh = scene.CreateEntity("NoMesh");
			mesh.AddComponent<MeshColliderComponent>(MeshColliderComponent{}).Convex = true;
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(5.0f, 0.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
			{
				body.Layer = "Missing";
			});
			const std::vector<PhysicsDiagnostic> diagnostics = ValidateScenePhysics(scene, PhysicsLayerTable(), nullptr);
			const std::vector<PhysicsDiagnostic> shape = WithCode(diagnostics, PhysicsInvalidShapeCode);
			REQUIRE(shape.size() == 1);
			CHECK(shape[0].Entity == mesh.GetUUID());
			CHECK(shape[0].Component == "MeshCollider");
			const std::vector<PhysicsDiagnostic> layer = WithCode(diagnostics, PhysicsUnknownLayerCode);
			REQUIRE(layer.size() == 1);
			CHECK(layer[0].Entity == ball.GetUUID());
			// Sorted by (entity, code, subject).
			CHECK(std::is_sorted(diagnostics.begin(), diagnostics.end(), [](const PhysicsDiagnostic& a, const PhysicsDiagnostic& b)
			{
				return std::tie(a.Entity, a.Code, a.Subject) < std::tie(b.Entity, b.Code, b.Subject);
			}));
		}
	}

}
