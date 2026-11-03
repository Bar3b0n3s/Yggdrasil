#include "TestsPCH.h"

#include "Engine/Physics/Physics2DWorld.h"

#include "Engine/Core/Hash.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Support/DeathTest.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <tuple>
#include <variant>
#include <vector>

// The 2D world on Jolt (ADR 0028 decision 6; Docs/Decisions/0032-m21-contract.md decision 8, stream P1; Roadmap M21 "a 2D
// body never leaves its plane"): shapes laid in the XY plane, poses, motion and queries in the plane, contact records
// projected onto it, and the step's determinism across Jolt thread counts. The depth rule (GetPhysics2DHalfDepth) keeps
// contact normals in the plane and gives bodies their 2D inertia. ECS-agnostic: bodies are created directly.

namespace Engine {

	namespace {

		constexpr float FixedDelta2D = 1.0f / 60.0f;

		Scope<Physics2DWorld> Create2DWorld()
		{
			Result<Scope<Physics2DWorld>> world = Physics2DWorld::Create({});
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			return std::move(*world);
		}

		Ref<const PhysicsShape> Create2DShape(const std::vector<Collider2DShapeDescription>& colliders)
		{
			Result<BodyShapeDescription> laid = MakeBody2DShape({ .Colliders = colliders });
			REQUIRE_MESSAGE(laid.has_value(), laid.error().ToString());
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(*laid);
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			return std::move(*shape);
		}

		Ref<const PhysicsShape> Create2DShape(const Shape2DGeometry& geometry)
		{
			return Create2DShape(std::vector<Collider2DShapeDescription>{ Collider2DShapeDescription{ .Geometry = geometry } });
		}

		// A convex pentagon about 0.6 m wide, its centroid near the origin.
		PolygonShape2D MakePentagon()
		{
			return PolygonShape2D{ .Points = { glm::vec2(-0.25f, -0.2f), glm::vec2(0.25f, -0.2f), glm::vec2(0.3f, 0.1f), glm::vec2(0.0f, 0.25f), glm::vec2(-0.3f, 0.1f) } };
		}

		// A compound of a box and a circle beside it, the circle 0.35 m along +X.
		std::vector<Collider2DShapeDescription> MakeBoxAndCircle()
		{
			return { Collider2DShapeDescription{ .Geometry = BoxShape2D{ .HalfExtents = glm::vec2(0.2f) }, .UserData = 1 },
				Collider2DShapeDescription{ .Geometry = CircleShape2D{ .Radius = 0.15f }, .Position = glm::vec2(0.35f, 0.0f), .UserData = 2 } };
		}

		BodyHandle CreateStatic2DBox(Physics2DWorld& world, const glm::vec2& position, const glm::vec2& halfExtents, float angle = 0.0f)
		{
			Result<BodyHandle> body = world.CreateBody({ .Shape = Create2DShape(BoxShape2D{ .HalfExtents = halfExtents }),
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = position, .Angle = angle } });
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			return *body;
		}

		// Whether `world` holds `body` exactly on the plane z = 0, rotated about Z only, with no velocity out of the plane.
		void CheckOnPlane(const Physics2DWorld& world, BodyHandle body)
		{
			REQUIRE(world.GetWorld() != nullptr);
			const PhysicsPose pose = world.GetWorld()->GetPose(body);
			CHECK(pose.Position.z == 0.0f);
			CHECK(pose.Rotation.x == 0.0f);
			CHECK(pose.Rotation.y == 0.0f);
			CHECK(world.GetWorld()->GetLinearVelocity(body).z == 0.0f);
			CHECK(world.GetWorld()->GetAngularVelocity(body).x == 0.0f);
			CHECK(world.GetWorld()->GetAngularVelocity(body).y == 0.0f);
		}

		// The pile of the thread-count test: 96 boxes, circles and pentagons in 12 columns between two walls, each row offset
		// and turned a little so the pile topples and keeps many contacts busy; built in creation order.
		constexpr uint32_t PileSteps2D = 240;

		std::vector<BodyHandle> Build2DPile(Physics2DWorld& world)
		{
			static_cast<void>(CreateStatic2DBox(world, glm::vec2(0.0f, -0.5f), glm::vec2(8.0f, 0.5f)));
			static_cast<void>(CreateStatic2DBox(world, glm::vec2(-8.5f, 5.0f), glm::vec2(0.5f, 6.0f)));
			static_cast<void>(CreateStatic2DBox(world, glm::vec2(8.5f, 5.0f), glm::vec2(0.5f, 6.0f)));
			std::vector<BodyHandle> bodies;
			for (int index = 0; index < 96; ++index)
			{
				const int row = index / 12;
				const int column = index % 12;
				Shape2DGeometry geometry = MakePentagon();
				if (index % 3 == 0)
					geometry = BoxShape2D{ .HalfExtents = glm::vec2(0.25f + 0.0625f * static_cast<float>(row % 3), 0.25f) };
				else if (index % 3 == 1)
					geometry = CircleShape2D{ .Radius = 0.25f + 0.0625f * static_cast<float>(column % 2) };
				Result<BodyHandle> body = world.CreateBody({ .Shape = Create2DShape(geometry),
					.Pose = { .Position = glm::vec2(-6.0f + static_cast<float>(column) + 0.125f * static_cast<float>(row % 2), 0.75f + 0.875f * static_cast<float>(row)),
						.Angle = 0.25f * static_cast<float>(index % 5) },
					.LinearVelocity = glm::vec2(0.25f * static_cast<float>(index % 3) - 0.25f, 0.0f),
					.UserData = static_cast<uint64_t>(index) });
				REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
				bodies.push_back(*body);
			}
			return bodies;
		}

