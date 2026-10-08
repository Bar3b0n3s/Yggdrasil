#include "TestsPCH.h"

#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Core/Hash.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <tuple>

// The physics world (Architecture §9.1, §9.3; §9.7: free fall vs analytic, rest height (0.48 with slop), restitution
// bounce, layer matrix, CCD prevents tunnelling at 200 m/s, and the world-level half of the determinism tests: a 200-body
// pile with 0, 1 and 8 Jolt threads, its committed final state, its contact records across runs; Roadmap M11 acceptance).
// ECS-agnostic: bodies are created directly. Physics: CCD prevents tunnelling at 200 m/s is a Roadmap acceptance name, used
// verbatim; the session-level determinism acceptance tests are in Tests/Source/Engine/Session/PhysicsDeterminismTests.cpp.

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

		// The first record of `kind` between `a` and `b`, in either order, or nullopt.
		std::optional<ContactEvent> FindContact(const std::vector<ContactEvent>& contacts, ContactEventKind kind, BodyHandle a, BodyHandle b)
		{
			const auto found = std::find_if(contacts.begin(), contacts.end(), [kind, a, b](const ContactEvent& event)
			{
				return event.Kind == kind && ((event.BodyA == a && event.BodyB == b) || (event.BodyA == b && event.BodyB == a));
			});
			if (found == contacts.end())
				return std::nullopt;
			return *found;
		}

		// The world-level pile's final hash after WorldPileSteps steps (RunWorldPile), recorded by the M11 integration from the
		// merged implementation (ADR 0014 decision 33). It is the same in Debug, Release and Dist and for every Jolt thread
		// count (§9.1: a result that depends on the configuration is a bug); a change of Jolt or of the pile that legitimately
		// changes the simulation updates it with the reason in the change's review.
		constexpr uint64_t CommittedWorldPileHash = 0x60b4a8276c63dfb3;

		// Two seconds: the layers land on each other and the pile topples with hundreds of contacts busy, which is where
		// thread counts or configurations would make results diverge. The session-level acceptance tests step their pile the
		// full 600 ticks of §9.7 (PhysicsDeterminismTests.cpp); seven world-level runs of that length would take minutes in
		// Debug.
		constexpr uint32_t WorldPileSteps = 120;

		// 200 boxes of three sizes in a 5 x 5 footprint, 8 layers high, each layer offset and rotated a little so the pile
		// topples and keeps many contacts busy, over the ground; positions are exact binary fractions. The world-level twin of
		// the session-level pile of PhysicsDeterminismTests.cpp, built in creation (= canonical) order.
		std::vector<BodyHandle> BuildWorldPile(PhysicsWorld& world)
		{
			static_cast<void>(CreateGround(world));
			std::vector<BodyHandle> boxes;
			for (int layer = 0; layer < 8; ++layer)
			{
				for (int row = 0; row < 5; ++row)
				{
					for (int column = 0; column < 5; ++column)
					{
						const float halfExtent = 0.25f + 0.125f * static_cast<float>((row + column + layer) % 3);
						const glm::vec3 position(static_cast<float>(column) * 1.25f + 0.0625f * static_cast<float>(layer % 2),
							0.5f + static_cast<float>(layer) * 1.25f, static_cast<float>(row) * 1.25f - 0.0625f * static_cast<float>(layer % 3));
						BodyDescription description;
						description.Pose.Rotation = glm::normalize(glm::quat(1.0f, 0.0f, 0.125f * static_cast<float>(layer % 4), 0.0f));
						description.UserData = boxes.size();
						boxes.push_back(CreateDynamic(world, BoxShapeGeometry{ .HalfExtents = glm::vec3(halfExtent) }, position, description));
					}
				}
			}
			return boxes;
		}

		// XXH64 of each body's pose and velocities (float bit patterns) and its sleeping flag, in the order given.
		uint64_t HashBodies(const PhysicsWorld& world, const std::vector<BodyHandle>& bodies)
		{
			XXH64Hasher hasher(0);
			const auto appendVector = [&hasher](const glm::vec3& vector)
			{
				hasher.UpdateU64(std::bit_cast<uint32_t>(vector.x));
				hasher.UpdateU64(std::bit_cast<uint32_t>(vector.y));
				hasher.UpdateU64(std::bit_cast<uint32_t>(vector.z));
			};
			for (const BodyHandle body : bodies)
			{
				const PhysicsPose pose = world.GetPose(body);
				appendVector(pose.Position);
				appendVector(glm::vec3(pose.Rotation.x, pose.Rotation.y, pose.Rotation.z));
				hasher.UpdateU64(std::bit_cast<uint32_t>(pose.Rotation.w));
				appendVector(world.GetLinearVelocity(body));
				appendVector(world.GetAngularVelocity(body));
				hasher.UpdateU64(world.IsSleeping(body) ? 1 : 0);
			}
			return hasher.Digest();
		}

		// Jolt reports a step's contacts in an order that depends on its threads (§9.4), but at most one record per pair,
		// sub-shapes and kind, so this order is total within one step's records.
		void SortStepContacts(std::vector<ContactEvent>& contacts)
		{
			std::sort(contacts.begin(), contacts.end(), [](const ContactEvent& a, const ContactEvent& b)
			{
				return std::tie(a.BodyA, a.BodyB, a.SubShapeA, a.SubShapeB, a.Kind) < std::tie(b.BodyA, b.BodyB, b.SubShapeA, b.SubShapeB, b.Kind);
			});
		}

		// The pile's hash after WorldPileSteps steps with `workerThreads` Jolt threads; `contacts`, when given, receives every
		// contact record, each step's sorted. It REQUIREs, so callers keep it out of CHECK expressions.
		uint64_t RunWorldPile(uint32_t workerThreads, std::vector<ContactEvent>* contacts = nullptr)
		{
			const Test::ScopedWorkerThreadCount threads(workerThreads);
			Scope<PhysicsWorld> world = CreateWorld();
			const std::vector<BodyHandle> boxes = BuildWorldPile(*world);
			for (uint32_t step = 0; step < WorldPileSteps; ++step)
			{
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				std::vector<ContactEvent> stepContacts = world->DrainContactEvents();
				if (contacts != nullptr)
				{
					SortStepContacts(stepContacts);
					contacts->insert(contacts->end(), stepContacts.begin(), stepContacts.end());
				}
			}
			return HashBodies(*world, boxes);
		}

	}

	ENGINE_DEATH_TEST("Physics/WorldDestroysDeadBody")
	{
		Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
		if (world.has_value())
			(*world)->DestroyBody(BodyHandle(12345));
	}

	ENGINE_DEATH_TEST("Physics/WorldPushesStaticBody")
	{
		Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
		Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{} } });
		if (!world.has_value() || !shape.has_value())
			return;
		Result<BodyHandle> body = (*world)->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static });
		if (body.has_value())
			(*world)->AddForce(*body, glm::vec3(1.0f, 0.0f, 0.0f));
	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsWorld: a falling sphere follows the analytic free fall")
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

		TEST_CASE("PhysicsWorld: a box comes to rest at its half height minus the penetration slop")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			const BodyHandle box = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 2.0f, 0.0f));
			StepWorld(*world, 300);
			// §9.1: resting contacts penetrate by mPenetrationSlop (0.02 m), so a 0.5 m half height rests at 0.48.
			CHECK(world->GetPose(box).Position.y == doctest::Approx(0.48f).epsilon(0.01));
			CHECK(world->IsSleeping(box));
		}

		TEST_CASE("PhysicsWorld: a restitution of 1 bounces a ball back near its drop height")
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

		TEST_CASE("Physics: CCD prevents tunnelling at 200 m/s")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			// A 5 cm wall; at 200 m/s a 10 cm ball moves 3.3 m per step. The balls start at x = 0.5, so the discrete one's
			// positions after each step (3.83, 7.17, 10.5, ...) never overlap the wall (x 9.975 to 10.025): a start at 0 would
			// put it at exactly 10 after the third step, inside the wall, where even discrete collision catches it.
			Result<BodyHandle> wall = world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(0.025f, 5.0f, 5.0f) }),
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = glm::vec3(10.0f, 0.0f, 0.0f) } });
			REQUIRE(wall.has_value());
			const auto launch = [&world](PhysicsMotionQuality quality, float z)
			{
				return CreateDynamic(*world, SphereShapeGeometry{ .Radius = 0.05f }, glm::vec3(0.5f, 0.0f, z),
					{ .MotionQuality = quality, .GravityFactor = 0.0f, .AllowSleeping = false, .LinearVelocity = glm::vec3(200.0f, 0.0f, 0.0f) });
			};
			const BodyHandle continuous = launch(PhysicsMotionQuality::LinearCast, -1.0f);
			const BodyHandle discrete = launch(PhysicsMotionQuality::Discrete, 1.0f);
			StepWorld(*world, 30);
			CHECK(world->GetPose(continuous).Position.x < 10.0f);
			// The control case: without CCD the same ball passes through.
			CHECK(world->GetPose(discrete).Position.x > 10.0f);
		}

		TEST_CASE("PhysicsWorld: layers the matrix does not pair pass through each other")
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

		TEST_CASE("PhysicsWorld: contacts are buffered with the colliders' user data, and sensors see sleeping bodies")
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

		TEST_CASE("PhysicsWorld: invalid bodies are refused with their codes and nothing reaches Jolt")
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

		TEST_CASE("PhysicsWorld: teleports, kinematic moves, forces and velocities act on the right bodies")
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

		TEST_CASE("PhysicsWorld: settings Jolt would assert on are made safe before they reach it")
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

		TEST_CASE("PhysicsWorld: a needle-thin rotated Dynamic body gets mass properties Jolt can decompose")
		{
			// A 2 mm thick, 4 m long rod, rotated within its body: its inertia has off-diagonal terms and a smallest principal
			// moment about a millionth of its largest, which Jolt's float eigen decomposition asserts on.
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			Result<Ref<const PhysicsShape>> rod = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{
																			 .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(0.001f, 2.0f, 0.001f) },
																			 .Rotation = glm::normalize(glm::quat(0.9f, 0.3f, 0.2f, 0.1f)),
																		 } } });
			REQUIRE_MESSAGE(rod.has_value(), rod.error().ToString());
			Result<BodyHandle> body = world->CreateBody({ .Shape = *rod, .MotionType = PhysicsMotionType::Dynamic, .Pose = { .Position = glm::vec3(0.0f, 3.0f, 0.0f) } });
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			// SetShape recomputes the inertia the same way.
			REQUIRE(world->SetShape(*body, *rod).has_value());
			world->AddTorque(*body, glm::vec3(0.0f, 5.0f, 0.0f));
			StepWorld(*world, 60);
			CHECK(world->GetPose(*body).Position.y < 3.0f);
		}

		TEST_CASE("PhysicsWorld: huge forces, torques and impulses on a tiny body leave its velocities finite and within its maxima")
		{
			// The body functions accept forces of 1e12 and lever arms of 1e9 m; on a 2 mm, 1 g sphere their velocity change
			// would overflow Jolt's squared lengths (it asserts on an infinite one) without the world's bounds.
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle grain = CreateDynamic(*world, SphereShapeGeometry{ .Radius = 0.001f }, glm::vec3(0.0f, 5.0f, 0.0f),
				{ .Mass = 0.001f, .MaxLinearVelocity = 500.0f, .MaxAngularVelocity = 100.0f, .AllowSleeping = false });
			const auto withinMaxima = [&world, grain]()
			{
				const glm::vec3 linear = world->GetLinearVelocity(grain);
				const glm::vec3 angular = world->GetAngularVelocity(grain);
				return std::isfinite(glm::length(linear)) && std::isfinite(glm::length(angular)) && glm::length(linear) <= 500.0f * 1.001f
					&& glm::length(angular) <= 100.0f * 1.001f;
			};
			world->AddTorque(grain, glm::vec3(1.0e12f, -1.0e12f, 1.0e12f));
			world->AddForceAtPosition(grain, glm::vec3(1.0e12f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0e9f, 1.0e9f));
			world->AddForce(grain, glm::vec3(0.0f, 1.0e12f, 0.0f));
			StepWorld(*world, 1);
			CHECK(withinMaxima());
			world->AddAngularImpulse(grain, glm::vec3(1.0e12f, 1.0e12f, -1.0e12f));
			CHECK(withinMaxima());
			world->AddImpulse(grain, glm::vec3(-1.0e12f, 0.0f, 1.0e12f));
			CHECK(withinMaxima());
			StepWorld(*world, 2);
			CHECK(withinMaxima());
		}

		TEST_CASE("PhysicsWorld: LinearCast falls back to Discrete for a shape without an inner radius, and extreme settings are bounded")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			// A Kinematic triangle mesh has no inner radius: a cast of it would assert in Jolt.
			Result<BodyHandle> mesh = world->CreateBody({ .Shape = CreateShape(MakeTriangle()),
				.MotionType = PhysicsMotionType::Kinematic,
				.MotionQuality = PhysicsMotionQuality::LinearCast });
			REQUIRE_MESSAGE(mesh.has_value(), mesh.error().ToString());
			world->MoveKinematic(*mesh, { .Position = glm::vec3(0.0f, 1.0f, 0.0f) }, FixedDelta);
			// A gravity factor and maximum velocities far beyond any game are bounded, not refused.
			const BodyHandle extreme = CreateDynamic(*world, SphereShapeGeometry{}, glm::vec3(5.0f, 0.0f, 0.0f),
				{ .GravityFactor = 1.0e30f, .MaxLinearVelocity = 1.0e30f, .MaxAngularVelocity = 1.0e30f, .LinearVelocity = glm::vec3(0.0f, 1.0e20f, 0.0f) });
			StepWorld(*world, 3);
			CHECK(std::isfinite(glm::length(world->GetLinearVelocity(extreme))));
			CHECK(glm::length(world->GetLinearVelocity(extreme)) <= 1.0e6f * 1.001f);
			// A position beyond MaxPhysicsCoordinate is refused.
			Result<BodyHandle> far = world->CreateBody({ .Shape = CreateShape(SphereShapeGeometry{}),
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = glm::vec3(0.0f, 0.0f, 2.0f * MaxPhysicsCoordinate) } });
			REQUIRE_FALSE(far.has_value());
			CHECK(far.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PhysicsWorld: bodies that share a collision group never collide")
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

		TEST_CASE("PhysicsWorld: a world needs the physics engine and valid settings")
		{
			Result<Scope<PhysicsWorld>> badGravity = PhysicsWorld::Create({ .Gravity = glm::vec3(std::numeric_limits<float>::quiet_NaN()) });
			REQUIRE_FALSE(badGravity.has_value());
			CHECK(badGravity.error().GetCode() == ErrorCode::InvalidArgument);
			Result<Scope<PhysicsWorld>> noBodies = PhysicsWorld::Create({ .Limits = { .MaxBodies = 0 } });
			REQUIRE_FALSE(noBodies.has_value());
			CHECK(noBodies.error().GetCode() == ErrorCode::InvalidArgument);
			for (const PhysicsWorldLimits& limits : { PhysicsWorldLimits{ .MaxBodyPairs = 0 }, PhysicsWorldLimits{ .MaxContactConstraints = 0 },
					 PhysicsWorldLimits{ .TempAllocatorBytes = 0 }, PhysicsWorldLimits{ .MaxBodies = 0x800001 } })
			{
				Result<Scope<PhysicsWorld>> refused = PhysicsWorld::Create({ .Limits = limits });
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("PhysicsWorld: a 200-body pile ends in the same state with 0, 1 and 8 Jolt threads")
		{
			const uint64_t none = RunWorldPile(0);
			const uint64_t one = RunWorldPile(1);
			const uint64_t eight = RunWorldPile(8);
			CHECK(one == none);
			CHECK(eight == none);
		}

		TEST_CASE("PhysicsWorld: a 200-body pile ends in the committed state in every configuration")
		{
			const uint64_t hash = RunWorldPile(PhysicsEngine::GetWorkerThreadCount());
			INFO("world pile hash: ", std::format("0x{:016x}", hash));
			CHECK(hash == CommittedWorldPileHash);
		}

		TEST_CASE("PhysicsWorld: a pile's contact records are identical across runs and thread counts once sorted")
		{
			std::vector<ContactEvent> first;
			std::vector<ContactEvent> second;
			std::vector<ContactEvent> threaded;
			const uint64_t firstHash = RunWorldPile(1, &first);
			const uint64_t secondHash = RunWorldPile(1, &second);
			static_cast<void>(RunWorldPile(8, &threaded));
			REQUIRE(first.size() > 200);
			CHECK(second == first);
			CHECK(threaded == first);
			// Repeated runs end in the same state too.
			CHECK(secondHash == firstHash);
		}

		TEST_CASE("PhysicsWorld: contacts name a compound sub-shape's collider and a single collider's user data")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			// A static track of three 1 m pieces whose collider indices are 10, 11 and 12 (§9.2: the sub-shape user data).
			BodyShapeDescription track;
			for (uint32_t piece = 0; piece < 3; ++piece)
			{
				track.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) },
					.Position = glm::vec3(static_cast<float>(piece), -0.5f, 0.0f),
					.UserData = 10 + piece });
			}
			Result<Ref<const PhysicsShape>> trackShape = PhysicsShape::Create(track);
			REQUIRE(trackShape.has_value());
			Result<BodyHandle> trackBody = world->CreateBody({ .Shape = *trackShape, .MotionType = PhysicsMotionType::Static });
			REQUIRE(trackBody.has_value());
			// A box whose one collider has index 7, dropped onto the last piece.
			Result<Ref<const PhysicsShape>> boxShape = PhysicsShape::Create(
				{ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(0.25f) }, .UserData = 7 } } });
			REQUIRE(boxShape.has_value());
			Result<BodyHandle> box = world->CreateBody({ .Shape = *boxShape, .Pose = { .Position = glm::vec3(2.0f, 0.5f, 0.0f) } });
			REQUIRE(box.has_value());
			StepWorld(*world, 30);

			const std::optional<ContactEvent> added = FindContact(world->DrainContactEvents(), ContactEventKind::Added, *trackBody, *box);
			REQUIRE(added.has_value());
			const bool trackIsA = added->BodyA == *trackBody;
			CHECK((trackIsA ? added->ColliderA : added->ColliderB) == 12);
			CHECK((trackIsA ? added->ColliderB : added->ColliderA) == 7);
			// The normal points from A towards B, and A approached B as the box fell onto the track.
			CHECK(std::abs(added->Normal.y) == doctest::Approx(1.0f).epsilon(1.0e-3));
			CHECK(added->Normal.y * (trackIsA ? 1.0f : -1.0f) > 0.0f);
			CHECK(added->RelativeNormalSpeed >= 0.0f);
			CHECK(added->Point.x == doctest::Approx(2.0f).epsilon(0.15));
		}

		TEST_CASE("PhysicsWorld: a contact that ends is reported removed with its sub-shapes, a sensor's as a trigger")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ground = CreateGround(*world);
			const BodyHandle box = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 0.5f, 0.0f),
				{ .AllowSleeping = false });
			Result<BodyHandle> sensor = world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(1.0f) }),
				.MotionType = PhysicsMotionType::Kinematic,
				.Pose = { .Position = glm::vec3(0.0f, 1.0f, 0.0f) },
				.IsSensor = true,
				.CollideKinematicVsNonDynamic = true });
			REQUIRE(sensor.has_value());
			StepWorld(*world, 2);
			const std::vector<ContactEvent> began = world->DrainContactEvents();
			const std::optional<ContactEvent> onGround = FindContact(began, ContactEventKind::Added, ground, box);
			const std::optional<ContactEvent> inSensor = FindContact(began, ContactEventKind::Added, *sensor, box);
			REQUIRE(onGround.has_value());
			REQUIRE(inSensor.has_value());
			CHECK(inSensor->IsSensor);

			// Teleported far away: Jolt's next step reports both contacts as ended.
			world->SetPose(box, { .Position = glm::vec3(30.0f, 20.0f, 0.0f) }, true);
			StepWorld(*world, 1);
			const std::vector<ContactEvent> ended = world->DrainContactEvents();
			const std::optional<ContactEvent> leftGround = FindContact(ended, ContactEventKind::Removed, ground, box);
			const std::optional<ContactEvent> leftSensor = FindContact(ended, ContactEventKind::Removed, *sensor, box);
			REQUIRE(leftGround.has_value());
			REQUIRE(leftSensor.has_value());
			CHECK(leftGround->BodyA == onGround->BodyA);
			CHECK(leftGround->SubShapeA == onGround->SubShapeA);
			CHECK(leftGround->SubShapeB == onGround->SubShapeB);
			CHECK_FALSE(leftGround->IsSensor);
			CHECK(leftSensor->IsSensor);
		}

		TEST_CASE("PhysicsWorld: sensors never pair with static bodies or other sensors")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ground = CreateGround(*world);
			const auto createSensor = [&world](const glm::vec3& position)
			{
				return world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(1.0f) }),
					.MotionType = PhysicsMotionType::Kinematic,
					.Pose = { .Position = position },
					.IsSensor = true,
					.CollideKinematicVsNonDynamic = true });
			};
			// Two overlapping sensors that also overlap the static ground.
			Result<BodyHandle> first = createSensor(glm::vec3(0.0f));
			Result<BodyHandle> second = createSensor(glm::vec3(0.5f, 0.0f, 0.0f));
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			StepWorld(*world, 2);
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			CHECK_FALSE(HasAddedContact(contacts, *first, ground));
			CHECK_FALSE(HasAddedContact(contacts, *second, ground));
			CHECK_FALSE(HasAddedContact(contacts, *first, *second));
		}

		TEST_CASE("PhysicsWorld: SetShape keeps a Dynamic body's handle, velocity and mass")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle body = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 10.0f, 0.0f),
				{ .Mass = 2.0f, .GravityFactor = 0.0f, .LinearVelocity = glm::vec3(1.0f, 0.0f, 0.0f) });
			const Ref<const PhysicsShape> sphere = CreateShape(SphereShapeGeometry{ .Radius = 1.0f });
			REQUIRE(world->SetShape(body, sphere).has_value());
			CHECK(world->IsBodyValid(body));
			CHECK(world->GetShape(body) == sphere);
			CHECK(world->GetLinearVelocity(body).x == doctest::Approx(1.0f));
			// Still 2 kg: an impulse of 4 N s changes the velocity by 2 m/s (the sphere's own mass would be about 4189 kg).
			world->AddImpulse(body, glm::vec3(0.0f, 4.0f, 0.0f));
			CHECK(world->GetLinearVelocity(body).y == doctest::Approx(2.0f));

			const Status mesh = world->SetShape(body, CreateShape(MakeTriangle()));
			REQUIRE_FALSE(mesh.has_value());
			CHECK(GetPhysicsDiagnosticCode(mesh.error()) == PhysicsNonconvexDynamicCode);
			CHECK(world->GetShape(body) == sphere);
			const Status missing = world->SetShape(body, nullptr);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::InvalidArgument);
			const Status dead = world->SetShape(BodyHandle(12345), sphere);
			REQUIRE_FALSE(dead.has_value());
			CHECK(dead.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// The two cases below record how Jolt reports contacts that end without the bodies separating, which the consumer of the
		// records (Scene/PhysicsSystem, §9.4) has to tell apart from real exits.
		TEST_CASE("PhysicsWorld: a body that falls asleep has its contacts reported removed, and added again when it wakes")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ground = CreateGround(*world);
			const BodyHandle box = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 0.5f, 0.0f));
			std::vector<ContactEvent> contacts;
			for (uint32_t step = 0; step < 300; ++step)
			{
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				const std::vector<ContactEvent> stepContacts = world->DrainContactEvents();
				contacts.insert(contacts.end(), stepContacts.begin(), stepContacts.end());
			}
			REQUIRE(world->IsSleeping(box));
			CHECK(FindContact(contacts, ContactEventKind::Added, ground, box).has_value());
			CHECK(FindContact(contacts, ContactEventKind::Removed, ground, box).has_value());
			world->WakeUp(box);
			StepWorld(*world, 1);
			CHECK(FindContact(world->DrainContactEvents(), ContactEventKind::Added, ground, box).has_value());
		}

		TEST_CASE("PhysicsWorld: a destroyed body's contacts are reported removed at the next step, with its stale handle")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ground = CreateGround(*world);
			const BodyHandle box = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 0.5f, 0.0f),
				{ .AllowSleeping = false });
			StepWorld(*world, 2);
			REQUIRE(FindContact(world->DrainContactEvents(), ContactEventKind::Added, ground, box).has_value());
			world->DestroyBody(box);
			StepWorld(*world, 1);
			const std::vector<ContactEvent> ended = world->DrainContactEvents();
			INFO("records after the destroy: ", ended.size());
			CHECK(FindContact(ended, ContactEventKind::Removed, ground, box).has_value());
		}

		TEST_CASE("PhysicsWorld: destroying a body wakes the bodies that slept on it")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle ground = CreateGround(*world);
			const BodyHandle box = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 0.5f, 0.0f));
			StepWorld(*world, 300);
			REQUIRE(world->IsSleeping(box));
			world->DestroyBody(ground);
			CHECK_FALSE(world->IsBodyValid(ground));
			CHECK_FALSE(world->IsSleeping(box));
			StepWorld(*world, 30);
			CHECK(world->GetPose(box).Position.y < 0.0f);
		}

		TEST_CASE("PhysicsWorld: stats count the bodies, the active ones and the last step's contacts")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			const BodyHandle left = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(-2.0f, 0.5f, 0.0f),
				{ .AllowSleeping = false });
			static_cast<void>(CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(2.0f, 0.5f, 0.0f), { .AllowSleeping = false }));
			StepWorld(*world, 5);
			const PhysicsWorldStats stats = world->GetStats();
			CHECK(stats.BodyCount == 3);
			CHECK(stats.ActiveBodyCount == 2);
			CHECK(stats.BodyPairCount == 2);
			CHECK(stats.ContactConstraintCount >= 2);
			// A teleported box no longer touches anything.
			world->SetPose(left, { .Position = glm::vec3(-2.0f, 20.0f, 0.0f) }, true);
			StepWorld(*world, 1);
			CHECK(world->GetStats().BodyPairCount == 1);
		}

		TEST_CASE("PhysicsWorld: approaching a limit logs one PHYSICS_LIMIT_EXCEEDED warning per limit")
		{
			// One worker-less job finds every pair, so Jolt's small caches fill the same way every run.
			const Test::ScopedWorkerThreadCount threads(0);
			const Test::ExpectLog bodies(LogLevel::Warn, "PHYSICS_LIMIT_EXCEEDED: the physics world reached 9 bodies of its limit of 10");
			const Test::ExpectLog bodyPairs(LogLevel::Warn, "PHYSICS_LIMIT_EXCEEDED: the physics world reached 9 body pairs in contact of its limit of 10");
			Scope<PhysicsWorld> world = CreateWorld({ .Limits = { .MaxBodies = 10, .MaxBodyPairs = 10 } });
			static_cast<void>(CreateGround(*world));
			// Nine boxes 2 m apart on the ground: nine body pairs, none between boxes.
			for (int box = 0; box < 9; ++box)
			{
				static_cast<void>(CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) },
					glm::vec3(static_cast<float>(box) * 2.0f, 0.5f, 0.0f), { .AllowSleeping = false }));
			}
			StepWorld(*world, 3);
			CHECK(world->GetStats().BodyPairCount == 9);
			CHECK(bodies.GetMatchCount() == 1);
			CHECK(bodyPairs.GetMatchCount() == 1);
		}

		TEST_CASE("PhysicsWorld: a step beyond the contact limits reports PHYSICS_LIMIT_EXCEEDED and the world keeps running")
		{
			Scope<PhysicsWorld> world = CreateWorld({ .Limits = { .MaxBodyPairs = 2, .MaxContactConstraints = 2 } });
			static_cast<void>(CreateGround(*world));
			std::vector<BodyHandle> boxes;
			for (int box = 0; box < 6; ++box)
			{
				boxes.push_back(CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) },
					glm::vec3(static_cast<float>(box) * 2.0f, 0.5f, 0.0f), { .AllowSleeping = false }));
			}
			for (int step = 0; step < 3; ++step)
			{
				const Status stepped = world->Step(FixedDelta, 1);
				REQUIRE_FALSE(stepped.has_value());
				CHECK(stepped.error().GetCode() == ErrorCode::InvalidState);
				CHECK(GetPhysicsDiagnosticCode(stepped.error()) == PhysicsLimitExceededCode);
			}
			CHECK(world->IsBodyValid(boxes.back()));
			CHECK(world->GetStats().BodyCount == 7);
		}

		TEST_CASE("PhysicsWorld: a handle of no live body or of the wrong motion type is a programmer error that asserts")
		{
			// Callers validate their input first (PhysicsWorld.h); a release build without asserts ignores the call.
			ENGINE_CHECK_DEATH("Physics/WorldDestroysDeadBody", "names no live body of this world");
			ENGINE_CHECK_DEATH("Physics/WorldPushesStaticBody", "PhysicsWorld::AddForce does not apply to a Static body");
		}

		TEST_CASE("PhysicsWorld: bodies, user data and layers are kept as described")
		{
			const std::vector<std::string> layers = { "Default", "Ball" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Ball" } };
			Result<PhysicsLayerTable> table = PhysicsLayerTable::Create(layers, collisions);
			REQUIRE(table.has_value());
			Scope<PhysicsWorld> world = CreateWorld({ .Gravity = glm::vec3(0.0f, -3.0f, 0.0f), .Layers = *table });
			CHECK(world->GetLayers().GetLayerCount() == 2);
			CHECK(Test::ApproxEqual(world->GetGravity(), glm::vec3(0.0f, -3.0f, 0.0f)));
			CHECK(world->GetLimits().MaxBodies == PhysicsWorldLimits::DefaultMaxBodies);
			const Ref<const PhysicsShape> shape = CreateShape(SphereShapeGeometry{});
			Result<BodyHandle> ball = world->CreateBody({ .Shape = shape, .Layer = 1, .UserData = 0x1234567890abcdefu });
			REQUIRE(ball.has_value());
			CHECK(world->GetUserData(*ball) == 0x1234567890abcdefu);
			CHECK(world->GetLayer(*ball) == 1);
			CHECK(world->GetMotionType(*ball) == PhysicsMotionType::Dynamic);
			CHECK_FALSE(world->IsSensor(*ball));
			CHECK(world->GetShape(*ball) == shape);
			CHECK_FALSE(world->IsSleeping(*ball));
			// A layer outside the table is an argument error.
			Result<BodyHandle> unknownLayer = world->CreateBody({ .Shape = shape, .Layer = 2 });
			REQUIRE_FALSE(unknownLayer.has_value());
			CHECK(unknownLayer.error().GetCode() == ErrorCode::InvalidArgument);
			// A destroyed body's handle stays invalid when its index is reused.
			world->DestroyBody(*ball);
			CHECK_FALSE(world->IsBodyValid(*ball));
			Result<BodyHandle> reused = world->CreateBody({ .Shape = shape });
			REQUIRE(reused.has_value());
			CHECK(*reused != *ball);
			CHECK_FALSE(world->IsBodyValid(*ball));
			CHECK_FALSE(world->IsBodyValid(BodyHandle{}));
			CHECK_FALSE(world->IsBodyValid(BodyHandle(0x80000001u)));
		}

		TEST_CASE("PhysicsWorld: a Dynamic body that can only rotate needs rotational inertia, and the world wakes and sleeps bodies")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const BodyHandle spinner = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 5.0f, 0.0f),
				{ .AllowedDofs = PhysicsDofs::RotationY });
			world->AddAngularImpulse(spinner, glm::vec3(0.0f, 10.0f, 0.0f));
			world->AddTorque(spinner, glm::vec3(0.0f, 1.0f, 0.0f));
			StepWorld(*world, 10);
			// Gravity cannot move it; it only turns about Y.
			CHECK(world->GetPose(spinner).Position.y == doctest::Approx(5.0f));
			CHECK(world->GetAngularVelocity(spinner).y > 0.0f);
			CHECK(world->GetLinearVelocity(spinner) == glm::vec3(0.0f));

			const BodyHandle pushed = CreateDynamic(*world, SphereShapeGeometry{}, glm::vec3(10.0f, 5.0f, 0.0f), { .GravityFactor = 0.0f });
			world->AddForce(pushed, glm::vec3(60.0f, 0.0f, 0.0f));
			world->AddForceAtPosition(pushed, glm::vec3(0.0f, 0.0f, 60.0f), glm::vec3(10.0f, 5.5f, 0.0f));
			StepWorld(*world, 1);
			CHECK(world->GetLinearVelocity(pushed).x > 0.0f);
			CHECK(world->GetLinearVelocity(pushed).z > 0.0f);
			CHECK(world->GetAngularVelocity(pushed) != glm::vec3(0.0f));
			world->SetLinearVelocity(pushed, glm::vec3(0.0f));
			world->SetAngularVelocity(pushed, glm::vec3(0.0f));
			StepWorld(*world, 120);
			CHECK(world->IsSleeping(pushed));
			world->WakeUp(pushed);
			CHECK_FALSE(world->IsSleeping(pushed));
		}

		TEST_CASE("PhysicsWorld: a Dynamic body of the largest collider gets mass properties Jolt can use, and a larger collider is refused")
		{
			// A Dynamic box of half extent 1e7 m made Jolt assert in its inertia decomposition (its inertia overflowed a float);
			// such a collider is now refused before Jolt sees it, and the largest one Create takes simulates (ADR 0014 decision
			// 34).
			Scope<PhysicsWorld> world = CreateWorld();
			const Result<Ref<const PhysicsShape>> refused = PhysicsShape::Create(
				{ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(1.0e7f) } } } });
			REQUIRE_FALSE(refused.has_value());
			CHECK(GetPhysicsDiagnosticCode(refused.error()) == PhysicsInvalidShapeCode);
			static_cast<void>(CreateGround(*world));
			const BodyHandle largest = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(PhysicsShape::MaxColliderSize / 2.0f) },
				glm::vec3(0.0f, PhysicsShape::MaxColliderSize, 0.0f));
			const BodyHandle pebble = CreateDynamic(*world, SphereShapeGeometry{ .Radius = 0.05f }, glm::vec3(10.0f, 1.0f, 0.0f));
			StepWorld(*world, 2);
			for (const BodyHandle body : { largest, pebble })
			{
				CHECK(std::isfinite(glm::length(world->GetLinearVelocity(body))));
				CHECK(std::isfinite(glm::length(world->GetAngularVelocity(body))));
			}
		}

		TEST_CASE("PhysicsWorld: a Dynamic body's Mass is at most MaxPhysicsMass and its inertia at most what Jolt can decompose")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			const Ref<const PhysicsShape> crate = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(3.0f) });
			// The mass that overflowed the inertia of a 6 m box (and made Jolt assert) is refused, as is anything above the limit.
			for (const float mass : { 3.0e38f, 2.0f * MaxPhysicsMass })
			{
				CAPTURE(mass);
				const Result<BodyHandle> heavy = world->CreateBody({ .Shape = crate, .MotionType = PhysicsMotionType::Dynamic, .Mass = mass });
				REQUIRE_FALSE(heavy.has_value());
				CHECK(heavy.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(crate->CheckDynamicBody(mass, PhysicsDofs::All).error().GetCode() == ErrorCode::InvalidArgument);
			}
			const Result<BodyHandle> heaviest = world->CreateBody({ .Shape = crate, .MotionType = PhysicsMotionType::Dynamic, .Mass = MaxPhysicsMass });
			REQUIRE(heaviest.has_value());
			StepWorld(*world, 2);
			CHECK(std::isfinite(world->GetPose(*heaviest).Position.y));

			// MaxPhysicsMass in a box 90 km across has an inertia beyond 1e18 kg m^2: refused at creation and as a new shape.
			const Ref<const PhysicsShape> vast = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(4.5e4f) });
			const Result<BodyHandle> refused = world->CreateBody({ .Shape = vast, .MotionType = PhysicsMotionType::Dynamic, .Mass = MaxPhysicsMass });
			REQUIRE_FALSE(refused.has_value());
			CHECK(GetPhysicsDiagnosticCode(refused.error()) == PhysicsInvalidShapeCode);
			const Status reshaped = world->SetShape(*heaviest, vast);
			REQUIRE_FALSE(reshaped.has_value());
			CHECK(GetPhysicsDiagnosticCode(reshaped.error()) == PhysicsInvalidShapeCode);
			CHECK(world->GetShape(*heaviest) == crate);
			// The same box at 1 kg is fine, and every shape's rule answers as the world does.
			CHECK(vast->CheckDynamicBody(1.0f, PhysicsDofs::All).has_value());
			CHECK(GetPhysicsDiagnosticCode(vast->CheckDynamicBody(MaxPhysicsMass, PhysicsDofs::All).error()) == PhysicsInvalidShapeCode);
			CHECK(GetPhysicsDiagnosticCode(crate->CheckDynamicBody(1.0f, PhysicsDofs::None).error()) == PhysicsAllDofsLockedCode);
			CHECK(GetPhysicsDiagnosticCode(CreateShape(MakeTriangle())->CheckDynamicBody(1.0f, PhysicsDofs::All).error()) == PhysicsNonconvexDynamicCode);
		}

		TEST_CASE("PhysicsWorld: a compound of the largest colliders across the range has a finite centre of mass and steps")
		{
			// Jolt sums the colliders' masses times their offsets (a compound's centre of mass) and times the squares of their
			// offsets (its inertia); two boxes of half extent 5e8 m at +-9e8 m overflowed the centre of mass at its default
			// density and the broad phase asserted on the infinite bounds. Such boxes are now too large (MaxColliderSize); the
			// largest ones at the ends of the range give a finite centre of mass, bounds and inertia (ADR 0014 decision 34).
			Scope<PhysicsWorld> world = CreateWorld();
			BodyShapeDescription huge;
			huge.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(5.0e8f) } });
			const Result<Ref<const PhysicsShape>> refused = PhysicsShape::Create(huge);
			REQUIRE_FALSE(refused.has_value());
			CHECK(GetPhysicsDiagnosticCode(refused.error()) == PhysicsInvalidShapeCode);

			BodyShapeDescription level;
			level.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(1.0f) } });
			for (const float x : { -0.9f * MaxPhysicsCoordinate, 0.9f * MaxPhysicsCoordinate })
			{
				level.Colliders.push_back(ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(PhysicsShape::MaxColliderSize / 2.0f) },
					.Position = glm::vec3(x, 0.0f, 0.0f),
					.UserData = static_cast<uint32_t>(level.Colliders.size()) });
			}
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create(level);
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			const Aabb bounds = (*shape)->GetLocalBounds();
			CHECK(bounds.Min.x == doctest::Approx(-0.9f * MaxPhysicsCoordinate - PhysicsShape::MaxColliderSize / 2.0f));
			CHECK(bounds.Max.x == doctest::Approx(0.9f * MaxPhysicsCoordinate + PhysicsShape::MaxColliderSize / 2.0f));
			// Static, it holds a ball; Dynamic at 1 kg, its inertia is finite and within what Jolt decomposes.
			Result<BodyHandle> body = world->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static });
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			const BodyHandle ball = CreateDynamic(*world, SphereShapeGeometry{}, glm::vec3(0.0f, 10.0f, 0.0f));
			StepWorld(*world, 5);
			CHECK(std::isfinite(world->GetPose(ball).Position.y));
			CHECK((*shape)->CheckDynamicBody(1.0f, PhysicsDofs::All).has_value());
		}

		TEST_CASE("PhysicsWorld: a gravity component beyond MaxPhysicsGravity is refused, and the largest one steps without overflow")
		{
			for (const glm::vec3& gravity : { glm::vec3(0.0f, -1.0e37f, 0.0f), glm::vec3(2.0f * MaxPhysicsGravity, 0.0f, 0.0f) })
			{
				Result<Scope<PhysicsWorld>> refused = PhysicsWorld::Create({ .Gravity = gravity });
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			Scope<PhysicsWorld> world = CreateWorld({ .Gravity = glm::vec3(0.0f, -MaxPhysicsGravity, 0.0f) });
			const Status tooStrong = world->SetGravity(glm::vec3(0.0f, 0.0f, -1.0e13f));
			REQUIRE_FALSE(tooStrong.has_value());
			CHECK(tooStrong.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(world->GetGravity() == glm::vec3(0.0f, -MaxPhysicsGravity, 0.0f));
			// The smallest body at the largest gravity factor over a whole second: the velocity change stays finite, and the body
			// ends at its maximum velocity.
			const BodyHandle grain = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(PhysicsShape::MinColliderSize / 2.0f) }, glm::vec3(0.0f),
				{ .Mass = 0.001f, .GravityFactor = 1.0e6f, .MaxLinearVelocity = 1.0e9f });
			REQUIRE(world->Step(1.0f, 1).has_value());
			const glm::vec3 velocity = world->GetLinearVelocity(grain);
			CHECK(std::isfinite(glm::length(velocity)));
			CHECK(velocity.y < 0.0f);
		}

		TEST_CASE("PhysicsWorld: a new Static body wakes the sleeping bodies it overlaps")
		{
			Scope<PhysicsWorld> world = CreateWorld();
			static_cast<void>(CreateGround(*world));
			const BodyHandle box = CreateDynamic(*world, BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f) }, glm::vec3(0.0f, 0.5f, 0.0f));
			StepWorld(*world, 240);
			REQUIRE(world->IsSleeping(box));
			// A wall appears overlapping the sleeping box: the box wakes and is pushed out.
			Result<BodyHandle> wall = world->CreateBody({ .Shape = CreateShape(BoxShapeGeometry{ .HalfExtents = glm::vec3(0.5f, 2.0f, 2.0f) }),
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = glm::vec3(0.8f, 2.0f, 0.0f) } });
			REQUIRE(wall.has_value());
			CHECK_FALSE(world->IsSleeping(box));
			StepWorld(*world, 30);
			CHECK(world->GetPose(box).Position.x < -0.1f);
		}
	}

}
