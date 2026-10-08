#include "TestsPCH.h"

#include "Engine/Physics/PhysicsWorld.h"

// The world's queries (Architecture §9.5: raycasts, shape casts and overlaps through the NarrowPhaseQuery, filtered by
// layer mask; hits name the collider through the sub-shape user data, §9.2). Skipped skeletons of the M11 contract
// (Docs/Decisions/0014-m11-decisions.md): stream C implements the queries and removes the skips.

namespace Engine {

	namespace {

		Scope<PhysicsWorld> CreateQueryWorld()
		{
			const std::vector<std::string> layers = { "Default", "Track" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Default" }, { "Default", "Track" } };
			Result<PhysicsLayerTable> table = PhysicsLayerTable::Create(layers, collisions);
			REQUIRE(table.has_value());
			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({ .Layers = *table });
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			return std::move(*world);
		}

		// A static compound of three 1 m boxes along +X at x = 0, 1, 2 (user data 10, 11, 12), on the Track layer.
		BodyHandle CreateTrack(PhysicsWorld& world)
		{
			BodyShapeDescription description;
			for (uint32_t index = 0; index < 3; ++index)
			{
				description.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) },
					.Position = glm::vec3(static_cast<float>(index), 0.0f, 0.0f),
					.UserData = 10 + index });
			}
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(description);
			REQUIRE(shape.has_value());
			Result<BodyHandle> body = world.CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static, .Layer = 1, .UserData = 42 });
			REQUIRE(body.has_value());
			return *body;
		}

		BodyHandle CreateSensor(PhysicsWorld& world, const glm::vec3& position)
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .UserData = 5 } } });
			REQUIRE(shape.has_value());
			Result<BodyHandle> body = world.CreateBody(
				{ .Shape = *shape, .MotionType = PhysicsMotionType::Kinematic, .Pose = { .Position = position }, .IsSensor = true, .CollideKinematicVsNonDynamic = true });
			REQUIRE(body.has_value());
			return *body;
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsWorld: Raycast returns the closest hit and the collider's user data" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateQueryWorld();
			const BodyHandle track = CreateTrack(*world);
			const std::optional<PhysicsQueryHit> hit =
				world->Raycast({ .Origin = glm::vec3(1.0f, 5.0f, 0.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .MaxDistance = 10.0f }, {});
			REQUIRE(hit.has_value());
			CHECK(hit->Body == track);
			CHECK(hit->Collider == 11);
			CHECK(hit->Distance == doctest::Approx(4.5f).epsilon(1.0e-4));
			CHECK(Test::ApproxEqual(hit->Point, glm::vec3(1.0f, 0.5f, 0.0f), 1.0e-4f));
			CHECK(Test::ApproxEqual(hit->Normal, glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-4f));
			CHECK(world->GetUserData(hit->Body) == 42);
			// Too short a ray misses.
			CHECK_FALSE(world->Raycast({ .Origin = glm::vec3(1.0f, 5.0f, 0.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .MaxDistance = 4.0f }, {})
					.has_value());
		}

		TEST_CASE("PhysicsWorld: hits on a compound that places one shared shape several times name each collider" * doctest::skip(true))
		{
			// The mesh shape cache's case (§9.2): the colliders share one leaf, so the collider comes from the compound's
			// sub-shape user data, never from the leaf's own.
			Scope<PhysicsWorld> world = CreateQueryWorld();
			Result<Ref<const PhysicsShape>> piece = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .UserData = 99 } } });
			REQUIRE(piece.has_value());
			BodyShapeDescription description;
			for (uint32_t index = 0; index < 3; ++index)
			{
				description.Colliders.push_back(ColliderShapeDescription{ .Geometry = SharedShapeGeometry{ *piece },
					.Position = glm::vec3(static_cast<float>(index), 0.0f, 0.0f),
					.UserData = 30 + index });
			}
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(description);
			REQUIRE(shape.has_value());
			Result<BodyHandle> body = world->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static });
			REQUIRE(body.has_value());
			for (const uint32_t index : { 0u, 1u, 2u })
			{
				CAPTURE(index);
				const std::optional<PhysicsQueryHit> hit = world->Raycast(
					{ .Origin = glm::vec3(static_cast<float>(index), 5.0f, 0.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .MaxDistance = 10.0f }, {});
				REQUIRE(hit.has_value());
				CHECK(hit->Body == *body);
				CHECK(hit->Collider == 30 + index);
			}
			const std::optional<Aabb> last = world->GetSubShapeBounds(*body, 32);
			REQUIRE(last.has_value());
			CHECK(last->GetCenter().x == doctest::Approx(2.0f).epsilon(1.0e-4));
		}

		TEST_CASE("PhysicsWorld: RaycastAll returns one hit per sub-shape along the ray" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateQueryWorld();
			static_cast<void>(CreateTrack(*world));
			const std::vector<PhysicsQueryHit> hits =
				world->RaycastAll({ .Origin = glm::vec3(-5.0f, 0.0f, 0.0f), .Direction = glm::vec3(1.0f, 0.0f, 0.0f), .MaxDistance = 20.0f }, {});
			std::vector<uint32_t> colliders;
			for (const PhysicsQueryHit& hit : hits)
				colliders.push_back(hit.Collider);
			std::sort(colliders.begin(), colliders.end());
			CHECK(colliders == std::vector<uint32_t>{ 10, 11, 12 });
		}

		TEST_CASE("PhysicsWorld: ShapeCast finds where a swept sphere first touches" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateQueryWorld();
			static_cast<void>(CreateTrack(*world));
			const std::optional<PhysicsQueryHit> hit = world->ShapeCast(SphereShapeGeometry{ .Radius = 0.25f }, { .Position = glm::vec3(0.0f, 3.0f, 0.0f) },
				glm::vec3(0.0f, -1.0f, 0.0f), 10.0f, {});
			REQUIRE(hit.has_value());
			CHECK(hit->Collider == 10);
			// The sphere's bottom reaches the top face at y = 0.5 after 3 - 0.25 - 0.5 = 2.25 m.
			CHECK(hit->Distance == doctest::Approx(2.25f).epsilon(1.0e-3));
		}

		TEST_CASE("PhysicsWorld: Overlap reports every overlapping sub-shape" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateQueryWorld();
			const BodyHandle track = CreateTrack(*world);
			const std::vector<PhysicsOverlap> overlaps =
				world->Overlap(BoxShapeGeometry{ .HalfExtents = glm::vec3(0.6f, 0.2f, 0.2f) }, { .Position = glm::vec3(0.5f, 0.0f, 0.0f) }, {});
			std::vector<uint32_t> colliders;
			for (const PhysicsOverlap& overlap : overlaps)
			{
				CHECK(overlap.Body == track);
				colliders.push_back(overlap.Collider);
			}
			std::sort(colliders.begin(), colliders.end());
			CHECK(colliders == std::vector<uint32_t>{ 10, 11 });
		}

		TEST_CASE("PhysicsWorld: queries honour the layer mask, sensors and the ignored body" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateQueryWorld();
			const BodyHandle track = CreateTrack(*world);
			const BodyHandle sensor = CreateSensor(*world, glm::vec3(0.0f, 3.0f, 0.0f));
			const PhysicsRay down{ .Origin = glm::vec3(0.0f, 10.0f, 0.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .MaxDistance = 20.0f };
			// Sensors are hit by default (§9.5): the sensor above the track comes first.
			CHECK(world->Raycast(down, {}).value_or(PhysicsQueryHit{}).Body == sensor);
			CHECK(world->Raycast(down, { .IncludeSensors = false }).value_or(PhysicsQueryHit{}).Body == track);
			CHECK(world->Raycast(down, { .IgnoreBody = sensor }).value_or(PhysicsQueryHit{}).Body == track);
			// The track is on layer 1, the sensor on layer 0.
			CHECK(world->Raycast(down, { .Layers = 0b10u }).value_or(PhysicsQueryHit{}).Body == track);
			CHECK_FALSE(world->Raycast(down, { .Layers = 0b100u }).has_value());
		}

		TEST_CASE("PhysicsWorld: body and sub-shape bounds are world AABBs" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateQueryWorld();
			const BodyHandle track = CreateTrack(*world);
			const std::optional<Aabb> body = world->GetBodyBounds(track);
			REQUIRE(body.has_value());
			CHECK(Test::ApproxEqual(body->Min, glm::vec3(-0.5f), 1.0e-4f));
			CHECK(Test::ApproxEqual(body->Max, glm::vec3(2.5f, 0.5f, 0.5f), 1.0e-4f));
			const std::optional<Aabb> middle = world->GetSubShapeBounds(track, 11);
			REQUIRE(middle.has_value());
			CHECK(Test::ApproxEqual(middle->Min, glm::vec3(0.5f, -0.5f, -0.5f), 1.0e-4f));
			CHECK(Test::ApproxEqual(middle->Max, glm::vec3(1.5f, 0.5f, 0.5f), 1.0e-4f));
			CHECK_FALSE(world->GetSubShapeBounds(track, 99).has_value());
			CHECK_FALSE(world->GetBodyBounds(BodyHandle()).has_value());
		}
	}

}