		// XXH64 of each body's planar pose and velocities (float bit patterns) and its sleeping flag, in the order given.
		uint64_t Hash2DBodies(const Physics2DWorld& world, const std::vector<BodyHandle>& bodies)
		{
			XXH64Hasher hasher(0);
			for (const BodyHandle body : bodies)
			{
				const Physics2DPose pose = world.GetPose(body);
				const glm::vec2 velocity = world.GetLinearVelocity(body);
				for (const float value : { pose.Position.x, pose.Position.y, pose.Angle, velocity.x, velocity.y, world.GetAngularVelocity(body) })
					hasher.UpdateU64(std::bit_cast<uint32_t>(value));
				hasher.UpdateU64(world.IsSleeping(body) ? 1 : 0);
			}
			return hasher.Digest();
		}

		// The pile's hash after PileSteps2D steps with `workerThreads` Jolt threads; `contacts` receives every contact record,
		// each step's sorted (Jolt reports a step's records in an order that depends on its threads, but at most one per pair,
		// sub-shapes and kind). It REQUIREs, so callers keep it out of CHECK expressions.
		uint64_t Run2DPile(uint32_t workerThreads, std::vector<ContactEvent>& contacts)
		{
			const Test::ScopedWorkerThreadCount threads(workerThreads);
			Scope<Physics2DWorld> world = Create2DWorld();
			const std::vector<BodyHandle> bodies = Build2DPile(*world);
			for (uint32_t step = 0; step < PileSteps2D; ++step)
			{
				REQUIRE(world->Step(FixedDelta2D, 1).has_value());
				std::vector<ContactEvent> stepContacts = world->DrainContactEvents();
				std::sort(stepContacts.begin(), stepContacts.end(), [](const ContactEvent& a, const ContactEvent& b)
				{
					return std::tie(a.BodyA, a.BodyB, a.SubShapeA, a.SubShapeB, a.Kind) < std::tie(b.BodyA, b.BodyB, b.SubShapeA, b.SubShapeB, b.Kind);
				});
				contacts.insert(contacts.end(), stepContacts.begin(), stepContacts.end());
			}
			for (const BodyHandle body : bodies)
				CheckOnPlane(*world, body);
			// Every contact record of the pile lies in the plane, whatever the pair of shapes.
			const auto pointsOffPlane = std::count_if(contacts.begin(), contacts.end(), [](const ContactEvent& event)
			{
				return event.Point.z != 0.0f;
			});
			const auto normalsOffPlane = std::count_if(contacts.begin(), contacts.end(), [](const ContactEvent& event)
			{
				return event.Normal.z != 0.0f;
			});
			CHECK(pointsOffPlane == 0);
			CHECK(normalsOffPlane == 0);
			return Hash2DBodies(*world, bodies);
		}

		// The first Added record between `a` and `b`, in either order, or nullopt.
		std::optional<ContactEvent> FindAdded(const std::vector<ContactEvent>& contacts, BodyHandle a, BodyHandle b)
		{
			const auto found = std::find_if(contacts.begin(), contacts.end(), [a, b](const ContactEvent& event)
			{
				return event.Kind == ContactEventKind::Added && ((event.BodyA == a && event.BodyB == b) || (event.BodyA == b && event.BodyB == a));
			});
			if (found == contacts.end())
				return std::nullopt;
			return *found;
		}

		// Whether `laid` is refused with PHYSICS_INVALID_SHAPE.
		bool IsInvalidShape(const Result<BodyShapeDescription>& laid)
		{
			return !laid.has_value() && laid.error().GetCode() == ErrorCode::Validation && GetPhysicsDiagnosticCode(laid.error()) == PhysicsInvalidShapeCode;
		}

