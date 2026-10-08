#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <array>
#include <limits>

// The scene-level queries and bounds (Architecture §9.5; §11.5 Physics.Raycast, RaycastAll, SphereCast, OverlapSphere,
// OverlapBox, GetBodyBounds, GetColliderBounds, LayerMask): hits name the collider entity and the body owner, results are
// sorted by distance then UUID. Skipped skeletons of the M11 contract (Docs/Decisions/0014-m11-decisions.md): stream C
// implements the queries and removes the skips.

namespace Engine {

	namespace {

		// A level root on the "Track" layer with three collider-only pieces (one static compound) and a checkpoint trigger
		// above the middle one; a "Default" crate off to the side.
		Scope<PlaySession> StartQueryScene(Test::SceneTestFixture& fixture)
		{
			Scene& scene = fixture.GetScene();
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static, .Layer = "Track" });
			for (int piece = 0; piece < 3; ++piece)
				static_cast<void>(Test::AddBoxBody(scene, "Piece", glm::vec3(static_cast<float>(piece), 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt, level));
			Entity checkpoint = Test::AddBoxBody(scene, "Checkpoint", glm::vec3(1.0f, 2.0f, 0.0f), glm::vec3(0.5f), std::nullopt, level);
			checkpoint.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Static));

			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Project.Physics.Layers = { "Default", "Track" };
			specification.Project.Physics.Collisions = { { "Default", "Default" }, { "Default", "Track" } };
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Test::RunTicks(**session, 1);
			return std::move(*session);
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsSystem: Raycast hits report the collider entity and the body owner" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			const Scene& scene = session->GetScene();
			// Straight down through the gap beside the checkpoint onto the last piece.
			const Result<std::optional<PhysicsRaycastHit>> hit = physics.Raycast(glm::vec3(2.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
			REQUIRE(hit.has_value());
			REQUIRE(hit->has_value());
			CHECK((*hit)->Entity == Test::GetEntityId(scene, "/Level/Piece[2]"));
			CHECK((*hit)->Body == Test::GetEntityId(scene, "/Level"));
			CHECK((*hit)->Distance == doctest::Approx(4.5f).epsilon(1.0e-4));
			CHECK(Test::ApproxEqual((*hit)->Normal, glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-4f));
			// Sensors are hit: the checkpoint above the middle piece comes first (§9.5).
			const Result<std::optional<PhysicsRaycastHit>> sensor = physics.Raycast(glm::vec3(1.0f, 5.0f, 0.0f), glm::vec3(0.0f, -2.0f, 0.0f), 10.0f);
			REQUIRE(sensor.has_value());
			CHECK(sensor->value_or(PhysicsRaycastHit{}).Entity == Test::GetEntityId(scene, "/Level/Checkpoint"));
			CHECK(physics.Raycast(glm::vec3(2.0f, 5.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 10.0f).value_or(std::nullopt) == std::nullopt);
		}

		TEST_CASE("PhysicsSystem: query results are sorted by distance, then by collider UUID" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			const Scene& scene = session->GetScene();
			const Result<std::vector<PhysicsRaycastHit>> along = physics.RaycastAll(glm::vec3(-5.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 20.0f);
			REQUIRE(along.has_value());
			REQUIRE(along->size() == 4);
			for (size_t index = 0; index < 3; ++index)
				CHECK((*along)[index].Entity == Test::GetEntityId(scene, std::format("/Level/Piece[{}]", index)));
			CHECK(along->back().Entity == Test::GetEntityId(scene, "/Crate"));
			CHECK(std::is_sorted(along->begin(), along->end(), [](const PhysicsRaycastHit& a, const PhysicsRaycastHit& b)
			{
				return a.Distance < b.Distance;
			}));
			// Overlaps have no distance: sorted by collider UUID, each collider once.
			const Result<std::vector<PhysicsOverlapHit>> overlaps = physics.OverlapBox(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.2f, 0.25f, 0.25f),
				glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			REQUIRE(overlaps.has_value());
			REQUIRE(overlaps->size() == 3);
			CHECK(std::is_sorted(overlaps->begin(), overlaps->end(), [](const PhysicsOverlapHit& a, const PhysicsOverlapHit& b)
			{
				return a.Entity < b.Entity;
			}));
			const Result<std::vector<PhysicsOverlapHit>> sphere = physics.OverlapSphere(glm::vec3(10.0f, 0.0f, 0.0f), 0.25f);
			REQUIRE(sphere.has_value());
			REQUIRE(sphere->size() == 1);
			CHECK((*sphere)[0].Entity == Test::GetEntityId(scene, "/Crate"));
			CHECK((*sphere)[0].Body == Test::GetEntityId(scene, "/Crate"));
			const Result<std::optional<PhysicsRaycastHit>> cast = physics.SphereCast(glm::vec3(0.0f, 5.0f, 0.0f), 0.25f, glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
			REQUIRE(cast.has_value());
			CHECK(cast->value_or(PhysicsRaycastHit{}).Distance == doctest::Approx(4.25f).epsilon(1.0e-3));
		}

		TEST_CASE("PhysicsSystem: GetBodyBounds and GetColliderBounds return the body's and the collider's world AABBs" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			const Scene& scene = session->GetScene();
			const std::optional<Aabb> body = physics.GetBodyBounds(Test::GetEntityId(scene, "/Level"));
			REQUIRE(body.has_value());
			CHECK(Test::ApproxEqual(body->Min, glm::vec3(-0.5f), 1.0e-4f));
			CHECK(Test::ApproxEqual(body->Max, glm::vec3(2.5f, 0.5f, 0.5f), 1.0e-4f));
			// A collider-only child owns no body, but its collider has bounds inside the ancestor's compound (§9.5).
			const UUID piece = Test::GetEntityId(scene, "/Level/Piece[1]");
			CHECK_FALSE(physics.GetBodyBounds(piece).has_value());
			const std::optional<Aabb> collider = physics.GetColliderBounds(piece);
			REQUIRE(collider.has_value());
			CHECK(Test::ApproxEqual(collider->Min, glm::vec3(0.5f, -0.5f, -0.5f), 1.0e-4f));
			CHECK(Test::ApproxEqual(collider->Max, glm::vec3(1.5f, 0.5f, 0.5f), 1.0e-4f));
			// The level root has no collider of its own.
			CHECK_FALSE(physics.GetColliderBounds(Test::GetEntityId(scene, "/Level")).has_value());
		}

		TEST_CASE("PhysicsSystem: MakeLayerMask selects named layers and queries honour it" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			const std::array<std::string_view, 1> trackOnly = { "Track" };
			const Result<PhysicsLayerMask> track = physics.MakeLayerMask(trackOnly);
			REQUIRE(track.has_value());
			CHECK(*track == 0b10u);
			const Result<std::vector<PhysicsRaycastHit>> hits = physics.RaycastAll(glm::vec3(-5.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 20.0f, *track);
			REQUIRE(hits.has_value());
			CHECK(hits->size() == 3);
			const std::array<std::string_view, 1> typo = { "Trak" };
			CHECK(physics.MakeLayerMask(typo).error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("PhysicsSystem: queries refuse non-finite and degenerate input" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const glm::vec3 down(0.0f, -1.0f, 0.0f);
			CHECK(physics.Raycast(glm::vec3(nan), down, 10.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.Raycast(glm::vec3(0.0f), glm::vec3(0.0f), 10.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.Raycast(glm::vec3(0.0f), down, 0.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.SphereCast(glm::vec3(0.0f), -1.0f, down, 10.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.OverlapSphere(glm::vec3(0.0f), std::numeric_limits<float>::infinity()).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.OverlapBox(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 1.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).error().GetCode()
				== ErrorCode::InvalidArgument);
		}
	}

}
