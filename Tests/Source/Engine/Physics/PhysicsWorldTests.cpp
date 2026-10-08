#include "TestsPCH.h"

#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Physics/PhysicsDiagnostics.h"

#include <cmath>
#include <limits>

// The physics world (Architecture §9.1, §9.3; §9.7: free fall vs analytic, rest height (0.48 with slop), restitution
// bounce, layer matrix, CCD prevents tunnelling at 200 m/s; Roadmap M11 acceptance). ECS-agnostic: bodies are created
// directly. Skipped skeletons of the M11 contract (Docs/Decisions/0014-m11-decisions.md): stream A implements the world and
// removes the skips. Physics: CCD prevents tunnelling at 200 m/s is a Roadmap acceptance name, used verbatim.

namespace Engine {

	namespace {

		constexpr float FixedDelta = 1.0f / 60.0f;

		Scope<PhysicsWorld> CreateWorld(const PhysicsWorldSpecification& specification = {})
		{
			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create(specification);
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			return std::move(*world);
		}

		Ref<const PhysicsShape> CreateShape(const PhysicsShapeGeometry& geometry)
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = geometry } } });
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			return std::move(*shape);
		}

		// A static box 100 m x 1 m x 100 m whose top face is at y = 0, on `layer`.
		BodyHandle CreateGround(PhysicsWorld& world, uint32_t layer = 0)
		{
			Result<BodyHandle> ground = world.CreateBody({ .Shape = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(50.0f, 0.5f, 50.0f) }),
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = glm::vec3(0.0f, -0.5f, 0.0f) },
				.Layer = layer });
			REQUIRE(ground.has_value());
			return *ground;
		}

		BodyHandle CreateDynamic(PhysicsWorld& world, const PhysicsShapeGeometry& geometry, const glm::vec3& position, BodyDescription description = {})
		{
			description.Shape = CreateShape(geometry);
			description.MotionType = PhysicsMotionType::Dynamic;
			description.Pose.Position = position;
			Result<BodyHandle> body = world.CreateBody(description);
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			return *body;
		}

		void StepWorld(PhysicsWorld& world, uint32_t steps)
		{
			for (uint32_t step = 0; step < steps; ++step)
				REQUIRE(world.Step(FixedDelta, 1).has_value());
		}

		// One triangle in the XZ plane: a mesh, which a Dynamic body cannot use and Jolt gives no mass.
		MeshShapeGeometry MakeTriangle()
		{
			return MeshShapeGeometry{ .Vertices = { glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) }, .Indices = { 0, 1, 2 } };
		}

		// Whether `contacts` holds an Added record between `a` and `b`, in either order.
		bool HasAddedContact(const std::vector<ContactEvent>& contacts, BodyHandle a, BodyHandle b)
		{
			return std::any_of(contacts.begin(), contacts.end(), [a, b](const ContactEvent& event)
			{
				return event.Kind == ContactEventKind::Added && ((event.BodyA == a && event.BodyB == b) || (event.BodyA == b && event.BodyB == a));
			});
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsWorld: a falling sphere follows the analytic free fall" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ball = CreateDynamic(*world, SphereShapeGeometry{ .Radius = 0.5f }, glm::vec3(0.0f, 100.0f, 0.0f),
				{ .LinearDamping = 0.0f, .AllowSleeping = false });
			StepWorld(*world, 60);
			// Jolt integrates velocity first (symplectic Euler): after n steps y = y0 - g dt^2 n (n + 1) / 2.
			const float expected = 100.0f - 9.81f * FixedDelta * FixedDelta * 60.0f * 61.0f / 2.0f;
			CHECK(world->GetPose(ball).Position.y == doctest::Approx(expected).epsilon(1.0e-3));
			CHECK(world->GetLinearVelocity(ball).y == doctest::Approx(-9.81f).epsilon(1.0e-3));
		}

		TEST_CASE("PhysicsWorld: a box comes to rest at its half height minus the penetration slop" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			const BodyHandle box = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 2.0f, 0.0f));
			StepWorld(*world, 300);
			// §9.1: resting contacts penetrate by mPenetrationSlop (0.02 m), so a 0.5 m half height rests at 0.48.
			CHECK(world->GetPose(box).Position.y == doctest::Approx(0.48f).epsilon(0.01));
			CHECK(world->IsSleeping(box));
		}

		TEST_CASE("PhysicsWorld: a restitution of 1 bounces a ball back near its drop height" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			const BodyHandle ball = CreateDynamic(*world, SphereShapeGeometry{ .Radius = 0.5f }, glm::vec3(0.0f, 5.5f, 0.0f),
				{ .Restitution = 1.0f, .LinearDamping = 0.0f, .AllowSleeping = false });
			float highestAfterBounce = 0.0f;
			bool bounced = false;
			for (uint32_t step = 0; step < 240; ++step)
			{
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				const float velocity = world->GetLinearVelocity(ball).y;
				bounced = bounced || velocity > 0.0f;
				if (bounced)
					highestAfterBounce = std::max(highestAfterBounce, world->GetPose(ball).Position.y);
			}
			CHECK(bounced);
			// The ground's restitution is 0; Jolt combines the two as their maximum by default.
			CHECK(highestAfterBounce == doctest::Approx(5.5f).epsilon(0.05));
		}

		TEST_CASE("Physics: CCD prevents tunnelling at 200 m/s" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld();
			// A 5 cm wall; at 200 m/s a 10 cm ball moves 3.3 m per step.
			Result<BodyHandle> wall = world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(0.025f, 5.0f, 5.0f) }),
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = glm::vec3(10.0f, 0.0f, 0.0f) } });
			REQUIRE(wall.has_value());
			const auto launch = [&world](PhysicsMotionQuality quality, float z)
			{
				return CreateDynamic(*world, SphereShapeGeometry{ .Radius = 0.05f }, glm::vec3(0.0f, 0.0f, z),
					{ .MotionQuality = quality, .GravityFactor = 0.0f, .AllowSleeping = false, .LinearVelocity = glm::vec3(200.0f, 0.0f, 0.0f) });
			};
			const BodyHandle continuous = launch(PhysicsMotionQuality::LinearCast, -1.0f);
			const BodyHandle discrete = launch(PhysicsMotionQuality::Discrete, 1.0f);
			StepWorld(*world, 30);
			CHECK(world->GetPose(continuous).Position.x < 10.0f);
			// The control case: without CCD the same ball passes through.
			CHECK(world->GetPose(discrete).Position.x > 10.0f);
		}

		TEST_CASE("PhysicsWorld: layers the matrix does not pair pass through each other" * doctest::skip(true))
		{
			const std::vector<std::string> layers = { "Default", "Ghost" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Default" } };
			Result<PhysicsLayerTable> table = PhysicsLayerTable::Create(layers, collisions);
			REQUIRE(table.has_value());
			Scope<PhysicsWorld> world = CreateWorld({ .Layers = *table });
			static_cast<void>(CreateGround(*world, 0));
			const BodyHandle solid = CreateDynamic(*world, SphereShapeGeometry{}, glm::vec3(-2.0f, 1.0f, 0.0f));
			const BodyHandle ghost = CreateDynamic(*world, SphereShapeGeometry{}, glm::vec3(2.0f, 1.0f, 0.0f), { .Layer = 1 });
			StepWorld(*world, 120);
			CHECK(world->GetPose(solid).Position.y > 0.4f);
			CHECK(world->GetPose(ghost).Position.y < -1.0f);
			CHECK(world->GetLayer(ghost) == 1);
		}

		TEST_CASE("PhysicsWorld: contacts are buffered with the colliders' user data, and sensors see sleeping bodies" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ground = CreateGround(*world);
			const BodyHandle box = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 0.6f, 0.0f));
			StepWorld(*world, 10);
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			const auto added = std::find_if(contacts.begin(), contacts.end(), [ground, box](const ContactEvent& event)
			{
				return event.Kind == ContactEventKind::Added && ((event.BodyA == ground && event.BodyB == box) || (event.BodyA == box && event.BodyB == ground));
			});
			REQUIRE(added != contacts.end());
			CHECK_FALSE(added->IsSensor);
			CHECK(std::isfinite(added->Point.y));
			StepWorld(*world, 300);
			REQUIRE(world->IsSleeping(box));
			static_cast<void>(world->DrainContactEvents());
			// A kinematic sensor around the sleeping box reports it (§9.2: sensors are Kinematic and kept active).
			Result<BodyHandle> sensor = world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(2.0f) }),
				.MotionType = PhysicsMotionType::Kinematic,
				.Pose = { .Position = glm::vec3(0.0f, 0.5f, 0.0f) },
				.IsSensor = true,
				.CollideKinematicVsNonDynamic = true });
			REQUIRE(sensor.has_value());
			StepWorld(*world, 2);
			const std::vector<ContactEvent> sensed = world->DrainContactEvents();
			CHECK(std::any_of(sensed.begin(), sensed.end(), [&](const ContactEvent& event)
			{
				return event.Kind == ContactEventKind::Added && event.IsSensor && (event.BodyA == box || event.BodyB == box);
			}));
		}

		TEST_CASE("PhysicsWorld: invalid bodies are refused with their codes and nothing reaches Jolt" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld({ .Limits = { .MaxBodies = 2 } });
			const Ref<const PhysicsShape> box = CreateShape(BoxShapeGeometry{});
			// The code of a refusal, or "<created>"; no assertion inside, so it can sit in a CHECK.
			const auto codeOf = [&world](const BodyDescription& description) -> std::string
			{
				Result<BodyHandle> body = world->CreateBody(description);
				return body.has_value() ? std::string("<created>") : std::string(GetPhysicsDiagnosticCode(body.error()));
			};
			CHECK(codeOf({ .Shape = box, .MotionType = PhysicsMotionType::Dynamic, .IsSensor = true }) == PhysicsDynamicTriggerCode);
			CHECK(codeOf({ .Shape = box, .MotionType = PhysicsMotionType::Dynamic, .AllowedDofs = PhysicsDofs::None }) == PhysicsAllDofsLockedCode);
			const Ref<const PhysicsShape> mesh = CreateShape(MakeTriangle());
			CHECK(codeOf({ .Shape = mesh, .MotionType = PhysicsMotionType::Dynamic }) == PhysicsNonconvexDynamicCode);
			// Non-finite values and missing shapes are argument errors without a code.
			Result<BodyHandle> noShape = world->CreateBody({});
			REQUIRE_FALSE(noShape.has_value());
			CHECK(noShape.error().GetCode() == ErrorCode::InvalidArgument);
			Result<BodyHandle> nanPose = world->CreateBody({ .Shape = box, .Pose = { .Position = glm::vec3(std::numeric_limits<float>::quiet_NaN()) } });
			REQUIRE_FALSE(nanPose.has_value());
			CHECK(nanPose.error().GetCode() == ErrorCode::InvalidArgument);
			// The body limit.
			REQUIRE(world->CreateBody({ .Shape = box, .MotionType = PhysicsMotionType::Static }).has_value());
			REQUIRE(world->CreateBody({ .Shape = box, .MotionType = PhysicsMotionType::Static }).has_value());
			CHECK(codeOf({ .Shape = box, .MotionType = PhysicsMotionType::Static }) == PhysicsLimitExceededCode);
			CHECK(world->GetStats().BodyCount == 2);
		}

		TEST_CASE("PhysicsWorld: teleports, kinematic moves, forces and velocities act on the right bodies" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ball = CreateDynamic(*world, SphereShapeGeometry{}, glm::vec3(0.0f, 10.0f, 0.0f), { .GravityFactor = 0.0f });
			world->SetPose(ball, { .Position = glm::vec3(5.0f, 10.0f, 0.0f) }, true);
			CHECK(Test::ApproxEqual(world->GetPose(ball).Position, glm::vec3(5.0f, 10.0f, 0.0f)));
			world->AddImpulse(ball, glm::vec3(1.0f, 0.0f, 0.0f));
			CHECK(world->GetLinearVelocity(ball).x == doctest::Approx(1.0f));
			world->SetAngularVelocity(ball, glm::vec3(0.0f, 2.0f, 0.0f));
			CHECK(Test::ApproxEqual(world->GetAngularVelocity(ball), glm::vec3(0.0f, 2.0f, 0.0f)));

			Result<BodyHandle> platform = world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{}), .MotionType = PhysicsMotionType::Kinematic });
			REQUIRE(platform.has_value());
			world->MoveKinematic(*platform, { .Position = glm::vec3(0.0f, 0.0f, 1.0f) }, FixedDelta);
			// MoveKinematic sets the velocity that reaches the target in one step.
			CHECK(world->GetLinearVelocity(*platform).z == doctest::Approx(60.0f).epsilon(1.0e-3));
			REQUIRE(world->Step(FixedDelta, 1).has_value());
			CHECK(world->GetPose(*platform).Position.z == doctest::Approx(1.0f).epsilon(1.0e-4));
			CHECK(world->GetMotionType(*platform) == PhysicsMotionType::Kinematic);

			REQUIRE(world->SetGravity(glm::vec3(0.0f, -20.0f, 0.0f)).has_value());
			CHECK(Test::ApproxEqual(world->GetGravity(), glm::vec3(0.0f, -20.0f, 0.0f)));
			const Status infiniteGravity = world->SetGravity(glm::vec3(std::numeric_limits<float>::infinity()));
			REQUIRE_FALSE(infiniteGravity.has_value());
			CHECK(infiniteGravity.error().GetCode() == ErrorCode::InvalidArgument);
			const Status zeroDelta = world->Step(0.0f, 1);
			REQUIRE_FALSE(zeroDelta.has_value());
			CHECK(zeroDelta.error().GetCode() == ErrorCode::InvalidArgument);
			const Status noCollisionStep = world->Step(FixedDelta, 0);
			REQUIRE_FALSE(noCollisionStep.has_value());
			CHECK(noCollisionStep.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PhysicsWorld: settings Jolt would assert on are made safe before they reach it" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const Ref<const PhysicsShape> mesh = CreateShape(MakeTriangle());
			// A Kinematic triangle mesh (Jolt's MeshShape has no mass; the world provides a Kinematic body's mass properties),
			// as an implicit mesh trigger is.
			Result<BodyHandle> meshTrigger = world->CreateBody({ .Shape = mesh,
				.MotionType = PhysicsMotionType::Kinematic,
				.IsSensor = true,
				.CollideKinematicVsNonDynamic = true });
			REQUIRE_MESSAGE(meshTrigger.has_value(), meshTrigger.error().ToString());
			// A Kinematic body whose degrees of freedom are all locked: only Dynamic bodies use AllowedDofs and Mass.
			Result<BodyHandle> locked = world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{}),
				.MotionType = PhysicsMotionType::Kinematic,
				.AllowedDofs = PhysicsDofs::None,
				.Mass = 0.0f });
			REQUIRE_MESSAGE(locked.has_value(), locked.error().ToString());
			// A Kinematic body takes a mesh later too, keeping the mass properties it was created with. It now overlaps the
			// mesh trigger: Jolt has no mesh-versus-mesh collision (it asserts on the pair), so the world never lets them meet.
			REQUIRE(world->SetShape(*locked, mesh).has_value());
			world->MoveKinematic(*locked, { .Position = glm::vec3(0.0f, 0.0f, 0.25f) }, FixedDelta);
			// Starting velocities above the maxima are clamped to them; so are velocities set later.
			const BodyHandle fast = CreateDynamic(*world, SphereShapeGeometry{}, glm::vec3(0.0f, 10.0f, 0.0f),
				{ .GravityFactor = 0.0f,
					.MaxLinearVelocity = 100.0f,
					.MaxAngularVelocity = 10.0f,
					.AllowSleeping = false,
					.LinearVelocity = glm::vec3(600.0f, 0.0f, 0.0f),
					.AngularVelocity = glm::vec3(0.0f, 50.0f, 0.0f) });
			CHECK(world->GetLinearVelocity(fast).x == doctest::Approx(100.0f));
			CHECK(world->GetAngularVelocity(fast).y == doctest::Approx(10.0f));
			world->SetLinearVelocity(fast, glm::vec3(0.0f, 0.0f, 1000.0f));
			CHECK(world->GetLinearVelocity(fast).z == doctest::Approx(100.0f));
			StepWorld(*world, 2);
			CHECK_FALSE(HasAddedContact(world->DrainContactEvents(), *meshTrigger, *locked));
			CHECK(world->GetStats().BodyCount == 3);
		}

		TEST_CASE("PhysicsWorld: bodies that share a collision group never collide" * doctest::skip(true))
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ground = CreateGround(*world);
			// A sensor of group 7 around a resting box of the same group and a box of none.
			const BodyHandle owned = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 0.5f, 0.0f), { .CollisionGroup = 7 });
			const BodyHandle stranger = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(3.0f, 0.5f, 0.0f));
			Result<BodyHandle> sensor = world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(3.0f, 1.0f, 1.0f) }),
				.MotionType = PhysicsMotionType::Kinematic,
				.Pose = { .Position = glm::vec3(1.5f, 0.5f, 0.0f) },
				.IsSensor = true,
				.CollideKinematicVsNonDynamic = true,
				.CollisionGroup = 7 });
			REQUIRE(sensor.has_value());
			StepWorld(*world, 2);
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			CHECK(HasAddedContact(contacts, *sensor, stranger));
			CHECK_FALSE(HasAddedContact(contacts, *sensor, owned));
			// Group 7 says nothing about the ground, which has no group: both boxes rest on it.
			CHECK(HasAddedContact(contacts, ground, owned));
			CHECK(HasAddedContact(contacts, ground, stranger));
		}

		TEST_CASE("PhysicsWorld: a world needs the physics engine and valid settings" * doctest::skip(true))
		{
			Result<Scope<PhysicsWorld>> badGravity = PhysicsWorld::Create({ .Gravity = glm::vec3(std::numeric_limits<float>::quiet_NaN()) });
			REQUIRE_FALSE(badGravity.has_value());
			CHECK(badGravity.error().GetCode() == ErrorCode::InvalidArgument);
			Result<Scope<PhysicsWorld>> noBodies = PhysicsWorld::Create({ .Limits = { .MaxBodies = 0 } });
			REQUIRE_FALSE(noBodies.has_value());
			CHECK(noBodies.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
