#include "TestsPCH.h"

#include "Engine/Physics/Physics2DWorld.h"

#include <cmath>
#include <optional>
#include <variant>
#include <vector>

// The 2D world on Jolt (ADR 0028 decision 6; Docs/Decisions/0032-m21-contract.md decision 8, stream P1; Roadmap M21 "a 2D
// body never leaves its plane"): skipped until stream P1 implements it. The depth rule (GetPhysics2DHalfDepth) keeps contact
// normals in the plane and gives bodies their 2D inertia.

namespace Engine {

	namespace {

		constexpr float FixedDelta2D = 1.0f / 60.0f;

		Scope<Physics2DWorld> Create2DWorld()
		{
			Result<Scope<Physics2DWorld>> world = Physics2DWorld::Create({});
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			return std::move(*world);
		}

		Ref<const PhysicsShape> Create2DShape(const Shape2DGeometry& geometry)
		{
			Result<BodyShapeDescription> laid = MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = geometry } } });
			REQUIRE_MESSAGE(laid.has_value(), laid.error().ToString());
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(*laid);
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			return std::move(*shape);
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("Physics2DWorld: shapes are laid in the XY plane as the existing shape variant" * doctest::skip(true))
		{
			Result<BodyShapeDescription> laid = MakeBody2DShape({ .Colliders = {
																	  Collider2DShapeDescription{ .Geometry = BoxShape2D{ .HalfExtents = glm::vec2(2.0f, 0.5f) }, .Position = glm::vec2(1.0f, 2.0f), .UserData = 4 },
																	  Collider2DShapeDescription{ .Geometry = CircleShape2D{ .Radius = 0.25f }, .Scale = glm::vec2(2.0f) },
																	  Collider2DShapeDescription{ .Geometry = PolygonShape2D{ .Points = { glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 1.0f) } } },
																  } });
			REQUIRE_MESSAGE(laid.has_value(), laid.error().ToString());
			REQUIRE(laid->Colliders.size() == 3);
			// Each box and polygon is extruded by the diagonal of its bounding rectangle, a circle is a sphere.
			const float boxDepth = GetPhysics2DHalfDepth(BoxShape2D{ .HalfExtents = glm::vec2(2.0f, 0.5f) }, glm::vec2(1.0f));
			CHECK(boxDepth == doctest::Approx(2.0f * std::sqrt(4.25f)));
			CHECK(GetPhysics2DHalfDepth(BoxShape2D{ .HalfExtents = glm::vec2(2.0f, 0.5f) }, glm::vec2(0.5f, -2.0f)) == doctest::Approx(2.0f * std::sqrt(2.0f)));
			CHECK(GetPhysics2DHalfDepth(CircleShape2D{ .Radius = 0.25f }, glm::vec2(2.0f)) == 0.5f);
			const BoxShapeGeometry* box = std::get_if<BoxShapeGeometry>(&laid->Colliders[0].Geometry);
			REQUIRE(box != nullptr);
			CHECK(box->HalfExtents == glm::vec3(2.0f, 0.5f, boxDepth));
			CHECK(laid->Colliders[0].Position == glm::vec3(1.0f, 2.0f, 0.0f));
			CHECK(laid->Colliders[0].UserData == 4);
			const SphereShapeGeometry* sphere = std::get_if<SphereShapeGeometry>(&laid->Colliders[1].Geometry);
			REQUIRE(sphere != nullptr);
			CHECK(sphere->Radius == 0.25f);
			CHECK(laid->Colliders[1].Scale == glm::vec3(2.0f));
			const ConvexHullShapeGeometry* hull = std::get_if<ConvexHullShapeGeometry>(&laid->Colliders[2].Geometry);
			REQUIRE(hull != nullptr);
			REQUIRE(hull->Points.size() == 6);
			const float hullDepth = GetPhysics2DHalfDepth(PolygonShape2D{ .Points = { glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.0f, 1.0f) } }, glm::vec2(1.0f));
			CHECK(hullDepth == doctest::Approx(std::sqrt(2.0f)));
			for (const glm::vec3& point : hull->Points)
				CHECK(std::abs(point.z) == hullDepth);

			// A polygon of two points has no area, and a circle cannot take a non-uniform scale.
			CHECK_FALSE(MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = PolygonShape2D{ .Points = { glm::vec2(0.0f), glm::vec2(1.0f) } } } } }).has_value());
			CHECK_FALSE(MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = CircleShape2D{}, .Scale = glm::vec2(1.0f, 2.0f) } } }).has_value());
		}

		TEST_CASE("Physics2DWorld: a dynamic body never leaves its plane" * doctest::skip(true))
		{
			Scope<Physics2DWorld> world = Create2DWorld();
			REQUIRE(world->CreateBody({ .Shape = Create2DShape(BoxShape2D{ .HalfExtents = glm::vec2(20.0f, 0.5f) }), .MotionType = PhysicsMotionType::Static, .Pose = { .Position = glm::vec2(0.0f, -0.5f) } })
					.has_value());
			std::vector<BodyHandle> bodies;
			for (int index = 0; index < 60; ++index)
			{
				const Shape2DGeometry geometry = index % 2 == 0 ? Shape2DGeometry(BoxShape2D{ .HalfExtents = glm::vec2(0.3f, 0.2f) }) : Shape2DGeometry(CircleShape2D{ .Radius = 0.25f });
				Result<BodyHandle> body = world->CreateBody({ .Shape = Create2DShape(geometry),
					.Pose = { .Position = glm::vec2(static_cast<float>(index % 6) * 0.45f - 1.2f, 0.5f + static_cast<float>(index / 6) * 0.6f), .Angle = 0.1f * static_cast<float>(index) },
					.LinearVelocity = glm::vec2(0.5f * static_cast<float>(index % 3) - 0.5f, 0.0f),
					.AngularVelocity = 2.0f - 0.07f * static_cast<float>(index) });
				REQUIRE(body.has_value());
				bodies.push_back(*body);
			}
			for (int step = 0; step < 600; ++step)
				REQUIRE(world->Step(FixedDelta2D, 1).has_value());
			REQUIRE(world->GetWorld() != nullptr);
			for (const BodyHandle body : bodies)
			{
				// Exactly on the plane, rotated about Z only: the extruded shapes put no motion out of it.
				const PhysicsPose pose = world->GetWorld()->GetPose(body);
				CHECK(pose.Position.z == 0.0f);
				CHECK(pose.Rotation.x == 0.0f);
				CHECK(pose.Rotation.y == 0.0f);
				CHECK(world->GetWorld()->GetLinearVelocity(body).z == 0.0f);
				CHECK(world->GetWorld()->GetAngularVelocity(body).x == 0.0f);
				CHECK(world->GetWorld()->GetAngularVelocity(body).y == 0.0f);
			}
		}

		TEST_CASE("Physics2DWorld: a ray in the plane hits an extruded box with an in-plane normal" * doctest::skip(true))
		{
			Scope<Physics2DWorld> world = Create2DWorld();
			Result<BodyHandle> wall = world->CreateBody({ .Shape = Create2DShape(BoxShape2D{ .HalfExtents = glm::vec2(0.5f, 2.0f) }), .MotionType = PhysicsMotionType::Static, .Pose = { .Position = glm::vec2(3.0f, 0.0f) } });
			REQUIRE(wall.has_value());
			const std::optional<PhysicsQueryHit> hit = world->Raycast(glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 0.0f), 10.0f, {});
			REQUIRE(hit.has_value());
			CHECK(hit->Body == *wall);
			CHECK(hit->Distance == doctest::Approx(2.5f));
			CHECK(hit->Point.z == 0.0f);
			CHECK(hit->Normal.x == doctest::Approx(-1.0f));
			CHECK(hit->Normal.z == 0.0f);
		}

		TEST_CASE("Physics2DWorld: a body sunk deep into a box is pushed out in the plane" * doctest::skip(true))
		{
			// A crate whose centre lies 1.6 m inside a thick floor (2.1 m from its way out, upward) and a ball whose centre lies
			// 0.8 m inside it: the overlap along Z must exceed both, or the contact normal would lie along the locked Z and push
			// nothing, so the crate would sink through the floor under gravity instead of rising onto it.
			Scope<Physics2DWorld> world = Create2DWorld();
			REQUIRE(world->CreateBody({ .Shape = Create2DShape(BoxShape2D{ .HalfExtents = glm::vec2(10.0f, 2.0f) }), .MotionType = PhysicsMotionType::Static, .Pose = { .Position = glm::vec2(0.0f, -2.0f) } })
					.has_value());
			const Result<BodyHandle> crate = world->CreateBody({ .Shape = Create2DShape(BoxShape2D{}), .Pose = { .Position = glm::vec2(0.0f, -1.6f) } });
			const Result<BodyHandle> ball = world->CreateBody({ .Shape = Create2DShape(CircleShape2D{ .Radius = 0.25f }), .Pose = { .Position = glm::vec2(3.0f, -0.8f) } });
			REQUIRE(crate.has_value());
			REQUIRE(ball.has_value());
			for (int step = 0; step < 180; ++step)
				REQUIRE(world->Step(FixedDelta2D, 1).has_value());
			// Resting on the floor's top face (y = 0), each on its own half height.
			const Physics2DPose cratePose = world->GetPose(*crate);
			CHECK(cratePose.Position.y > 0.4f);
			CHECK(cratePose.Position.y < 0.6f);
			CHECK(std::abs(cratePose.Position.x) < 0.1f);
			const Physics2DPose ballPose = world->GetPose(*ball);
			CHECK(ballPose.Position.y > 0.15f);
			CHECK(ballPose.Position.y < 0.35f);
		}

		TEST_CASE("Physics2DWorld: a small body's angular acceleration under a torque follows its 2D inertia" * doctest::skip(true))
		{
			// A 4 cm box's inertia about Z is m (w^2 + h^2) / 12, whatever its depth, with no isotropic floor added for a size
			// the depth would make ill-conditioned; a circle's is its sphere's, 0.4 m r^2.
			Scope<Physics2DWorld> world = Create2DWorld();
			REQUIRE(world->SetGravity(glm::vec2(0.0f)).has_value());
			const Result<BodyHandle> box = world->CreateBody({ .Shape = Create2DShape(BoxShape2D{ .HalfExtents = glm::vec2(0.02f) }), .Mass = 2.0f, .AngularDamping = 0.0f });
			const Result<BodyHandle> circle = world->CreateBody({ .Shape = Create2DShape(CircleShape2D{ .Radius = 0.5f }), .Pose = { .Position = glm::vec2(5.0f, 0.0f) }, .AngularDamping = 0.0f });
			REQUIRE(box.has_value());
			REQUIRE(circle.has_value());
			constexpr float BoxTorque = 1.0e-4f;
			constexpr float CircleTorque = 0.01f;
			world->AddTorque(*box, BoxTorque);
			world->AddTorque(*circle, CircleTorque);
			REQUIRE(world->Step(FixedDelta2D, 1).has_value());
			const float boxInertia = 2.0f * (0.04f * 0.04f + 0.04f * 0.04f) / 12.0f;
			CHECK(world->GetAngularVelocity(*box) == doctest::Approx(BoxTorque / boxInertia * FixedDelta2D).epsilon(0.01));
			const float circleInertia = 0.4f * 1.0f * 0.5f * 0.5f;
			CHECK(world->GetAngularVelocity(*circle) == doctest::Approx(CircleTorque / circleInertia * FixedDelta2D).epsilon(0.01));
		}
	}

}