		bool IsInvalidCollider(const Collider2DShapeDescription& collider)
		{
			return IsInvalidShape(MakeBody2DShape({ .Colliders = { collider } }));
		}

	}

	ENGINE_DEATH_TEST("Physics/World2DSetsNonFinitePose")
	{
		Result<Scope<Physics2DWorld>> world = Physics2DWorld::Create({});
		Result<BodyShapeDescription> laid = MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{} } });
		if (!world.has_value() || !laid.has_value())
			return;
		Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(*laid);
		if (!shape.has_value())
			return;
		Result<BodyHandle> body = (*world)->CreateBody({ .Shape = *shape });
		if (body.has_value())
			(*world)->SetPose(*body, { .Angle = std::numeric_limits<float>::quiet_NaN() }, true);
	}

	ENGINE_DEATH_TEST("Physics/World2DOverlapsEmptyCircle")
	{
		Result<Scope<Physics2DWorld>> world = Physics2DWorld::Create({});
		if (world.has_value())
			static_cast<void>((*world)->Overlap(CircleShape2D{ .Radius = 0.0f }, {}, {}));
	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("Physics2DWorld: shapes are laid in the XY plane as the existing shape variant")
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

			// The scale acts in the plane only (the depth is computed after it); an angle is a rotation about +Z.
			CHECK(laid->Colliders[0].Scale == glm::vec3(1.0f));
			CHECK(laid->Colliders[2].Scale == glm::vec3(1.0f));
			Result<BodyShapeDescription> turned = MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Scale = glm::vec2(2.0f, -3.0f), .Angle = std::numbers::pi_v<float> / 2.0f } } });
			REQUIRE(turned.has_value());
			CHECK(turned->Colliders[0].Scale == glm::vec3(2.0f, -3.0f, 1.0f));
			const glm::quat rotation = turned->Colliders[0].Rotation;
			CHECK(rotation.x == 0.0f);
			CHECK(rotation.y == 0.0f);
			CHECK(rotation.w == doctest::Approx(std::sqrt(0.5f)));
			CHECK(rotation.z == doctest::Approx(std::sqrt(0.5f)));

			// A polygon of two points has no area, and a circle cannot take a non-uniform scale.
			CHECK_FALSE(MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = PolygonShape2D{ .Points = { glm::vec2(0.0f), glm::vec2(1.0f) } } } } }).has_value());
			CHECK_FALSE(MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = CircleShape2D{}, .Scale = glm::vec2(1.0f, 2.0f) } } }).has_value());
		}

		TEST_CASE("Physics2DWorld: invalid colliders are refused with PHYSICS_INVALID_SHAPE, and the depth bounds a collider's size")
		{
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float infinity = std::numeric_limits<float>::infinity();
			CHECK(IsInvalidShape(MakeBody2DShape({})));
			CHECK(IsInvalidCollider({ .Geometry = BoxShape2D{ .HalfExtents = glm::vec2(0.0f, 1.0f) } }));
			CHECK(IsInvalidCollider({ .Geometry = BoxShape2D{ .HalfExtents = glm::vec2(nan, 1.0f) } }));
			CHECK(IsInvalidCollider({ .Geometry = CircleShape2D{ .Radius = -1.0f } }));
			CHECK(IsInvalidCollider({ .Geometry = CircleShape2D{ .Radius = infinity } }));
			CHECK(IsInvalidCollider({ .Geometry = PolygonShape2D{ .Points = { glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(infinity, 1.0f) } } }));
			CHECK(IsInvalidCollider({ .Scale = glm::vec2(0.0f, 1.0f) }));
			CHECK(IsInvalidCollider({ .Scale = glm::vec2(1.0f, nan) }));
			CHECK(IsInvalidCollider({ .Position = glm::vec2(infinity, 0.0f) }));
			CHECK(IsInvalidCollider({ .Angle = nan }));
			// A box whose diagonal overflows a float has no finite depth.
			CHECK(IsInvalidCollider({ .Geometry = BoxShape2D{ .HalfExtents = glm::vec2(3.0e38f) } }));

			// A circle takes a mirroring uniform scale, its sphere's scale uniform in size.
			Result<BodyShapeDescription> mirrored = MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = CircleShape2D{}, .Scale = glm::vec2(-2.0f, 2.0f) } } });
			REQUIRE(mirrored.has_value());
			CHECK(mirrored->Colliders[0].Scale == glm::vec3(-2.0f, 2.0f, 2.0f));
			CHECK(PhysicsShape::Create(*mirrored).has_value());

			// The depth, twice the diagonal of the bounding rectangle, is a collider dimension: a box whose diagonal exceeds
			// MaxColliderSize / 2 is refused when the shape is built.
			Result<BodyShapeDescription> largest = MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = BoxShape2D{ .HalfExtents = glm::vec2(17000.0f) } } } });
			REQUIRE(largest.has_value());
			CHECK(PhysicsShape::Create(*largest).has_value());
			Result<BodyShapeDescription> tooLarge = MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = BoxShape2D{ .HalfExtents = glm::vec2(20000.0f) } } } });
			REQUIRE(tooLarge.has_value());
			const Result<Ref<const PhysicsShape>> refused = PhysicsShape::Create(*tooLarge);
			REQUIRE_FALSE(refused.has_value());
			CHECK(GetPhysicsDiagnosticCode(refused.error()) == PhysicsInvalidShapeCode);

			// Points on one line span no area: laid, but refused when the hull is built.
			Result<BodyShapeDescription> line = MakeBody2DShape({ .Colliders = { Collider2DShapeDescription{ .Geometry = PolygonShape2D{ .Points = { glm::vec2(0.0f), glm::vec2(1.0f), glm::vec2(2.0f) } } } } });
			REQUIRE(line.has_value());
			CHECK_FALSE(PhysicsShape::Create(*line).has_value());
		}

		TEST_CASE("Physics2DWorld: poses convert to 3D about +Z and back through the rotation's twist about Z")
		{
			CHECK(ToPhysicsPose({ .Position = glm::vec2(1.5f, -2.0f) }) == PhysicsPose{ .Position = glm::vec3(1.5f, -2.0f, 0.0f) });
			const PhysicsPose quarter = ToPhysicsPose({ .Angle = std::numbers::pi_v<float> / 2.0f });
			CHECK(quarter.Rotation.x == 0.0f);
			CHECK(quarter.Rotation.y == 0.0f);
			CHECK(quarter.Rotation.w == doctest::Approx(std::sqrt(0.5f)));
			CHECK(quarter.Rotation.z == doctest::Approx(std::sqrt(0.5f)));
			for (const float angle : { -3.0f, -1.0f, 0.0f, 0.5f, 2.0f, 3.1f })
			{
				const Physics2DPose back = ToPhysics2DPose(ToPhysicsPose({ .Position = glm::vec2(4.0f, -0.25f), .Angle = angle }));
				CHECK(back.Position == glm::vec2(4.0f, -0.25f));
				CHECK(back.Angle == doctest::Approx(angle).epsilon(1.0e-6));
			}

			// The swing about X and Y is dropped, whichever side it is on, and either sign of the quaternion gives the angle.
			const glm::quat twist = ToPhysicsPose({ .Angle = 0.7f }).Rotation;
			const glm::quat swing(0.98006658f, 0.19866933f, 0.0f, 0.0f); // 0.4 rad about X
			CHECK(ToPhysics2DPose({ .Position = glm::vec3(1.0f, 2.0f, 3.0f), .Rotation = twist * swing }).Angle == doctest::Approx(0.7f));
			CHECK(ToPhysics2DPose({ .Rotation = swing * twist }).Angle == doctest::Approx(0.7f));
			CHECK(ToPhysics2DPose({ .Rotation = -twist }).Angle == doctest::Approx(0.7f));
			CHECK(ToPhysics2DPose({ .Position = glm::vec3(1.0f, 2.0f, 3.0f) }).Position == glm::vec2(1.0f, 2.0f));

			// A half turn is pi, never -pi, and a rotation without a twist part (a half turn about an axis in the plane) is 0.
			CHECK(ToPhysics2DPose({ .Rotation = glm::quat(0.0f, 0.0f, 0.0f, 1.0f) }).Angle == std::numbers::pi_v<float>);
			CHECK(ToPhysics2DPose({ .Rotation = glm::quat(0.0f, 0.0f, 0.0f, -1.0f) }).Angle == std::numbers::pi_v<float>);
			CHECK(ToPhysics2DPose({ .Rotation = glm::quat(-0.0f, 1.0f, 0.0f, 0.0f) }).Angle == 0.0f);
			CHECK(ToPhysics2DPose({ .Rotation = glm::quat(-0.0f, 0.0f, 1.0f, -0.0f) }).Angle == 0.0f);
		}

		TEST_CASE("Physics2DWorld: a dynamic body never leaves its plane")
		{
			Scope<Physics2DWorld> world = Create2DWorld();
			REQUIRE(world->CreateBody({ .Shape = Create2DShape(BoxShape2D{ .HalfExtents = glm::vec2(20.0f, 0.5f) }), .MotionType = PhysicsMotionType::Static, .Pose = { .Position = glm::vec2(0.0f, -0.5f) } })
					.has_value());
			// 60 boxes and circles, then 24 pentagons and box-and-circle compounds stacked on them.
			std::vector<BodyHandle> bodies;
			for (int index = 0; index < 84; ++index)
			{
				Ref<const PhysicsShape> shape;
				if (index < 60)
					shape = Create2DShape(index % 2 == 0 ? Shape2DGeometry(BoxShape2D{ .HalfExtents = glm::vec2(0.3f, 0.2f) }) : Shape2DGeometry(CircleShape2D{ .Radius = 0.25f }));
				else
					shape = index % 2 == 0 ? Create2DShape(MakePentagon()) : Create2DShape(MakeBoxAndCircle());
				Result<BodyHandle> body = world->CreateBody({ .Shape = shape,
					.Pose = { .Position = glm::vec2(static_cast<float>(index % 6) * 0.45f - 1.2f, 0.5f + static_cast<float>(index / 6) * 0.6f), .Angle = 0.1f * static_cast<float>(index) },
					.LinearVelocity = glm::vec2(0.5f * static_cast<float>(index % 3) - 0.5f, 0.0f),
					.AngularVelocity = 2.0f - 0.07f * static_cast<float>(index) });
				REQUIRE(body.has_value());
				bodies.push_back(*body);
			}
			for (int step = 0; step < 600; ++step)
				REQUIRE(world->Step(FixedDelta2D, 1).has_value());
			// Exactly on the plane, rotated about Z only: the extruded shapes put no motion out of it.
			for (const BodyHandle body : bodies)
				CheckOnPlane(*world, body);
		}

		TEST_CASE("Physics2DWorld: a ray in the plane hits an extruded box with an in-plane normal")
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

		TEST_CASE("Physics2DWorld: rays and overlaps meet circles, polygons and turned boxes in the plane")
		{
			Scope<Physics2DWorld> world = Create2DWorld();
			const BodyHandle box = CreateStatic2DBox(*world, glm::vec2(0.0f), glm::vec2(1.0f, 0.5f));
			Result<BodyHandle> circle = world->CreateBody({ .Shape = Create2DShape(CircleShape2D{ .Radius = 0.5f }), .MotionType = PhysicsMotionType::Static, .Pose = { .Position = glm::vec2(4.0f, 0.0f) } });
			const PolygonShape2D triangle{ .Points = { glm::vec2(-0.5f, -0.5f), glm::vec2(0.5f, -0.5f), glm::vec2(0.0f, 0.5f) } };
			Result<BodyHandle> polygon = world->CreateBody({ .Shape = Create2DShape(triangle), .MotionType = PhysicsMotionType::Static, .Pose = { .Position = glm::vec2(-4.0f, 0.0f) } });
			const BodyHandle turned = CreateStatic2DBox(*world, glm::vec2(0.0f, 4.0f), glm::vec2(1.0f, 0.2f), std::numbers::pi_v<float> / 4.0f);
			REQUIRE(circle.has_value());
			REQUIRE(polygon.has_value());

			// Rays: a circle's top, a polygon's slanted side (its outward normal (-1, 0.5) normalized) and a turned box's
			// slanted top, each with its point at z = 0 and its normal in the plane.
			const std::optional<PhysicsQueryHit> top = world->Raycast(glm::vec2(4.0f, 3.0f), glm::vec2(0.0f, -1.0f), 10.0f, {});
			REQUIRE(top.has_value());
			CHECK(top->Body == *circle);
			CHECK(top->Distance == doctest::Approx(2.5f));
			CHECK(top->Normal == glm::vec3(0.0f, 1.0f, 0.0f));
			const std::optional<PhysicsQueryHit> side = world->Raycast(glm::vec2(-4.25f, 3.0f), glm::vec2(0.0f, -1.0f), 10.0f, {});
			REQUIRE(side.has_value());
			CHECK(side->Body == *polygon);
			CHECK(side->Distance == doctest::Approx(3.0f).epsilon(1.0e-4));
			CHECK(side->Point.z == 0.0f);
			CHECK(side->Normal.x == doctest::Approx(-1.0f / std::sqrt(1.25f)).epsilon(1.0e-4));
			CHECK(side->Normal.y == doctest::Approx(0.5f / std::sqrt(1.25f)).epsilon(1.0e-4));
			CHECK(side->Normal.z == 0.0f);
			const std::optional<PhysicsQueryHit> slope = world->Raycast(glm::vec2(0.0f, 8.0f), glm::vec2(0.0f, -1.0f), 10.0f, {});
			REQUIRE(slope.has_value());
			CHECK(slope->Body == turned);
			CHECK(slope->Normal.x == doctest::Approx(-std::sqrt(0.5f)).epsilon(1.0e-4));
			CHECK(slope->Normal.y == doctest::Approx(std::sqrt(0.5f)).epsilon(1.0e-4));
			CHECK(slope->Normal.z == 0.0f);
			// Along the X axis: the polygon's slanted side, the box's left face and the circle's leftmost point.
			const std::vector<PhysicsQueryHit> across = world->RaycastAll(glm::vec2(-10.0f, 0.0f), glm::vec2(1.0f, 0.0f), 20.0f, {});
			CHECK(across.size() == 3);
			for (const PhysicsQueryHit& hit : across)
			{
				CHECK(hit.Point.z == 0.0f);
				CHECK(hit.Normal.z == 0.0f);
				CHECK(hit.Normal.x < 0.0f);
				CHECK(glm::length(hit.Normal) == doctest::Approx(1.0f));
			}

			// Overlaps: a circle reaching the circle body, a small box inside the polygon, a box that reaches the box body only
			// once it is turned a quarter, and a box over everything.
			const auto overlapped = [&world](const Overlap2DShape& shape, const Physics2DPose& pose)
			{
				std::vector<BodyHandle> bodies;
				for (const PhysicsOverlap& overlap : world->Overlap(shape, pose, {}))
					bodies.push_back(overlap.Body);
				std::sort(bodies.begin(), bodies.end());
				return bodies;
			};
			CHECK(overlapped(CircleShape2D{ .Radius = 0.6f }, { .Position = glm::vec2(4.9f, 0.0f) }) == std::vector<BodyHandle>{ *circle });
			CHECK(overlapped(CircleShape2D{ .Radius = 0.3f }, { .Position = glm::vec2(5.2f, 0.0f) }).empty());
			CHECK(overlapped(BoxShape2D{ .HalfExtents = glm::vec2(0.1f) }, { .Position = glm::vec2(-4.0f, -0.2f) }) == std::vector<BodyHandle>{ *polygon });
			CHECK(overlapped(BoxShape2D{ .HalfExtents = glm::vec2(0.1f, 1.5f) }, { .Position = glm::vec2(1.5f, 0.0f) }).empty());
			CHECK(overlapped(BoxShape2D{ .HalfExtents = glm::vec2(0.1f, 1.5f) }, { .Position = glm::vec2(1.5f, 0.0f), .Angle = std::numbers::pi_v<float> / 2.0f })
				== std::vector<BodyHandle>{ box });
			CHECK(overlapped(BoxShape2D{ .HalfExtents = glm::vec2(10.0f) }, {}).size() == 4);

			// Bounds are the extruded shape's.
			const std::optional<Aabb> bounds = world->GetBodyBounds(box);
			REQUIRE(bounds.has_value());
			const float depth = GetPhysics2DHalfDepth(BoxShape2D{ .HalfExtents = glm::vec2(1.0f, 0.5f) }, glm::vec2(1.0f));
			CHECK(bounds->Min.x == doctest::Approx(-1.0f));
			CHECK(bounds->Max.y == doctest::Approx(0.5f));
			CHECK(bounds->Min.z == doctest::Approx(-depth));
			CHECK(bounds->Max.z == doctest::Approx(depth));
		}

		TEST_CASE("Physics2DWorld: a body sunk deep into a box is pushed out in the plane")
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

		TEST_CASE("Physics2DWorld: a small body's angular acceleration under a torque follows its 2D inertia")
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

		TEST_CASE("Physics2DWorld: forces, impulses, velocities, teleports and kinematic moves act in the plane")
		{
			Scope<Physics2DWorld> world = Create2DWorld();
			REQUIRE(world->SetGravity(glm::vec2(0.0f)).has_value());
			CHECK(world->GetGravity() == glm::vec2(0.0f));

			// Starting velocities, then impulses: a 2 kg unit box (inertia about Z 2 (1 + 1) / 12 = 1/3).
			const Result<BodyHandle> box = world->CreateBody({ .Shape = Create2DShape(BoxShape2D{}),
				.Mass = 2.0f,
				.LinearDamping = 0.0f,
				.AngularDamping = 0.0f,
				.LinearVelocity = glm::vec2(1.0f, -2.0f),
				.AngularVelocity = 0.5f });
			REQUIRE(box.has_value());
			CHECK(world->GetLinearVelocity(*box) == glm::vec2(1.0f, -2.0f));
			CHECK(world->GetAngularVelocity(*box) == 0.5f);
			world->SetLinearVelocity(*box, glm::vec2(0.0f));
			world->SetAngularVelocity(*box, 0.0f);
			world->AddImpulse(*box, glm::vec2(4.0f, 0.0f));
			world->AddAngularImpulse(*box, 1.0f);
			CHECK(world->GetLinearVelocity(*box) == glm::vec2(2.0f, 0.0f));
			CHECK(world->GetAngularVelocity(*box) == doctest::Approx(3.0f).epsilon(1.0e-4));

			// A force at a point off the centre of mass pushes and turns (counter-clockwise for +Y at +X); a FixedRotation body
			// only translates.
			const Result<BodyHandle> pushed = world->CreateBody({ .Shape = Create2DShape(BoxShape2D{}), .Pose = { .Position = glm::vec2(10.0f, 0.0f) } });
			const Result<BodyHandle> fixed = world->CreateBody({ .Shape = Create2DShape(BoxShape2D{}), .Pose = { .Position = glm::vec2(-10.0f, 0.0f) }, .FixedRotation = true });
			REQUIRE(pushed.has_value());
			REQUIRE(fixed.has_value());
			world->AddForceAtPosition(*pushed, glm::vec2(0.0f, 60.0f), glm::vec2(11.0f, 0.0f));
			world->AddForce(*fixed, glm::vec2(60.0f, 0.0f));
			world->AddTorque(*fixed, 100.0f);
			world->AddAngularImpulse(*fixed, 5.0f);

			// A kinematic body moves to its planar target in one step; a teleport places a body exactly.
			const Result<BodyHandle> platform = world->CreateBody({ .Shape = Create2DShape(BoxShape2D{}), .MotionType = PhysicsMotionType::Kinematic, .Pose = { .Position = glm::vec2(0.0f, 10.0f) } });
			REQUIRE(platform.has_value());
			world->MoveKinematic(*platform, { .Position = glm::vec2(1.0f, 10.5f), .Angle = 0.25f }, FixedDelta2D);
			REQUIRE(world->Step(FixedDelta2D, 1).has_value());

			CHECK(world->GetLinearVelocity(*pushed).x == 0.0f);
			CHECK(world->GetLinearVelocity(*pushed).y > 0.0f);
			CHECK(world->GetAngularVelocity(*pushed) > 0.0f);
			CHECK(world->GetLinearVelocity(*fixed).x > 0.0f);
			CHECK(world->GetAngularVelocity(*fixed) == 0.0f);
			CHECK(world->GetPose(*fixed).Angle == 0.0f);
			const Physics2DPose moved = world->GetPose(*platform);
			CHECK(moved.Position.x == doctest::Approx(1.0f).epsilon(1.0e-4));
			CHECK(moved.Position.y == doctest::Approx(10.5f).epsilon(1.0e-4));
			CHECK(moved.Angle == doctest::Approx(0.25f).epsilon(1.0e-4));

			world->SetPose(*box, { .Position = glm::vec2(5.0f, -5.0f), .Angle = 1.0f }, true);
			const Physics2DPose teleported = world->GetPose(*box);
			CHECK(teleported.Position == glm::vec2(5.0f, -5.0f));
			CHECK(teleported.Angle == doctest::Approx(1.0f).epsilon(1.0e-6));
			CHECK_FALSE(world->IsSleeping(*box));
			for (const BodyHandle body : { *box, *pushed, *fixed, *platform })
				CheckOnPlane(*world, body);
		}

		TEST_CASE("Physics2DWorld: contact records lie in the plane and name the colliders' user data")
		{
			Scope<Physics2DWorld> world = Create2DWorld();
			Result<BodyHandle> floor = world->CreateBody({ .Shape = Create2DShape({ Collider2DShapeDescription{ .Geometry = BoxShape2D{ .HalfExtents = glm::vec2(5.0f, 0.5f) }, .UserData = 7 } }),
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = glm::vec2(0.0f, -0.5f) } });
			REQUIRE(floor.has_value());
			std::vector<BodyHandle> bodies;
			for (const Shape2DGeometry& geometry : { Shape2DGeometry(BoxShape2D{}), Shape2DGeometry(CircleShape2D{ .Radius = 0.25f }), Shape2DGeometry(MakePentagon()) })
			{
				Result<BodyHandle> body = world->CreateBody({ .Shape = Create2DShape(geometry),
					.Pose = { .Position = glm::vec2(-2.0f + 2.0f * static_cast<float>(bodies.size()), 0.6f), .Angle = 0.05f } });
				REQUIRE(body.has_value());
				bodies.push_back(*body);
			}
			std::vector<ContactEvent> contacts;
			for (int step = 0; step < 60; ++step)
			{
				REQUIRE(world->Step(FixedDelta2D, 1).has_value());
				const std::vector<ContactEvent> drained = world->DrainContactEvents();
				contacts.insert(contacts.end(), drained.begin(), drained.end());
			}
			REQUIRE_FALSE(contacts.empty());
			for (const ContactEvent& event : contacts)
			{
				// A manifold's points spread over the shapes' depth along Z; each record keeps the one in the plane.
				CHECK(event.Point.z == 0.0f);
				CHECK(event.Normal.z == 0.0f);
				CHECK(glm::length(event.Normal) == doctest::Approx(1.0f));
			}
			for (const BodyHandle body : bodies)
			{
				const std::optional<ContactEvent> added = FindAdded(contacts, *floor, body);
				REQUIRE(added.has_value());
				CHECK(std::abs(added->Normal.y) == doctest::Approx(1.0f).epsilon(1.0e-3));
				const bool floorIsA = added->BodyA == *floor;
				CHECK((floorIsA ? added->ColliderA : added->ColliderB) == 7);
				CHECK(world->GetSubShapeCollider(*floor, floorIsA ? added->SubShapeA : added->SubShapeB) == 7);
			}

			// At rest, the bodies fall asleep; a sleeping body wakes when asked.
			for (int step = 0; step < 180; ++step)
				REQUIRE(world->Step(FixedDelta2D, 1).has_value());
			CHECK(world->IsSleeping(bodies.front()));
			world->WakeUp(bodies.front());
			CHECK_FALSE(world->IsSleeping(bodies.front()));
		}

		TEST_CASE("Physics2DWorld: a 2D pile ends in the same state with 0, 1 and 8 Jolt threads")
		{
			std::vector<ContactEvent> noneContacts;
			std::vector<ContactEvent> oneContacts;
			std::vector<ContactEvent> eightContacts;
			const uint64_t none = Run2DPile(0, noneContacts);
			const uint64_t one = Run2DPile(1, oneContacts);
			const uint64_t eight = Run2DPile(8, eightContacts);
			CHECK(one == none);
			CHECK(eight == none);
			CHECK_FALSE(noneContacts.empty());
			CHECK(oneContacts == noneContacts);
			CHECK(eightContacts == noneContacts);
		}

		TEST_CASE("Physics2DWorld: the world's gravity, layers and bodies are kept in the plane, and invalid ones are refused")
		{
			const std::vector<std::string> layers = { "Default", "Tiles" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Tiles" } };
			Result<PhysicsLayerTable> table = PhysicsLayerTable::Create(layers, collisions);
			REQUIRE(table.has_value());
			Result<Scope<Physics2DWorld>> created = Physics2DWorld::Create({ .Gravity = glm::vec2(1.0f, -3.0f), .Layers = *table });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			Physics2DWorld& world = **created;
			CHECK(world.GetGravity() == glm::vec2(1.0f, -3.0f));
			REQUIRE(world.GetWorld() != nullptr);
			CHECK(world.GetWorld()->GetGravity() == glm::vec3(1.0f, -3.0f, 0.0f));
			CHECK(world.GetLayers().GetLayerCount() == 2);
			CHECK(world.GetWorld()->GetLayers().GetLayerCount() == 2);

			const float nan = std::numeric_limits<float>::quiet_NaN();
			const Result<Scope<Physics2DWorld>> notFinite = Physics2DWorld::Create({ .Gravity = glm::vec2(nan, 0.0f) });
			REQUIRE_FALSE(notFinite.has_value());
			CHECK(notFinite.error().GetCode() == ErrorCode::InvalidArgument);
			const Status tooStrong = world.SetGravity(glm::vec2(0.0f, -2.0f * MaxPhysicsGravity));
			REQUIRE_FALSE(tooStrong.has_value());
			CHECK(tooStrong.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(world.GetGravity() == glm::vec2(1.0f, -3.0f));

			const Ref<const PhysicsShape> shape = Create2DShape(CircleShape2D{});
			const Result<BodyHandle> turned = world.CreateBody({ .Shape = shape, .Pose = { .Angle = nan } });
			REQUIRE_FALSE(turned.has_value());
			CHECK(turned.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<BodyHandle> distant = world.CreateBody({ .Shape = shape, .Pose = { .Position = glm::vec2(2.0f * MaxPhysicsCoordinate, 0.0f) } });
			REQUIRE_FALSE(distant.has_value());
			CHECK(distant.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<BodyHandle> dynamicSensor = world.CreateBody({ .Shape = shape, .IsSensor = true });
			REQUIRE_FALSE(dynamicSensor.has_value());
			CHECK(GetPhysicsDiagnosticCode(dynamicSensor.error()) == PhysicsDynamicTriggerCode);
			CHECK(world.GetStats().BodyCount == 0);

			// A body keeps what it was described with.
			const Result<BodyHandle> sensor = world.CreateBody({ .Shape = shape,
				.MotionType = PhysicsMotionType::Kinematic,
				.Pose = { .Position = glm::vec2(1.0f, 2.0f), .Angle = 0.5f },
				.Layer = 1,
				.IsSensor = true,
				.UserData = 99 });
			REQUIRE(sensor.has_value());
			CHECK(world.IsBodyValid(*sensor));
			CHECK(world.GetUserData(*sensor) == 99);
			CHECK(world.GetMotionType(*sensor) == PhysicsMotionType::Kinematic);
			CHECK(world.IsSensor(*sensor));
			CHECK(world.GetLayer(*sensor) == 1);
			CHECK(world.GetShape(*sensor) == shape);
			CHECK(world.GetPose(*sensor).Position == glm::vec2(1.0f, 2.0f));
			CHECK(world.GetPose(*sensor).Angle == doctest::Approx(0.5f).epsilon(1.0e-6));
			CHECK(world.GetSubShapeBounds(*sensor, 0).has_value());
			const Ref<const PhysicsShape> larger = Create2DShape(CircleShape2D{ .Radius = 2.0f });
			REQUIRE(world.SetShape(*sensor, larger).has_value());
			CHECK(world.GetShape(*sensor) == larger);
			CHECK(world.GetStats().BodyCount == 1);
			world.DestroyBody(*sensor);
			CHECK_FALSE(world.IsBodyValid(*sensor));
			CHECK(world.GetStats().BodyCount == 0);
		}

		TEST_CASE("Physics2DWorld: a pose that is not finite and an empty query shape are programmer errors that assert")
		{
			// Callers validate their input first (Physics2DWorld.h); a release build without asserts ignores the call.
			ENGINE_CHECK_DEATH("Physics/World2DSetsNonFinitePose", "Physics2DWorld::SetPose");
			ENGINE_CHECK_DEATH("Physics/World2DOverlapsEmptyCircle", "Physics2DWorld::Overlap");
		}
	}

}
