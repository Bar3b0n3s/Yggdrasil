#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <array>
#include <limits>

// The scene-level queries and bounds (Architecture §9.5; §11.5 Physics.Raycast, RaycastAll, SphereCast, OverlapSphere,
// OverlapBox, GetBodyBounds, GetColliderBounds, LayerMask): hits name the collider entity and the body owner (a character's
// inner body names the character), results are sorted by distance then UUID.

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
		TEST_CASE("PhysicsSystem: Raycast hits report the collider entity and the body owner")
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

		TEST_CASE("PhysicsSystem: query results are sorted by distance, then by collider UUID")
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

		TEST_CASE("PhysicsSystem: GetBodyBounds and GetColliderBounds return the body's and the collider's world AABBs")
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

		TEST_CASE("PhysicsSystem: MakeLayerMask selects named layers and queries honour it")
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

		TEST_CASE("PhysicsSystem: queries refuse non-finite and degenerate input")
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

		TEST_CASE("PhysicsSystem: query shapes and points are bounded as bodies are")
		{
			// Query shapes go through the narrow phase as bodies do (§9.1): a radius or half extent below half the smallest
			// collider or above half the largest, and a point beyond MaxPhysicsCoordinate, are refused.
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			const glm::vec3 down(0.0f, -1.0f, 0.0f);
			const glm::vec3 far(0.0f, 2.0f * MaxPhysicsCoordinate, 0.0f);
			const float tiny = PhysicsShape::MinColliderSize / 4.0f;
			const float huge = 2.0f * MaxPhysicsCoordinate;
			const float largest = PhysicsShape::MaxColliderSize / 2.0f;
			const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
			CHECK(physics.Raycast(far, down, 10.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.Raycast(glm::vec3(0.0f), down, huge).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.RaycastAll(glm::vec3(0.0f, -0.9f * MaxPhysicsCoordinate, 0.0f), down, 0.5f * MaxPhysicsCoordinate).error().GetCode()
				== ErrorCode::InvalidArgument);
			CHECK(physics.SphereCast(glm::vec3(0.0f, 5.0f, 0.0f), 1.0e-30f, down, 10.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.SphereCast(glm::vec3(0.0f, 5.0f, 0.0f), tiny, down, 10.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.SphereCast(glm::vec3(0.0f, 5.0f, 0.0f), 3.0e38f, down, 10.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.OverlapSphere(far, 1.0f).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.OverlapSphere(glm::vec3(0.0f), huge).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.OverlapSphere(glm::vec3(0.0f), 1.01f * largest).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.OverlapBox(glm::vec3(0.0f), glm::vec3(1.0f, 1.01f * largest, 1.0f), identity).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.OverlapBox(glm::vec3(1.0e30f), glm::vec3(1.0f), identity).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.OverlapBox(glm::vec3(0.0f), glm::vec3(1.0f, tiny, 1.0f), identity).error().GetCode() == ErrorCode::InvalidArgument);
			// The bounds themselves are accepted, and a query as large as the largest collider finds the whole scene.
			CHECK(physics.SphereCast(glm::vec3(0.0f, 5.0f, 0.0f), PhysicsShape::MinColliderSize / 2.0f, down, 10.0f).has_value());
			CHECK(physics.OverlapBox(glm::vec3(0.0f, MaxPhysicsCoordinate, 0.0f), glm::vec3(largest), identity).has_value());
			CHECK(physics.Raycast(glm::vec3(0.0f, -0.5f * MaxPhysicsCoordinate, 0.0f), down, 0.5f * MaxPhysicsCoordinate).has_value());
			const Result<std::vector<PhysicsOverlapHit>> everything = physics.OverlapSphere(glm::vec3(0.0f), largest);
			REQUIRE(everything.has_value());
			CHECK(everything->size() == 5);
		}

		TEST_CASE("PhysicsSystem: queries and bounds leave out entities pending destruction or disabled")
		{
			// Their bodies stay in the world until FlushDestroyed (§5.7 step 8), but the system treats them as gone at once, as
			// its body functions do: a script that destroys a crate and then raycasts in the same OnFixedUpdate hits nothing.
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			Scene& scene = session->GetScene();
			const UUID crate = Test::GetEntityId(scene, "/Crate");
			const UUID piece = Test::GetEntityId(scene, "/Level/Piece[2]");
			const glm::vec3 down(0.0f, -1.0f, 0.0f);
			REQUIRE(physics.Raycast(glm::vec3(10.0f, 5.0f, 0.0f), down, 10.0f).value_or(std::nullopt).has_value());
			scene.DestroyEntity(scene.FindEntityByID(crate));
			CHECK_FALSE(physics.Raycast(glm::vec3(10.0f, 5.0f, 0.0f), down, 10.0f).value_or(std::nullopt).has_value());
			CHECK(physics.OverlapSphere(glm::vec3(10.0f, 0.0f, 0.0f), 1.0f).value_or(std::vector<PhysicsOverlapHit>{ PhysicsOverlapHit{} }).empty());
			CHECK_FALSE(physics.GetBodyBounds(crate).has_value());
			CHECK_FALSE(physics.GetColliderBounds(crate).has_value());
			CHECK_FALSE(physics.GetBodyInfo(crate).has_value());

			// A disabled piece of the level's compound: the compound is still hit through its other pieces, never this one.
			scene.FindEntityByID(piece).SetActive(false);
			const Result<std::vector<PhysicsRaycastHit>> hits = physics.RaycastAll(glm::vec3(-5.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 20.0f);
			REQUIRE(hits.has_value());
			CHECK(std::none_of(hits->begin(), hits->end(), [piece](const PhysicsRaycastHit& hit)
			{
				return hit.Entity == piece;
			}));
			CHECK_FALSE(hits->empty());
			CHECK_FALSE(physics.GetColliderBounds(piece).has_value());
			CHECK_FALSE(physics.GetBodyInfo(piece).has_value());
		}

		TEST_CASE("PhysicsSystem: equally close hits are ordered by collider UUID")
		{
			// Straight down onto the edge the first two pieces share (from below the checkpoint): both are hit at the same
			// distance, and the lower UUID comes first, whichever piece Jolt found first.
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			const Scene& scene = session->GetScene();
			const Result<std::vector<PhysicsRaycastHit>> hits = physics.RaycastAll(glm::vec3(0.5f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
			REQUIRE(hits.has_value());
			REQUIRE(hits->size() == 2);
			CHECK((*hits)[0].Distance == (*hits)[1].Distance);
			const UUID first = Test::GetEntityId(scene, "/Level/Piece[0]");
			const UUID second = Test::GetEntityId(scene, "/Level/Piece[1]");
			CHECK((*hits)[0].Entity == std::min(first, second));
			CHECK((*hits)[1].Entity == std::max(first, second));
			const Result<std::optional<PhysicsRaycastHit>> closest = physics.Raycast(glm::vec3(0.5f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
			REQUIRE(closest.has_value());
			CHECK(closest->value_or(PhysicsRaycastHit{}).Entity == std::min(first, second));
		}

		TEST_CASE("PhysicsSystem: a trigger collider's bounds are those of its own sensor body")
		{
			Test::SceneTestFixture fixture;
			Scope<PlaySession> session = StartQueryScene(fixture);
			const PhysicsSystem& physics = session->GetPhysics();
			const UUID checkpoint = Test::GetEntityId(session->GetScene(), "/Level/Checkpoint");
			const std::optional<Aabb> collider = physics.GetColliderBounds(checkpoint);
			REQUIRE(collider.has_value());
			CHECK(Test::ApproxEqual(collider->Min, glm::vec3(0.5f, 1.5f, -0.5f), 1.0e-4f));
			CHECK(Test::ApproxEqual(collider->Max, glm::vec3(1.5f, 2.5f, 0.5f), 1.0e-4f));
			// The checkpoint owns that implicit sensor body.
			CHECK(physics.GetBodyBounds(checkpoint) == collider);
			const Result<std::vector<PhysicsOverlapHit>> inside = physics.OverlapSphere(glm::vec3(1.0f, 2.0f, 0.0f), 0.1f);
			REQUIRE(inside.has_value());
			REQUIRE(inside->size() == 1);
			CHECK((*inside)[0].Entity == checkpoint);
			CHECK((*inside)[0].Body == checkpoint);
		}

		TEST_CASE("PhysicsSystem: hits on a character's inner body name the character")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity player = scene.CreateEntity("Player");
			player.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(2.0f, 0.0f, 0.0f);
			});
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			const PhysicsSystem& physics = (*session)->GetPhysics();
			const UUID id = player.GetUUID();

			const Result<std::optional<PhysicsRaycastHit>> hit = physics.Raycast(glm::vec3(-5.0f, 1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 20.0f);
			REQUIRE(hit.has_value());
			REQUIRE(hit->has_value());
			CHECK((*hit)->Entity == id);
			CHECK((*hit)->Body == id);
			CHECK((*hit)->Distance == doctest::Approx(6.7f).epsilon(1.0e-3));
			const Result<std::vector<PhysicsOverlapHit>> overlaps = physics.OverlapBox(glm::vec3(2.0f, 1.0f, 0.0f), glm::vec3(0.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			REQUIRE(overlaps.has_value());
			REQUIRE(overlaps->size() == 1);
			CHECK((*overlaps)[0].Entity == id);
			// Its body's bounds are the capsule's (0.6 m wide, 1.8 m tall, standing on its base); it has no collider.
			const std::optional<Aabb> bounds = physics.GetBodyBounds(id);
			REQUIRE(bounds.has_value());
			CHECK(bounds->GetSize().x == doctest::Approx(0.6f).epsilon(1.0e-3));
			CHECK(bounds->GetSize().y == doctest::Approx(1.8f).epsilon(1.0e-3));
			CHECK(bounds->GetCenter().x == doctest::Approx(2.0f).epsilon(1.0e-3));
			CHECK(bounds->Min.y >= 0.0f);
			CHECK_FALSE(physics.GetColliderBounds(id).has_value());
		}
	}

}
