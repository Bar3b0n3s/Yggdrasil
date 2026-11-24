#include "TestsPCH.h"

#include "Engine/Physics/CharacterController.h"

#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// The character controller (Architecture §9.6: CharacterVirtual with ExtendedUpdate, a kinematic inner body seen by
// queries and sensors, a CharacterContactListener recording into the world's buffer).

namespace Engine {

	namespace {

		constexpr float FixedDelta = 1.0f / 60.0f;
		const glm::vec3 Gravity(0.0f, -9.81f, 0.0f);

		Scope<PhysicsWorld> CreateCharacterWorld()
		{
			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			return std::move(*world);
		}

		// A static box with its top face centred at `top`.
		BodyHandle CreateBlock(PhysicsWorld& world, const glm::vec3& top, const glm::vec3& halfExtents, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = halfExtents } } } });
			REQUIRE(shape.has_value());
			Result<BodyHandle> body = world.CreateBody({ .Shape = *shape,
				.MotionType = PhysicsMotionType::Static,
				.Pose = { .Position = top - rotation * glm::vec3(0.0f, halfExtents.y, 0.0f), .Rotation = rotation } });
			REQUIRE(body.has_value());
			return *body;
		}

		Scope<CharacterController> CreateCharacter(PhysicsWorld& world, const glm::vec3& base)
		{
			Result<Scope<CharacterController>> character = CharacterController::Create(world, { .Pose = { .Position = base }, .UserData = 77 });
			REQUIRE_MESSAGE(character.has_value(), character.error().ToString());
			return std::move(*character);
		}

		void Walk(PhysicsWorld& world, CharacterController& character, const glm::vec3& velocity, uint32_t steps)
		{
			for (uint32_t step = 0; step < steps; ++step)
			{
				character.Update(FixedDelta, velocity, Gravity);
				REQUIRE(world.Step(FixedDelta, 1).has_value());
			}
		}

		// A kinematic platform, 40 m square with its top at y 0, moved at a constant velocity by MoveKinematic, as
		// Scene/PhysicsSystem moves a kinematic body towards its entity's pose in PreStep, before the characters update.
		class MovingPlatform
		{
		public:
			MovingPlatform(PhysicsWorld& world, const glm::vec3& velocity)
				: m_World(&world), m_Velocity(velocity)
			{
				Result<Ref<const PhysicsShape>> shape =
					PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(20.0f, 0.5f, 20.0f) } } } });
				REQUIRE(shape.has_value());
				Result<BodyHandle> body = world.CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Kinematic, .Pose = m_Pose });
				REQUIRE(body.has_value());
				m_Body = *body;
			}

			// One step: the platform starts its move, the character updates with `desired`, then the world steps.
			void Step(CharacterController& character, const glm::vec3& desired)
			{
				m_Pose.Position += m_Velocity * FixedDelta;
				m_World->MoveKinematic(m_Body, m_Pose, FixedDelta);
				character.Update(FixedDelta, desired, Gravity);
				REQUIRE(m_World->Step(FixedDelta, 1).has_value());
			}
		private:
			// A documented back-reference: the world outlives the platform (both live in one test case).
			PhysicsWorld* m_World = nullptr;
			glm::vec3 m_Velocity = glm::vec3(0.0f);
			PhysicsPose m_Pose{ .Position = glm::vec3(0.0f, -0.5f, 0.0f) };
			BodyHandle m_Body{};
		};

		// A Dynamic box of `halfExtents` and `mass` kg resting on a floor whose top is at y 0, its base at `base`.
		BodyHandle CreateCrate(PhysicsWorld& world, const glm::vec3& base, const glm::vec3& halfExtents, float mass, float restitution = 0.0f)
		{
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = halfExtents } } } });
			REQUIRE(shape.has_value());
			Result<BodyHandle> body = world.CreateBody({ .Shape = *shape,
				.MotionType = PhysicsMotionType::Dynamic,
				.Pose = { .Position = base + glm::vec3(0.0f, halfExtents.y, 0.0f) },
				.Mass = mass,
				.Restitution = restitution });
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			return *body;
		}

		// A character of World Tree's Warden (brief 11): 1.8 m tall, 0.35 m in radius, 80 kg, stepping up 0.35 m.
		Scope<CharacterController> CreateWarden(PhysicsWorld& world, const glm::vec3& base)
		{
			Result<Scope<CharacterController>> character =
				CharacterController::Create(world, { .Height = 1.8f, .Radius = 0.35f, .StepHeight = 0.35f, .Mass = 80.0f, .Pose = { .Position = base } });
			REQUIRE_MESSAGE(character.has_value(), character.error().ToString());
			return std::move(*character);
		}

		// M36: a static compound of two slabs 4 m wide and 20 m long side by side, their tops at y 0, of friction `friction`:
		// the collider whose user data is `left` at x in [-4, 0] and the one whose user data is `right` at x in [0, 4].
		BodyHandle CreateSlabs(PhysicsWorld& world, uint32_t left, uint32_t right, float friction = 0.5f)
		{
			const BoxShapeGeometry slab{ .HalfExtents = glm::vec3(2.0f, 0.5f, 10.0f) };
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = {
																			   ColliderShapeDescription{ .Geometry = slab, .Position = glm::vec3(-2.0f, -0.5f, 0.0f), .UserData = left },
																			   ColliderShapeDescription{ .Geometry = slab, .Position = glm::vec3(2.0f, -0.5f, 0.0f), .UserData = right } } });
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			Result<BodyHandle> body = world.CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static, .Friction = friction });
			REQUIRE(body.has_value());
			return *body;
		}

		// M36 W36.2: a Kinematic box moved at a constant velocity by MoveKinematic, as Scene/PhysicsSystem moves a Kinematic
		// body in PreStep before the characters update: a piston, a press or a ram.
		class KinematicBox
		{
		public:
			KinematicBox(PhysicsWorld& world, const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec3& velocity)
				: m_World(&world), m_Velocity(velocity), m_Pose{ .Position = center }
			{
				Result<Ref<const PhysicsShape>> shape =
					PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = halfExtents } } } });
				REQUIRE(shape.has_value());
				Result<BodyHandle> body = world.CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Kinematic, .Pose = m_Pose });
				REQUIRE(body.has_value());
				m_Body = *body;
			}

			// One step: the box starts its move, the character updates with `desired`, then the world steps.
			void Step(CharacterController& character, const glm::vec3& desired = glm::vec3(0.0f))
			{
				m_Pose.Position += m_Velocity * FixedDelta;
				m_World->MoveKinematic(m_Body, m_Pose, FixedDelta);
				character.Update(FixedDelta, desired, Gravity);
				REQUIRE(m_World->Step(FixedDelta, 1).has_value());
			}

			void SetVelocity(const glm::vec3& velocity) { m_Velocity = velocity; }
			[[nodiscard]] BodyHandle GetBody() const { return m_Body; }
			[[nodiscard]] glm::vec3 GetPosition() const { return m_World->GetPose(m_Body).Position; }
		private:
			// A documented back-reference: the world outlives the box (both live in one test case).
			PhysicsWorld* m_World = nullptr;
			glm::vec3 m_Velocity = glm::vec3(0.0f);
			PhysicsPose m_Pose{};
			BodyHandle m_Body{};
		};

		// A character created on a MovingPlatform that has not moved yet, standing on it after half a second at rest.
		Scope<CharacterController> CreateCarriedCharacter(PhysicsWorld& world, bool inheritGroundVelocity)
		{
			Result<Scope<CharacterController>> character =
				CharacterController::Create(world, { .Pose = { .Position = glm::vec3(0.0f, 0.05f, 0.0f) }, .InheritGroundVelocity = inheritGroundVelocity });
			REQUIRE_MESSAGE(character.has_value(), character.error().ToString());
			Walk(world, **character, glm::vec3(0.0f), 30);
			REQUIRE((*character)->GetGroundState().IsGrounded);
			return std::move(*character);
		}

	}

	// A step without a duration is a programmer error the controller refuses before Jolt sees it (PhysicsSystem passes the
	// fixed delta).
	ENGINE_DEATH_TEST("Physics/CharacterUpdateWithoutTimeAsserts")
	{
		Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
		if (!world.has_value())
			return;
		Result<Scope<CharacterController>> character = CharacterController::Create(**world, {});
		if (character.has_value())
			(*character)->Update(0.0f, glm::vec3(0.0f), glm::vec3(0.0f, -9.81f, 0.0f));
	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("CharacterController: a character falls, lands and reports the ground")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 2.0f, 0.0f));
			CHECK_FALSE(character->GetGroundState().IsGrounded);
			Walk(*world, *character, glm::vec3(0.0f), 120);
			const CharacterGroundState ground = character->GetGroundState();
			CHECK(ground.IsGrounded);
			CHECK(Test::ApproxEqual(ground.GroundNormal, glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-3f));
			CHECK(character->GetPose().Position.y == doctest::Approx(0.0f).epsilon(0.05));
			CHECK(character->GetUserData() == 77);
		}

		TEST_CASE("CharacterController: a character climbs a step below StepHeight but not a higher one")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A 0.2 m step at x = 2 and a 0.6 m wall at z = 5 (StepHeight 0.3).
			static_cast<void>(CreateBlock(*world, glm::vec3(4.0f, 0.2f, 0.0f), glm::vec3(2.0f, 0.1f, 1.0f)));
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f, 0.6f, 6.0f), glm::vec3(1.0f, 0.3f, 1.0f)));
			Scope<CharacterController> climber = CreateCharacter(*world, glm::vec3(0.0f, 0.0f, 0.0f));
			Walk(*world, *climber, glm::vec3(3.0f, 0.0f, 0.0f), 90);
			CHECK(climber->GetPose().Position.x > 3.0f);
			CHECK(climber->GetPose().Position.y == doctest::Approx(0.2f).epsilon(0.05));
			climber.reset();
			Scope<CharacterController> blocked = CreateCharacter(*world, glm::vec3(0.0f, 0.0f, 3.0f));
			Walk(*world, *blocked, glm::vec3(0.0f, 0.0f, 3.0f), 90);
			CHECK(blocked->GetPose().Position.z < 5.0f);
		}

		TEST_CASE("CharacterController: a character cannot walk up a slope steeper than MaxSlopeAngle")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A 60 degree ramp rising towards +X, starting at x = 2.
			const glm::quat steep = glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			static_cast<void>(CreateBlock(*world, glm::vec3(4.0f, 1.5f, 0.0f), glm::vec3(3.0f, 0.1f, 2.0f), steep));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f));
			Walk(*world, *character, glm::vec3(3.0f, 0.0f, 0.0f), 120);
			CHECK(character->GetPose().Position.y < 0.5f);
		}

		TEST_CASE("CharacterController: the inner body follows the character and is seen by raycasts")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f));
			const BodyHandle inner = character->GetInnerBody();
			REQUIRE(inner.IsValid());
			CHECK(world->GetMotionType(inner) == PhysicsMotionType::Kinematic);
			CHECK(world->GetUserData(inner) == 77);
			character->SetPose({ .Position = glm::vec3(3.0f, 0.0f, 0.0f) });
			Walk(*world, *character, glm::vec3(0.0f), 2);
			const std::optional<PhysicsQueryHit> hit =
				world->Raycast({ .Origin = glm::vec3(3.0f, 0.9f, -5.0f), .Direction = glm::vec3(0.0f, 0.0f, 1.0f), .MaxDistance = 10.0f }, {});
			REQUIRE(hit.has_value());
			CHECK(hit->Body == inner);
			// Destroying the controller removes its inner body.
			character.reset();
			CHECK_FALSE(world->IsBodyValid(inner));
		}

		TEST_CASE("CharacterController: the character's contacts are recorded in the world's buffer")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			const BodyHandle floor = CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.5f, 0.0f));
			static_cast<void>(world->DrainContactEvents());
			Walk(*world, *character, glm::vec3(0.0f), 30);
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			CHECK(std::any_of(contacts.begin(), contacts.end(), [&](const ContactEvent& event)
			{
				return event.Kind == ContactEventKind::Added && event.FromCharacter && event.BodyA == character->GetInnerBody() && event.BodyB == floor;
			}));
		}

		// Docs/Decisions/0031-m20-contract.md decision 3: CharacterVirtual reports through its listener only the contacts its
		// update collided with (a body it moved into, or the ground it stands on), never one merely within its predictive
		// contact distance, so the character's contacts are recorded touching.
		TEST_CASE("CharacterController: a character records the contacts it collides with, touching, and not a wall it walks past")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			const BodyHandle floor = CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f));
			// A wall whose face is 5 cm beside the capsule (radius 0.3 m), well within the character's predictive contact
			// distance, along the way it walks.
			const BodyHandle wall = CreateBlock(*world, glm::vec3(0.85f, 2.0f, 0.0f), glm::vec3(0.5f, 1.0f, 10.0f));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.5f, -5.0f));
			static_cast<void>(world->DrainContactEvents());
			Walk(*world, *character, glm::vec3(0.0f, 0.0f, 2.0f), 120);
			CHECK(character->GetPose().Position.z > -2.0f);
			CHECK(character->GetPose().Position.x == doctest::Approx(0.0f).epsilon(1.0e-3));
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			const auto added = [&contacts](BodyHandle body)
			{
				return std::count_if(contacts.begin(), contacts.end(), [body](const ContactEvent& event)
				{
					return event.Kind == ContactEventKind::Added && event.FromCharacter && event.BodyB == body;
				});
			};
			CHECK(added(floor) >= 1);
			CHECK(added(wall) == 0);
			CHECK(std::all_of(contacts.begin(), contacts.end(), [](const ContactEvent& event)
			{
				return event.Kind != ContactEventKind::Added || event.Touching;
			}));
		}

		TEST_CASE("CharacterController: invalid descriptions are refused before Jolt sees them")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			// "<ErrorCode>" or "<ErrorCode> <PHYSICS code>" of a refusal, "created" when there was none; no assertion inside,
			// so it can sit in a CHECK.
			const auto refusalOf = [&world](const CharacterControllerDescription& description) -> std::string
			{
				Result<Scope<CharacterController>> character = CharacterController::Create(*world, description);
				if (character.has_value())
					return "created";
				const std::string_view code = GetPhysicsDiagnosticCode(character.error());
				return code.empty() ? std::format("{}", character.error().GetCode()) : std::format("{} {}", character.error().GetCode(), code);
			};
			CHECK(refusalOf({ .Height = 0.5f, .Radius = 0.3f }) == "Validation PHYSICS_INVALID_SHAPE");
			CHECK(refusalOf({ .Radius = std::numeric_limits<float>::quiet_NaN() }) == "InvalidArgument");
			CHECK(refusalOf({ .MaxSlopeAngle = 120.0f }) == "InvalidArgument");
			CHECK(refusalOf({ .Mass = 0.0f }) == "InvalidArgument");
			CHECK(refusalOf({ .Mass = 2.0f * MaxPhysicsMass }) == "InvalidArgument");
			CHECK(refusalOf({ .Mass = MaxPhysicsMass }) == "created");
			CHECK(refusalOf({ .Pose = { .Position = glm::vec3(0.0f, -2.0f * MaxPhysicsCoordinate, 0.0f) } }) == "InvalidArgument");
			CHECK(refusalOf({ .Layer = 3 }) == "InvalidArgument");
		}

		TEST_CASE("CharacterController: bodies of the character's collision group are ignored by it and by its inner body")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Result<Scope<CharacterController>> character =
				CharacterController::Create(*world, { .Pose = { .Position = glm::vec3(0.0f) }, .UserData = 77, .CollisionGroup = 77 });
			REQUIRE_MESSAGE(character.has_value(), character.error().ToString());
			// A pickup radius around the character (a kinematic sensor of its group, as Scene/PhysicsSystem attaches one), and
			// a stranger's sensor of no group at the same place.
			Result<Ref<const PhysicsShape>> radius = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = SphereShapeGeometry{ .Radius = 2.0f } } } });
			REQUIRE(radius.has_value());
			const BodyDescription sensor{ .Shape = *radius,
				.MotionType = PhysicsMotionType::Kinematic,
				.Pose = { .Position = glm::vec3(0.0f, 0.9f, 0.0f) },
				.IsSensor = true,
				.CollideKinematicVsNonDynamic = true };
			BodyDescription ownSensor = sensor;
			ownSensor.CollisionGroup = 77;
			Result<BodyHandle> own = world->CreateBody(ownSensor);
			Result<BodyHandle> stranger = world->CreateBody(sensor);
			REQUIRE(own.has_value());
			REQUIRE(stranger.has_value());
			static_cast<void>(world->DrainContactEvents());
			Walk(*world, **character, glm::vec3(0.0f), 5);
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			const BodyHandle inner = (*character)->GetInnerBody();
			const auto touched = [&contacts, inner](BodyHandle body)
			{
				return std::any_of(contacts.begin(), contacts.end(), [inner, body](const ContactEvent& event)
				{
					return event.Kind == ContactEventKind::Added && ((event.BodyA == inner && event.BodyB == body) || (event.BodyA == body && event.BodyB == inner));
				});
			};
			CHECK(touched(*stranger));
			CHECK_FALSE(touched(*own));
		}

		TEST_CASE("CharacterController: a jump is one upward desired velocity, after which gravity brings the character down")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			const BodyHandle floor = CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.1f, 0.0f));
			Walk(*world, *character, glm::vec3(0.0f), 30);
			REQUIRE(character->GetGroundState().IsGrounded);
			static_cast<void>(world->DrainContactEvents());

			// The desired vertical part applies only while grounded and only for the step it is given in.
			Walk(*world, *character, glm::vec3(0.0f, 5.0f, 0.0f), 1);
			CHECK_FALSE(character->GetGroundState().IsGrounded);
			CHECK(character->GetVelocity().y == doctest::Approx(5.0f).epsilon(1.0e-3));
			float apex = character->GetPose().Position.y;
			for (uint32_t step = 0; step < 120; ++step)
			{
				Walk(*world, *character, glm::vec3(0.0f), 1);
				apex = std::max(apex, character->GetPose().Position.y);
			}
			// v^2 / 2g = 1.27 m, plus what the first step adds before gravity acts.
			CHECK(apex > 1.2f);
			CHECK(apex < 1.4f);
			CHECK(character->GetGroundState().IsGrounded);
			CHECK(character->GetPose().Position.y == doctest::Approx(0.0f).epsilon(0.05));

			// Leaving the floor ended the character's contact with it, and landing began a new one.
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			const auto count = [&contacts, &character, floor](ContactEventKind kind)
			{
				return std::count_if(contacts.begin(), contacts.end(), [&character, floor, kind](const ContactEvent& event)
				{
					return event.Kind == kind && event.FromCharacter && event.BodyA == character->GetInnerBody() && event.BodyB == floor;
				});
			};
			CHECK(count(ContactEventKind::Removed) >= 1);
			CHECK(count(ContactEventKind::Added) == count(ContactEventKind::Removed));
		}

		TEST_CASE("CharacterController: a character in the air falls with the gravity it is given")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			Scope<CharacterController> full = CreateCharacter(*world, glm::vec3(0.0f, 50.0f, 0.0f));
			Scope<CharacterController> half = CreateCharacter(*world, glm::vec3(10.0f, 50.0f, 0.0f));
			for (uint32_t step = 0; step < 60; ++step)
			{
				full->Update(FixedDelta, glm::vec3(0.0f), Gravity);
				half->Update(FixedDelta, glm::vec3(0.0f), 0.5f * Gravity);
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			}
			CHECK(full->GetVelocity().y == doctest::Approx(-9.81f).epsilon(1.0e-3));
			CHECK(half->GetVelocity().y == doctest::Approx(-4.905f).epsilon(1.0e-3));
			// 50 - g t^2 / 2 with the step's discretisation: between 45 and 45.3.
			CHECK(full->GetPose().Position.y > 44.9f);
			CHECK(full->GetPose().Position.y < 45.3f);
			CHECK(half->GetPose().Position.y > full->GetPose().Position.y);
			CHECK_FALSE(full->GetGroundState().IsGrounded);
			// The horizontal desired velocity still steers in the air.
			full->Update(FixedDelta, glm::vec3(6.0f, 0.0f, 0.0f), Gravity);
			CHECK(full->GetPose().Position.x == doctest::Approx(0.1f).epsilon(1.0e-3));
		}

		TEST_CASE("CharacterController: SetPose teleports the character and its inner body, keeping the velocity")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f));
			character->SetVelocity(glm::vec3(1.0f, 0.0f, 0.0f));
			const glm::quat facing(0.70710678f, 0.0f, 0.70710678f, 0.0f);
			character->SetPose({ .Position = glm::vec3(5.0f, 0.0f, -3.0f), .Rotation = facing });
			CHECK(Test::ApproxEqual(character->GetPose().Position, glm::vec3(5.0f, 0.0f, -3.0f), 1.0e-6f));
			CHECK(Test::ApproxEqual(character->GetPose().Rotation, glm::normalize(facing), 1.0e-6f));
			CHECK(Test::ApproxEqual(character->GetVelocity(), glm::vec3(1.0f, 0.0f, 0.0f), 1.0e-6f));
			// The inner body stands on the character's base (raised by the character's padding of a few centimetres).
			const PhysicsPose inner = world->GetPose(character->GetInnerBody());
			CHECK(inner.Position.x == doctest::Approx(5.0f).epsilon(1.0e-5));
			CHECK(inner.Position.z == doctest::Approx(-3.0f).epsilon(1.0e-5));
			CHECK(inner.Position.y >= 0.0f);
			CHECK(inner.Position.y < 0.05f);
			const std::optional<PhysicsQueryHit> hit =
				world->Raycast({ .Origin = glm::vec3(5.0f, 0.9f, -10.0f), .Direction = glm::vec3(0.0f, 0.0f, 1.0f), .MaxDistance = 20.0f }, {});
			REQUIRE(hit.has_value());
			CHECK(hit->Body == character->GetInnerBody());
			CHECK(hit->Distance == doctest::Approx(6.7f).epsilon(1.0e-3));
		}

		TEST_CASE("CharacterController: the inner body is a body of the world on the character's layer")
		{
			const std::vector<std::string> layers = { "Default", "Player" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Default" }, { "Default", "Player" } };
			Result<PhysicsLayerTable> table = PhysicsLayerTable::Create(layers, collisions);
			REQUIRE(table.has_value());
			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({ .Layers = *table });
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			const uint32_t bodies = PhysicsEngine::GetLiveBodyCount();
			Result<Scope<CharacterController>> character =
				CharacterController::Create(**world, { .Layer = 1, .Pose = { .Position = glm::vec3(0.0f) }, .UserData = 5 });
			REQUIRE_MESSAGE(character.has_value(), character.error().ToString());
			const BodyHandle inner = (*character)->GetInnerBody();
			CHECK(PhysicsEngine::GetLiveBodyCount() == bodies + 1);
			CHECK((*world)->GetStats().BodyCount == 1);
			CHECK((*world)->GetLayer(inner) == 1);
			CHECK_FALSE((*world)->IsSensor(inner));
			CHECK((*world)->GetUserData(inner) == 5);
			// Queries filter it by its layer like any body.
			const PhysicsRay across{ .Origin = glm::vec3(0.0f, 0.9f, -5.0f), .Direction = glm::vec3(0.0f, 0.0f, 1.0f), .MaxDistance = 10.0f };
			CHECK((*world)->Raycast(across, { .Layers = 0b10u }).value_or(PhysicsQueryHit{}).Body == inner);
			CHECK_FALSE((*world)->Raycast(across, { .Layers = 0b01u }).has_value());
			character->reset();
			CHECK(PhysicsEngine::GetLiveBodyCount() == bodies);
			CHECK((*world)->GetStats().BodyCount == 0);
		}

		TEST_CASE("CharacterController: an update without a duration is refused before Jolt sees it")
		{
			ENGINE_CHECK_DEATH("Physics/CharacterUpdateWithoutTimeAsserts", "CharacterController::Update needs");
		}

		TEST_CASE("CharacterController: a step's velocity is clamped to MaxCharacterSpeed")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 1000.0f, 0.0f));
			// A fall under an enormous gravity reaches the terminal speed and stays there.
			for (uint32_t step = 0; step < 3; ++step)
			{
				character->Update(FixedDelta, glm::vec3(0.0f), glm::vec3(0.0f, -1.0e9f, 0.0f));
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			}
			CHECK(glm::length(character->GetVelocity()) <= MaxCharacterSpeed * 1.0001f);
			CHECK(character->GetVelocity().y == doctest::Approx(-MaxCharacterSpeed).epsilon(1.0e-4));
		}

		TEST_CASE("CharacterController: a gravity too weak to have a length is none, also for the body the character stands on")
		{
			// CharacterVirtual divides by the gravity's length when it pushes the body it stands on; 1e-30 squared underflows.
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Result<Ref<const PhysicsShape>> crateShape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{} } } });
			REQUIRE(crateShape.has_value());
			Result<BodyHandle> crate = world->CreateBody({ .Shape = *crateShape,
				.MotionType = PhysicsMotionType::Dynamic,
				.Pose = { .Position = glm::vec3(0.0f, 0.5f, 0.0f) },
				.AllowSleeping = false });
			REQUIRE(crate.has_value());
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 1.2f, 0.0f));
			Walk(*world, *character, glm::vec3(0.0f), 60);
			REQUIRE(character->GetGroundState().IsGrounded);
			const glm::vec3 faint(0.0f, -1.0e-30f, 0.0f);
			for (uint32_t step = 0; step < 5; ++step)
			{
				character->Update(FixedDelta, glm::vec3(0.0f), faint);
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			}
			CHECK(std::isfinite(glm::length(world->GetLinearVelocity(*crate))));
			CHECK(std::isfinite(glm::length(character->GetVelocity())));
		}

		TEST_CASE("CharacterController: a heavy character's weight on a light body is capped, so the body's velocity stays finite")
		{
			// Jolt pushes the body a character stands on with mass * gravity * deltaTime; a character of MaxPhysicsMass on a
			// 1 g crate under the largest gravity over a whole second would give the crate a velocity whose squared length
			// overflows, which Jolt asserts on.
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Result<Ref<const PhysicsShape>> crateShape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{} } } });
			REQUIRE(crateShape.has_value());
			Result<BodyHandle> crate = world->CreateBody({ .Shape = *crateShape,
				.MotionType = PhysicsMotionType::Dynamic,
				.Pose = { .Position = glm::vec3(0.0f, 0.5f, 0.0f) },
				.Mass = 0.001f,
				.AllowSleeping = false });
			REQUIRE(crate.has_value());
			// The character starts on the crate's top face and lands on it during the one update, which then pushes the crate.
			Result<Scope<CharacterController>> created =
				CharacterController::Create(*world, { .Mass = MaxPhysicsMass, .Pose = { .Position = glm::vec3(0.0f, 1.0f, 0.0f) }, .UserData = 77 });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CharacterController& character = **created;
			character.Update(1.0f, glm::vec3(0.0f), glm::vec3(0.0f, -MaxPhysicsGravity, 0.0f));
			REQUIRE(character.GetGroundState().IsGrounded);
			REQUIRE(world->Step(FixedDelta, 1).has_value());
			CHECK(std::isfinite(glm::length(world->GetLinearVelocity(*crate))));
			CHECK(std::isfinite(glm::length(world->GetAngularVelocity(*crate))));
		}

		TEST_CASE("CharacterController: an update never carries the character beyond MaxPhysicsCoordinate")
		{
			// At the edge of the range floats are 64 m apart, and a fall at the terminal speed over a fifth of a second would
			// leave it; the character stops where it was instead, and its inner body stays where the world places bodies.
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, -MaxPhysicsCoordinate, 0.0f));
			character->SetVelocity(glm::vec3(0.0f, -MaxCharacterSpeed, 0.0f));
			Test::ExpectLog stopped(LogLevel::Warn, "would leave the");
			for (uint32_t step = 0; step < 3; ++step)
				character->Update(0.2f, glm::vec3(0.0f), Gravity);
			CHECK(stopped.GetMatchCount() == 1);
			CHECK(character->GetPose().Position.y >= -MaxPhysicsCoordinate);
			CHECK(IsPlaceablePhysicsPose(world->GetPose(character->GetInnerBody())));
			CHECK(character->GetVelocity().y > -MaxCharacterSpeed);
		}

		// Docs/Decisions/0030-m19-contract.md decision 36 (P-CARRY, stream K; Roadmap M19's acceptance "a carried character
		// moves v·t"): with InheritGroundVelocity a character standing on a kinematic platform that moves at v is carried
		// v * t; without it the platform slides under the character, as before.
		TEST_CASE("CharacterController: with InheritGroundVelocity a character on a moving platform moves v * t")
		{
			const glm::vec3 platformVelocity(2.0f, 0.0f, 0.0f);
			constexpr uint32_t Steps = 120; // two seconds
			for (const bool inherit : { true, false })
			{
				CAPTURE(inherit);
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				MovingPlatform platform(*world, platformVelocity);
				const Scope<CharacterController> character = CreateCarriedCharacter(*world, inherit);

				const float start = character->GetPose().Position.x;
				for (uint32_t step = 0; step < Steps; ++step)
					platform.Step(*character, glm::vec3(0.0f));
				const float moved = character->GetPose().Position.x - start;
				if (inherit)
					CHECK(moved == doctest::Approx(platformVelocity.x * static_cast<float>(Steps) * FixedDelta).epsilon(0.02));
				else
					CHECK(std::abs(moved) < 0.05f);
				CHECK(character->GetGroundState().IsGrounded);
			}
		}

		// ADR 0030 decision 36: the ground's horizontal velocity is added to the desired one, and only while grounded, so a
		// carried character walks relative to its platform and, once it jumps off, moves only as it is told.
		TEST_CASE("CharacterController: a carried character walks relative to its ground and is carried only while grounded")
		{
			const glm::vec3 platformVelocity(2.0f, 0.0f, 0.0f);
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			MovingPlatform platform(*world, platformVelocity);
			const Scope<CharacterController> character = CreateCarriedCharacter(*world, true);

			const glm::vec3 start = character->GetPose().Position;
			for (uint32_t step = 0; step < 60; ++step)
				platform.Step(*character, glm::vec3(0.0f, 0.0f, 1.0f));
			const glm::vec3 moved = character->GetPose().Position - start;
			CHECK(moved.x == doctest::Approx(2.0f).epsilon(0.02));
			CHECK(moved.z == doctest::Approx(1.0f).epsilon(0.02));
			CHECK(character->GetGroundState().GroundVelocity.x == doctest::Approx(platformVelocity.x).epsilon(1.0e-3));
			CHECK(character->GetVelocity().x == doctest::Approx(platformVelocity.x).epsilon(1.0e-3));
			CHECK(character->GetVelocity().z == doctest::Approx(1.0f).epsilon(1.0e-3));

			// The take-off step is still on the ground, so still carried; in the air the platform no longer carries it.
			platform.Step(*character, glm::vec3(0.0f, 5.0f, 0.0f));
			CHECK(character->GetVelocity().x == doctest::Approx(platformVelocity.x).epsilon(1.0e-3));
			for (uint32_t step = 0; step < 3; ++step)
				platform.Step(*character, glm::vec3(0.0f));
			CHECK_FALSE(character->GetGroundState().IsGrounded);
			CHECK(character->GetVelocity().y > 0.0f);
			CHECK(std::abs(character->GetVelocity().x) < 1.0e-4f);
		}

		// M34 (Docs/Decisions/0049-m34-characters-input.md, Roadmap M34's acceptance; World Tree's brief 11): before M34 the
		// character's kinematic inner body, placed rather than swept, drove the shards it overlapped through the floor at
		// infinite mass, and the character, standing on a shard its inner body shoved, was launched upwards.
		TEST_CASE("CharacterController: a character walking at 6.5 m/s through fifteen 0.35 kg Dynamic shards on the floor stays below 0.1 m over 60 ticks, and no shard goes below -0.05 m: the shards are pushed aside along the ground")
		{
			// World Tree's shards are convex hulls; FeatureTest's CharacterDebris scene uses boxes turned about Y.
			for (const bool boxes : { false, true })
			{
				CAPTURE(boxes);
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
				// Fifteen irregular shards about 0.2 m across, as a broken urn leaves them, in three rows of five across the
				// character's way along +X, their bases on the floor.
				std::vector<BodyHandle> shards;
				for (uint32_t index = 0; index < 15; ++index)
				{
					const float jitter = 0.01f * static_cast<float>(index % 4);
					const std::vector<glm::vec3> points = { glm::vec3(-0.1f, -0.06f, -0.08f), glm::vec3(0.11f + jitter, -0.06f, -0.07f),
						glm::vec3(0.02f, -0.06f, 0.1f), glm::vec3(-0.04f, 0.07f + jitter, 0.0f), glm::vec3(0.06f, 0.05f, 0.03f + jitter),
						glm::vec3(-0.08f, 0.02f, 0.07f) };
					const PhysicsShapeGeometry geometry = boxes ? PhysicsShapeGeometry(BoxShapeGeometry{ .HalfExtents = glm::vec3(0.1f + 0.5f * jitter, 0.06f, 0.08f) })
																: PhysicsShapeGeometry(ConvexHullShapeGeometry{ .Points = points });
					Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = geometry } } });
					REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
					const glm::vec3 position(1.0f + 0.3f * static_cast<float>(index / 3), boxes ? 0.06f : 0.07f, -0.3f + 0.3f * static_cast<float>(index % 3));
					const glm::quat turn = glm::angleAxis(glm::radians(static_cast<float>(17 * index % 90)), glm::vec3(0.0f, 1.0f, 0.0f));
					Result<BodyHandle> shard = world->CreateBody(
						{ .Shape = *shape, .MotionType = PhysicsMotionType::Dynamic, .Pose = { .Position = position, .Rotation = turn }, .Mass = 0.35f });
					REQUIRE_MESSAGE(shard.has_value(), shard.error().ToString());
					shards.push_back(*shard);
				}
				Scope<CharacterController> warden = CreateWarden(*world, glm::vec3(-1.0f, 0.0f, 0.0f));
				// The shards settle and the character stands.
				Walk(*world, *warden, glm::vec3(0.0f), 30);
				REQUIRE(warden->GetGroundState().IsGrounded);
				std::vector<glm::vec3> rest;
				for (const BodyHandle shard : shards)
					rest.push_back(world->GetPose(shard).Position);

				float highest = -std::numeric_limits<float>::infinity();
				float lowestShard = std::numeric_limits<float>::infinity();
				for (uint32_t tick = 0; tick < 60; ++tick)
				{
					Walk(*world, *warden, glm::vec3(6.5f, 0.0f, 0.0f), 1);
					highest = std::max(highest, warden->GetPose().Position.y);
					for (const BodyHandle shard : shards)
						lowestShard = std::min(lowestShard, world->GetPose(shard).Position.y);
				}
				CHECK(highest < 0.1f);
				CHECK(lowestShard > -0.05f);
				// The character walked on through them (4 m of the 6.5 m it asked for), and they were pushed aside along the
				// ground.
				CHECK(warden->GetPose().Position.x > 3.0f);
				size_t pushed = 0;
				for (size_t index = 0; index < shards.size(); ++index)
				{
					const glm::vec3 moved = world->GetPose(shards[index]).Position - rest[index];
					if (glm::length(glm::vec2(moved.x, moved.z)) > 0.3f)
						++pushed;
				}
				CHECK(pushed >= 5);
			}
		}

		// M34 decision 1: the inner body's object layer has the Character kind, which the solver pairs with sensors only. Before
		// M34 the kinematic inner body, placed where the character is, pushed a Dynamic body it overlapped out of it with
		// infinite mass.
		TEST_CASE("CharacterController: a Dynamic body the inner body overlaps is not pushed by it in the solver")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A light box resting on the floor, then a character placed so that its capsule (radius 0.3 m) overlaps the box by
			// 0.15 m; the world steps without the character's update, so only its inner body could act on the box.
			const BodyHandle box = CreateCrate(*world, glm::vec3(0.3f, 0.0f, 0.0f), glm::vec3(0.15f), 0.35f);
			for (uint32_t step = 0; step < 10; ++step)
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			const glm::vec3 resting = world->GetPose(box).Position;
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f));
			static_cast<void>(world->DrainContactEvents());
			for (uint32_t step = 0; step < 30; ++step)
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			const BodyHandle inner = character->GetInnerBody();
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			CHECK(std::none_of(contacts.begin(), contacts.end(), [inner, box](const ContactEvent& event)
			{
				return (event.BodyA == inner && event.BodyB == box) || (event.BodyA == box && event.BodyB == inner);
			}));
			CHECK(glm::length(world->GetPose(box).Position - resting) < 0.001f);
		}

		// M34 decision 2: a Dynamic body lighter than the character never moves it. Before M34 a light box sliding into a
		// standing character pushed it along.
		TEST_CASE("CharacterController: a light Dynamic body sliding into a standing character never moves it, and meets no inner body in the solver")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f));
			Walk(*world, *character, glm::vec3(0.0f), 10);
			const glm::vec3 standing = character->GetPose().Position;
			// A light box sliding along the floor at 6 m/s straight at the character, 2 m away.
			const BodyHandle box = CreateCrate(*world, glm::vec3(-2.0f, 0.0f, 0.0f), glm::vec3(0.1f), 0.35f);
			world->SetLinearVelocity(box, glm::vec3(6.0f, 0.0f, 0.0f));
			static_cast<void>(world->DrainContactEvents());
			Walk(*world, *character, glm::vec3(0.0f), 60);
			const BodyHandle inner = character->GetInnerBody();
			const std::vector<ContactEvent> contacts = world->DrainContactEvents();
			CHECK(std::none_of(contacts.begin(), contacts.end(), [inner, box](const ContactEvent& event)
			{
				return !event.FromCharacter && ((event.BodyA == inner && event.BodyB == box) || (event.BodyA == box && event.BodyB == inner));
			}));
			CHECK(glm::length(character->GetPose().Position - standing) < 0.01f);
			CHECK(world->GetPose(box).Position.y > 0.0f);
		}

		// M34 decision 2: a light body is never the character's ground velocity, and carries at most its own weight.
		TEST_CASE("CharacterController: a character standing on a light Dynamic body is not launched by it")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A bouncy 0.35 kg plate, 0.2 m high and wider than the character, which lands on it.
			const BodyHandle plate = CreateCrate(*world, glm::vec3(0.0f), glm::vec3(0.5f, 0.1f, 0.5f), 0.35f, 0.8f);
			Scope<CharacterController> warden = CreateWarden(*world, glm::vec3(0.0f, 0.25f, 0.0f));
			float highest = -std::numeric_limits<float>::infinity();
			float fastestUp = -std::numeric_limits<float>::infinity();
			for (uint32_t tick = 0; tick < 120; ++tick)
			{
				Walk(*world, *warden, glm::vec3(0.0f), 1);
				highest = std::max(highest, warden->GetPose().Position.y);
				fastestUp = std::max(fastestUp, warden->GetVelocity().y);
			}
			CHECK(warden->GetGroundState().IsGrounded);
			CHECK(highest < 0.3f);
			CHECK(fastestUp < 0.1f);
			CHECK(world->GetPose(plate).Position.y > 0.05f);
			CHECK(glm::length(world->GetLinearVelocity(plate)) < 0.5f);
		}

		// M34 decision 2: a light body is no stair; a body at least as heavy as the character still is.
		TEST_CASE("CharacterController: a character pushes a light Dynamic body below StepHeight instead of climbing it, and climbs a heavy one")
		{
			for (const float mass : { 0.35f, 500.0f })
			{
				CAPTURE(mass);
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
				// A 0.25 m high box, below the character's 0.35 m step, half a metre ahead.
				const BodyHandle box = CreateCrate(*world, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.25f, 0.125f, 0.5f), mass);
				Scope<CharacterController> warden = CreateWarden(*world, glm::vec3(0.0f));
				Walk(*world, *warden, glm::vec3(0.0f), 10);
				float highest = -std::numeric_limits<float>::infinity();
				for (uint32_t tick = 0; tick < 60; ++tick)
				{
					Walk(*world, *warden, glm::vec3(2.0f, 0.0f, 0.0f), 1);
					highest = std::max(highest, warden->GetPose().Position.y);
				}
				if (mass < 80.0f)
				{
					CHECK(highest < 0.05f);
					CHECK(world->GetPose(box).Position.x > 2.0f);
				}
				else
				{
					CHECK(highest > 0.2f);
					CHECK(world->GetPose(box).Position.x < 1.5f);
				}
			}
		}

		// M34 decision 2: Jolt slides a character up a walkable surface it runs into; a light body's never carries it up (its
		// contact is a vertical wall to the slide), while a heavy body's ramp is walked up as before.
		TEST_CASE("CharacterController: a character does not walk up a light Dynamic ramp, and walks up a heavy one")
		{
			for (const float mass : { 50.0f, 500.0f })
			{
				CAPTURE(mass);
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
				// A wedge 2 m long and 2 m wide rising to 0.3 m, an 8.5 degree slope, its low edge 0.5 m ahead of the character.
				const std::vector<glm::vec3> points = { glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(2.0f, 0.0f, -1.0f),
					glm::vec3(2.0f, 0.0f, 1.0f), glm::vec3(2.0f, 0.3f, -1.0f), glm::vec3(2.0f, 0.3f, 1.0f) };
				Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = ConvexHullShapeGeometry{ .Points = points } } } });
				REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
				Result<BodyHandle> ramp = world->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Dynamic, .Pose = { .Position = glm::vec3(0.85f, 0.0f, 0.0f) }, .Mass = mass });
				REQUIRE_MESSAGE(ramp.has_value(), ramp.error().ToString());
				Scope<CharacterController> warden = CreateWarden(*world, glm::vec3(0.0f));
				Walk(*world, *warden, glm::vec3(0.0f), 10);
				// A second and a half at 1.5 m/s: on the heavy ramp the character is some 1.4 m up the slope by then.
				float highest = -std::numeric_limits<float>::infinity();
				for (uint32_t tick = 0; tick < 90; ++tick)
				{
					Walk(*world, *warden, glm::vec3(1.5f, 0.0f, 0.0f), 1);
					highest = std::max(highest, warden->GetPose().Position.y);
				}
				if (mass < 80.0f)
					CHECK(highest < 0.05f);
				else
					CHECK(highest > 0.15f);
			}
		}

		// M34 decision 2: the slide rule stops a character getting onto a light body, never moving across the one it stands
		// on. Before the rule exempted the ground body, a character on a tilted light plank met a wall at its own feet.
		TEST_CASE("CharacterController: a character standing on a tilted light Dynamic plank walks up along it")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A 20 kg plank 4 m long, its low end on the floor against a stop, its high end on a block 0.36 m high: a slope of
			// about 5.5 degrees.
			static_cast<void>(CreateBlock(*world, glm::vec3(0.2f, 0.15f, 0.0f), glm::vec3(0.2f, 0.15f, 1.5f)));
			static_cast<void>(CreateBlock(*world, glm::vec3(4.5f, 0.36f, 0.0f), glm::vec3(0.5f, 0.18f, 1.5f)));
			Result<Ref<const PhysicsShape>> shape =
				PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(2.0f, 0.05f, 0.75f) } } } });
			REQUIRE(shape.has_value());
			const glm::quat tilt = glm::angleAxis(glm::radians(6.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			Result<BodyHandle> plank = world->CreateBody({ .Shape = *shape, .Pose = { .Position = glm::vec3(2.4f, 0.3f, 0.0f), .Rotation = tilt }, .Mass = 20.0f });
			REQUIRE_MESSAGE(plank.has_value(), plank.error().ToString());
			for (uint32_t step = 0; step < 60; ++step)
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			Scope<CharacterController> warden = CreateWarden(*world, glm::vec3(1.2f, 0.3f, 0.0f));
			Walk(*world, *warden, glm::vec3(0.0f), 30);
			REQUIRE(warden->GetGroundState().IsGrounded);
			const glm::vec3 start = warden->GetPose().Position;
			// A second at 1.5 m/s up the plank.
			Walk(*world, *warden, glm::vec3(1.5f, 0.0f, 0.0f), 60);
			const glm::vec3 end = warden->GetPose().Position;
			CHECK(end.x - start.x > 1.2f);
			CHECK(end.y - start.y > 0.1f);
			CHECK(warden->GetGroundState().IsGrounded);
		}

		// M34 decision 2: the slide rule never leaves a velocity into the surface, which Jolt's solver could not resolve.
		TEST_CASE("CharacterController: a character falling onto a steep face of a light Dynamic body wedged against a wall slides off it")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A wall whose face is at x 1.605, and against it a 40 kg wedge whose face rises at 60 degrees from its toe at x 1.
			static_cast<void>(CreateBlock(*world, glm::vec3(2.105f, 3.0f, 0.0f), glm::vec3(0.5f, 1.5f, 2.0f)));
			const std::vector<glm::vec3> points = { glm::vec3(0.0f, 0.0f, -0.75f), glm::vec3(0.0f, 0.0f, 0.75f), glm::vec3(0.6f, 0.0f, -0.75f),
				glm::vec3(0.6f, 0.0f, 0.75f), glm::vec3(0.6f, 1.04f, -0.75f), glm::vec3(0.6f, 1.04f, 0.75f) };
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = ConvexHullShapeGeometry{ .Points = points } } } });
			REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
			Result<BodyHandle> wedge = world->CreateBody({ .Shape = *shape, .Pose = { .Position = glm::vec3(1.0f, 0.0f, 0.0f) }, .Mass = 40.0f });
			REQUIRE_MESSAGE(wedge.has_value(), wedge.error().ToString());
			for (uint32_t step = 0; step < 30; ++step)
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			// The character (70 kg, steepest walkable slope 45 degrees) drops onto the face from above it.
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(1.2f, 1.5f, 0.0f));
			Walk(*world, *character, glm::vec3(0.0f), 120);
			const glm::vec3 position = character->GetPose().Position;
			CHECK(position.y < 0.05f);
			CHECK(position.x < 1.0f);
			CHECK(character->GetGroundState().IsGrounded);
		}

		// M34 decision 2: a light body that reaches a character within a tick is stopped by the character's push, at most
		// Mass * 9.81 N, which the body never returns.
		TEST_CASE("CharacterController: a light Dynamic body thrown at a standing character is stopped by its push")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f));
			Walk(*world, *character, glm::vec3(0.0f), 10);
			const glm::vec3 standing = character->GetPose().Position;
			// A 0.35 kg box thrown at 6 m/s from 2 m away, 0.9 m up: it reaches the character's capsule (radius 0.3 m) at
			// about half its height.
			const BodyHandle box = CreateCrate(*world, glm::vec3(-2.0f, 0.8f, 0.0f), glm::vec3(0.1f), 0.35f);
			world->SetLinearVelocity(box, glm::vec3(6.0f, 0.0f, 0.0f));
			float furthest = -std::numeric_limits<float>::infinity();
			for (uint32_t tick = 0; tick < 60; ++tick)
			{
				Walk(*world, *character, glm::vec3(0.0f), 1);
				furthest = std::max(furthest, world->GetPose(box).Position.x);
			}
			CHECK(furthest < -0.2f);
			CHECK(glm::length(character->GetPose().Position - standing) < 0.01f);
		}

		// M34 decision 2: a light body cannot push the character, so a light platform driven up under it rises through it
		// rather than lifting it.
		TEST_CASE("CharacterController: a light Dynamic platform rising under a character passes up through it")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A 5 kg plate 0.1 m thick, wider than the character, which stands on it.
			const BodyHandle plate = CreateCrate(*world, glm::vec3(0.0f), glm::vec3(0.75f, 0.05f, 0.75f), 5.0f);
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.12f, 0.0f));
			Walk(*world, *character, glm::vec3(0.0f), 20);
			REQUIRE(character->GetGroundState().IsGrounded);
			// The plate is driven up at 1.5 m/s for two seconds, past the top of the 1.8 m capsule.
			float highest = -std::numeric_limits<float>::infinity();
			for (uint32_t tick = 0; tick < 120; ++tick)
			{
				world->SetLinearVelocity(plate, glm::vec3(0.0f, 1.5f, 0.0f));
				Walk(*world, *character, glm::vec3(0.0f), 1);
				highest = std::max(highest, character->GetPose().Position.y);
			}
			CHECK(highest < 0.25f);
			CHECK(world->GetPose(plate).Position.y > 2.0f);
		}

		// M34 decision 2: the push is CharacterVirtual's, at most Mass * 9.81 N, and the pushed body meets static geometry
		// as any body does.
		TEST_CASE("CharacterController: a light body a character walks into is pushed along the ground at about its speed, and not through a wall")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A wall whose face is at x 4.
			static_cast<void>(CreateBlock(*world, glm::vec3(4.5f, 2.0f, 0.0f), glm::vec3(0.5f, 1.0f, 3.0f)));
			const BodyHandle box = CreateCrate(*world, glm::vec3(0.8f, 0.0f, 0.0f), glm::vec3(0.15f), 0.35f);
			Scope<CharacterController> warden = CreateWarden(*world, glm::vec3(0.0f));
			Walk(*world, *warden, glm::vec3(0.0f), 10);
			// A second at 3 m/s brings the character to the wall; half of it sees the box ahead, moving along.
			float pushedSpeed = 0.0f;
			for (uint32_t tick = 0; tick < 60; ++tick)
			{
				Walk(*world, *warden, glm::vec3(3.0f, 0.0f, 0.0f), 1);
				if (tick == 20)
					pushedSpeed = world->GetLinearVelocity(box).x;
			}
			CHECK(pushedSpeed > 2.0f);
			CHECK(pushedSpeed < 4.5f);
			const glm::vec3 position = world->GetPose(box).Position;
			CHECK(position.x < 4.0f);
			CHECK(position.x > 3.0f);
			CHECK(position.y > 0.0f);
			CHECK(warden->GetPose().Position.y < 0.05f);
		}

		// M34 decision 1: the character's own collision meets another character's inner body as before.
		TEST_CASE("CharacterController: characters still block each other through their inner bodies")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Scope<CharacterController> walker = CreateCharacter(*world, glm::vec3(0.0f));
			Scope<CharacterController> standing = CreateCharacter(*world, glm::vec3(2.0f, 0.0f, 0.0f));
			for (uint32_t tick = 0; tick < 90; ++tick)
			{
				walker->Update(FixedDelta, glm::vec3(2.0f, 0.0f, 0.0f), Gravity);
				standing->Update(FixedDelta, glm::vec3(0.0f), Gravity);
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			}
			// Two capsules of radius 0.3 m stop about 0.6 m apart.
			CHECK(walker->GetPose().Position.x < 1.45f);
			CHECK(standing->GetPose().Position.x == doctest::Approx(2.0f).epsilon(1.0e-3));
		}

		// --- Surfaces (M36, Docs/Decisions/0053-m36-contact-surfaces.md decision 4) ----------------------------------------

		TEST_CASE("CharacterController: a grounded character reports the collider it stands on")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			const BodyHandle floor = CreateSlabs(*world, 3, 7);
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(-2.0f, 1.0f, 0.0f));
			// In the air: no ground.
			CharacterGroundState ground = character->GetGroundState();
			CHECK_FALSE(ground.IsGrounded);
			CHECK_FALSE(ground.GroundBody.IsValid());
			CHECK(ground.GroundCollider == 0);

			// Landed on the left slab: the compound and that collider's user data, from the update itself.
			Walk(*world, *character, glm::vec3(0.0f), 60);
			ground = character->GetGroundState();
			REQUIRE(ground.IsGrounded);
			CHECK(ground.GroundBody == floor);
			CHECK(ground.GroundCollider == 3);

			// Walked onto the right one.
			Walk(*world, *character, glm::vec3(3.0f, 0.0f, 0.0f), 60);
			ground = character->GetGroundState();
			REQUIRE(ground.IsGrounded);
			CHECK(character->GetPose().Position.x > 0.5f);
			CHECK(ground.GroundBody == floor);
			CHECK(ground.GroundCollider == 7);

			// Off the end of the floor and falling: none again.
			Walk(*world, *character, glm::vec3(0.0f, 0.0f, 12.0f), 90);
			ground = character->GetGroundState();
			CHECK_FALSE(ground.IsGrounded);
			CHECK_FALSE(ground.GroundBody.IsValid());
		}

		TEST_CASE("CharacterController: a belt carries a grounded character whatever InheritGroundVelocity says")
		{
			// World Tree's brief 16: the Warden carried 2.5 m in a second by a belt's surface.
			const glm::vec3 beltVelocity(2.5f, 0.0f, 0.0f);
			for (const bool inherit : { false, true })
			{
				CAPTURE(inherit);
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				const BodyHandle belt = CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 2.0f));
				Result<Scope<CharacterController>> created =
					CharacterController::Create(*world, { .Pose = { .Position = glm::vec3(-10.0f, 0.05f, 0.0f) }, .InheritGroundVelocity = inherit });
				REQUIRE(created.has_value());
				CharacterController& character = **created;
				Walk(*world, character, glm::vec3(0.0f), 30);
				REQUIRE(character.GetGroundState().IsGrounded);

				REQUIRE(world->SetColliderSurfaces(belt, std::array{ PhysicsColliderSurface{ .Collider = 0, .SurfaceVelocity = beltVelocity } }).has_value());
				const float start = character.GetPose().Position.x;
				Walk(*world, character, glm::vec3(0.0f), 60);
				CHECK(character.GetPose().Position.x - start == doctest::Approx(2.5f).epsilon(0.02));
				CHECK(character.GetGroundState().IsGrounded);
				CHECK(character.GetGroundState().GroundVelocity.x == doctest::Approx(2.5f).epsilon(1.0e-3));
				CHECK(character.GetVelocity().x == doctest::Approx(2.5f).epsilon(1.0e-3));

				// Walking against the belt at its speed keeps the character in place.
				const float walking = character.GetPose().Position.x;
				Walk(*world, character, -beltVelocity, 30);
				CHECK(std::abs(character.GetPose().Position.x - walking) < 0.02f);

				// A stopped belt carries nothing.
				REQUIRE(world->SetColliderSurfaces(belt, {}).has_value());
				Walk(*world, character, glm::vec3(0.0f), 2);
				const float stopped = character.GetPose().Position.x;
				Walk(*world, character, glm::vec3(0.0f), 30);
				CHECK(std::abs(character.GetPose().Position.x - stopped) < 1.0e-3f);
				CHECK(character.GetGroundState().GroundVelocity == glm::vec3(0.0f));
			}
		}

		// M34's rule holds for surfaces: a Dynamic body lighter than the character gives it none of its ground velocity.
		TEST_CASE("CharacterController: the surface of a Dynamic body lighter than the character never carries it")
		{
			for (const float mass : { 10.0f, 1000.0f })
			{
				CAPTURE(mass);
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				// A frictionless floor, so the crate's own surface cannot drive the crate along it.
				Result<Ref<const PhysicsShape>> floorShape =
					PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(20.0f, 0.5f, 20.0f) } } } });
				REQUIRE(floorShape.has_value());
				REQUIRE(world->CreateBody({ .Shape = *floorShape, .MotionType = PhysicsMotionType::Static, .Pose = { .Position = glm::vec3(0.0f, -0.5f, 0.0f) }, .Friction = 0.0f })
						.has_value());
				const BodyHandle crate = CreateCrate(*world, glm::vec3(0.0f), glm::vec3(2.0f, 0.25f, 2.0f), mass);
				Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.55f, 0.0f));
				Walk(*world, *character, glm::vec3(0.0f), 30);
				REQUIRE(character->GetGroundState().IsGrounded);
				REQUIRE(character->GetGroundState().GroundBody == crate);

				REQUIRE(world->SetColliderSurfaces(crate, std::array{ PhysicsColliderSurface{ .Collider = 0, .SurfaceVelocity = glm::vec3(1.0f, 0.0f, 0.0f) } })
						.has_value());
				const float start = character->GetPose().Position.x;
				Walk(*world, *character, glm::vec3(0.0f), 60);
				const float moved = character->GetPose().Position.x - start;
				if (mass < 70.0f)
					CHECK(std::abs(moved) < 0.01f);
				else
					CHECK(moved == doctest::Approx(1.0f).epsilon(0.05));
			}
		}

		// The lead's decision on the W36.2 review (Docs/Decisions/0053-m36-contact-surfaces.md, "The W36.2 review's fixes"): a
		// character takes only the part of its ground collider's surface velocity along the ground (projected onto the ground
		// plane by the ground normal), as a body takes it through friction, so a belt whose velocity leaves its surface neither
		// lifts the character nor presses it in, and carries it along the surface at the tangential speed.
		TEST_CASE("CharacterController: a belt whose surface velocity leaves its surface carries a character along it without lifting it")
		{
			for (const bool inherit : { false, true })
			{
				for (const float normal : { 1.5f, -1.5f })
				{
					CAPTURE(inherit);
					CAPTURE(normal);
					Scope<PhysicsWorld> world = CreateCharacterWorld();
					const BodyHandle belt = CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 2.0f));
					Result<Scope<CharacterController>> created =
						CharacterController::Create(*world, { .Pose = { .Position = glm::vec3(-10.0f, 0.05f, 0.0f) }, .InheritGroundVelocity = inherit });
					REQUIRE(created.has_value());
					CharacterController& character = **created;
					Walk(*world, character, glm::vec3(0.0f), 30);
					REQUIRE(character.GetGroundState().IsGrounded);
					const float height = character.GetPose().Position.y;

					REQUIRE(world->SetColliderSurfaces(belt, std::array{ PhysicsColliderSurface{ .Collider = 0, .SurfaceVelocity = glm::vec3(2.5f, normal, 0.0f) } })
							.has_value());
					const float start = character.GetPose().Position.x;
					float drift = 0.0f;
					bool grounded = true;
					for (uint32_t tick = 0; tick < 60; ++tick)
					{
						Walk(*world, character, glm::vec3(0.0f), 1);
						drift = std::max(drift, std::abs(character.GetPose().Position.y - height));
						grounded = grounded && character.GetGroundState().IsGrounded;
					}
					CHECK(drift < 1.0e-3f);
					CHECK(grounded);
					// Along the belt at its tangential 2.5 m/s, as a belt without the normal part carries it.
					CHECK(character.GetPose().Position.x - start == doctest::Approx(2.5f).epsilon(0.02));
					CHECK(character.GetGroundState().GroundVelocity.x == doctest::Approx(2.5f).epsilon(1.0e-3));
					CHECK(std::abs(character.GetGroundState().GroundVelocity.y) < 1.0e-4f);
				}
			}
		}

		// --- Pushes and the squeeze (M36 W36.2, Docs/Decisions/0053-m36-contact-surfaces.md decisions 11 and 12) -------------

		// World Tree's brief 16: a Kinematic piston moved into a character pushes it at the piston's velocity. Within the
		// character's look-ahead (0.1 m a tick, 6 m/s at 60 Hz) the piston never overlaps the capsule; a faster one overlaps it
		// for one tick, by at most its step beyond the look-ahead, and pushes it out at the next update.
		TEST_CASE("CharacterController: a Kinematic piston pushes a character at its velocity")
		{
			for (const float speed : { 2.0f, 8.0f })
			{
				CAPTURE(speed);
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
				Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.05f, 0.0f));
				Walk(*world, *character, glm::vec3(0.0f), 30);
				REQUIRE(character->GetGroundState().IsGrounded);
				// The piston's face starts 0.45 m from the capsule (radius 0.3 m), moving along +X.
				constexpr float HalfWidth = 0.25f;
				KinematicBox piston(*world, glm::vec3(-1.0f, 0.9f, 0.0f), glm::vec3(HalfWidth, 0.85f, 1.0f), glm::vec3(speed, 0.0f, 0.0f));
				float deepest = -1.0f;
				uint32_t overlappingTicks = 0;
				float lastSecondStart = 0.0f;
				for (uint32_t tick = 0; tick < 90; ++tick)
				{
					if (tick == 30)
						lastSecondStart = character->GetPose().Position.x;
					piston.Step(*character);
					const float overlap = (piston.GetPosition().x + HalfWidth) - (character->GetPose().Position.x - 0.3f);
					deepest = std::max(deepest, overlap);
					overlappingTicks += overlap > 0.005f ? 1 : 0;
					CHECK_FALSE(character->GetSqueeze().IsSqueezed);
				}
				// The last second: the character moved at the piston's speed (its own velocity stays the one it asked for).
				CHECK(character->GetPose().Position.x - lastSecondStart == doctest::Approx(speed).epsilon(0.02));
				CHECK(character->GetGroundState().IsGrounded);
				if (speed < 6.0f)
				{
					CHECK(deepest < 0.005f);
				}
				else
				{
					CHECK(deepest < speed * FixedDelta - 0.1f + 0.005f);
					CHECK(overlappingTicks <= 1);
				}
			}
		}

		// World Tree's brief 16: a press's hammer brought down onto a character standing on the floor cannot push it out of the
		// way (the floor holds it), so the character is squeezed by the press, from the second tick of the push on; while the
		// hammer is still above its head, the first contact within the look-ahead included, it is not.
		TEST_CASE("CharacterController: a press brought down onto a grounded character reports it squeezed by the press")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.05f, 0.0f));
			Walk(*world, *character, glm::vec3(0.0f), 30);
			REQUIRE(character->GetGroundState().IsGrounded);
			// The hammer's face 0.3 m above the 1.8 m character, coming down at 1 m/s.
			KinematicBox press(*world, glm::vec3(0.0f, 2.35f, 0.0f), glm::vec3(1.0f, 0.25f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
			std::optional<uint32_t> squeezedAt;
			std::optional<uint32_t> reachedAt;
			for (uint32_t tick = 0; tick < 40; ++tick)
			{
				press.Step(*character);
				// The gap between the hammer's face and the top of the capsule after the step.
				const float gap = (press.GetPosition().y - 0.25f) - (character->GetPose().Position.y + 1.8f);
				if (!reachedAt.has_value() && gap <= 0.005f)
					reachedAt = tick;
				const CharacterSqueeze squeeze = character->GetSqueeze();
				if (!squeezedAt.has_value() && squeeze.IsSqueezed)
				{
					squeezedAt = tick;
					CHECK(squeeze.Body == press.GetBody());
				}
				// Still above the character's padding (2 cm), the first contact within its look-ahead included: no squeeze.
				if (gap > 0.025f)
					CHECK_FALSE(squeeze.IsSqueezed);
			}
			REQUIRE(reachedAt.has_value());
			REQUIRE(squeezedAt.has_value());
			CHECK(*squeezedAt <= *reachedAt + 1);
			// The floor holds the character: it stays on it and stays squeezed while the press comes on.
			CHECK(std::abs(character->GetPose().Position.y) < 0.02f);
			CHECK(character->GetSqueeze().IsSqueezed);
			// A press that stops is no push any more.
			press.SetVelocity(glm::vec3(0.0f));
			press.Step(*character);
			press.Step(*character);
			CHECK_FALSE(character->GetSqueeze().IsSqueezed);
		}

		TEST_CASE("CharacterController: a piston pressing a character against a wall reports it squeezed by the piston")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A wall whose face is at x 1.
			static_cast<void>(CreateBlock(*world, glm::vec3(1.5f, 2.0f, 0.0f), glm::vec3(0.5f, 1.0f, 2.0f)));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.05f, 0.0f));
			Walk(*world, *character, glm::vec3(0.0f), 30);
			KinematicBox piston(*world, glm::vec3(-1.0f, 0.9f, 0.0f), glm::vec3(0.25f, 0.85f, 1.0f), glm::vec3(2.0f, 0.0f, 0.0f));
			bool squeezed = false;
			for (uint32_t tick = 0; tick < 90 && !squeezed; ++tick)
			{
				piston.Step(*character);
				squeezed = character->GetSqueeze().IsSqueezed;
				if (squeezed)
					CHECK(character->GetSqueeze().Body == piston.GetBody());
			}
			CHECK(squeezed);
			// Pushed up to the wall, no farther.
			CHECK(character->GetPose().Position.x < 1.0f - 0.29f);
		}

		// The W36.2 review's finding 1: a character's own ground is its most common pusher. A Kinematic lift rising under it
		// pushes it in every tick it rises, and the character is not squeezed because its update takes the ground's vertical
		// velocity, so it leaves a push ahead of the lift; held under a ceiling, it is squeezed by the lift.
		TEST_CASE("CharacterController: a character a Kinematic lift carries up is never squeezed, and one it lifts into a ceiling is squeezed by the lift")
		{
			for (const float speed : { 1.0f, 2.0f })
			{
				CAPTURE(speed);
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				// The lift's top at y 0; a static ceiling 0.3 m above the 1.8 m character once the lift has risen for a second.
				const float rise = speed;
				static_cast<void>(CreateBlock(*world, glm::vec3(0.0f, rise + 1.8f + 0.3f + 1.0f, 0.0f), glm::vec3(3.0f, 0.5f, 3.0f)));
				KinematicBox lift(*world, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(2.0f, 0.5f, 2.0f), glm::vec3(0.0f));
				Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.05f, 0.0f));
				for (uint32_t tick = 0; tick < 30; ++tick)
					lift.Step(*character);
				REQUIRE(character->GetGroundState().IsGrounded);

				// Up for a second, then at rest: never squeezed, its start and its stop included, and carried with the lift.
				bool squeezed = false;
				lift.SetVelocity(glm::vec3(0.0f, speed, 0.0f));
				for (uint32_t tick = 0; tick < 60; ++tick)
				{
					lift.Step(*character);
					squeezed = squeezed || character->GetSqueeze().IsSqueezed;
				}
				lift.SetVelocity(glm::vec3(0.0f));
				for (uint32_t tick = 0; tick < 30; ++tick)
				{
					lift.Step(*character);
					squeezed = squeezed || character->GetSqueeze().IsSqueezed;
				}
				CHECK_FALSE(squeezed);
				CHECK(character->GetGroundState().IsGrounded);
				CHECK(std::abs(character->GetPose().Position.y - rise) < 0.03f);

				// Up again into the ceiling: the ceiling holds the character, and the lift squeezes it.
				lift.SetVelocity(glm::vec3(0.0f, speed, 0.0f));
				for (uint32_t tick = 0; tick < 60 && !squeezed; ++tick)
				{
					lift.Step(*character);
					squeezed = character->GetSqueeze().IsSqueezed;
					if (squeezed)
						CHECK(character->GetSqueeze().Body == lift.GetBody());
				}
				CHECK(squeezed);
			}
		}

		// World Tree's brief 16: a husk carried by a belt into a resting hammer stops against it, and a hammer at rest pushes
		// nothing, so the character is not squeezed.
		TEST_CASE("CharacterController: a character carried into a resting ram stops against it and is not squeezed")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			const BodyHandle belt = CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 2.0f));
			REQUIRE(world->SetColliderSurfaces(belt, std::array{ PhysicsColliderSurface{ .Collider = 0, .SurfaceVelocity = glm::vec3(2.5f, 0.0f, 0.0f) } })
					.has_value());
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(-2.0f, 0.05f, 0.0f));
			// The ram's face at x 1, its base 0.2 m above the belt, at rest.
			KinematicBox ram(*world, glm::vec3(1.5f, 1.2f, 0.0f), glm::vec3(0.5f, 1.0f, 1.0f), glm::vec3(0.0f));
			for (uint32_t tick = 0; tick < 120; ++tick)
			{
				ram.Step(*character);
				CHECK_FALSE(character->GetSqueeze().IsSqueezed);
			}
			// Against the ram's face, kept off it by the character's padding (2 cm).
			const float stopped = character->GetPose().Position.x;
			CHECK(stopped == doctest::Approx(1.0f - 0.3f - 0.02f).epsilon(0.02));
			ram.Step(*character);
			CHECK(std::abs(character->GetPose().Position.x - stopped) < 1.0e-3f);
		}

		// The squeeze's cases that are not (ADR 0052, the M36 risks): a character's own walk into a wall, into a corner and up
		// a steep face meets only static bodies, which push nothing.
		TEST_CASE("CharacterController: a character's own walk into a wall, a corner or a steep face is never a squeeze")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			// A corner of two walls whose faces are at x 2 and z 2, and a face rising at 60 degrees from x -4 towards -X.
			static_cast<void>(CreateBlock(*world, glm::vec3(2.5f, 2.0f, 0.0f), glm::vec3(0.5f, 1.0f, 3.0f)));
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f, 2.0f, 2.5f), glm::vec3(3.0f, 1.0f, 0.5f)));
			static_cast<void>(CreateBlock(*world, glm::vec3(-4.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.5f, 2.0f), glm::normalize(glm::quat(0.8660254f, 0.0f, 0.0f, -0.5f))));
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 0.05f, -1.0f));
			struct Approach
			{
				glm::vec3 Start = glm::vec3(0.0f);
				glm::vec3 Velocity = glm::vec3(0.0f);
			};
			for (const Approach& walk : { Approach{ glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(3.0f, 0.0f, 0.0f) }, Approach{ glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(3.0f, 0.0f, 3.0f) },
					 Approach{ glm::vec3(-2.0f, 0.0f, 0.0f), glm::vec3(-3.0f, 0.0f, 0.0f) } })
			{
				CAPTURE(walk.Velocity.x);
				CAPTURE(walk.Velocity.z);
				character->SetPose({ .Position = walk.Start });
				for (uint32_t tick = 0; tick < 90; ++tick)
				{
					character->Update(FixedDelta, walk.Velocity, Gravity);
					REQUIRE(world->Step(FixedDelta, 1).has_value());
					CHECK_FALSE(character->GetSqueeze().IsSqueezed);
				}
				// Held back by what it walked into: never through it, never up the steep face.
				CHECK(character->GetPose().Position.y < 0.1f);
				CHECK(character->GetPose().Position.x < 1.75f);
				CHECK(character->GetPose().Position.x > -4.1f);
			}
		}

		// --- Free moves (M48 W48.1, Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md decision 3) -----

		// Skerries' brief S17: before M48 a script could not give a character in the air a vertical velocity, so it could not
		// swim, climb or glide.
		TEST_CASE("CharacterController: a free move in the air takes the whole velocity asked, and no gravity acts")
		{
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 20.0f, 0.0f));
			// Falling first, so the free move replaces a velocity of its own.
			Walk(*world, *character, glm::vec3(0.0f), 30);
			REQUIRE(character->GetVelocity().y < -4.0f);
			CHECK(character->GetLastMove() == CharacterMoveMode::Walk);
			const glm::vec3 asked(2.0f, 3.0f, -1.0f);
			const glm::vec3 start = character->GetPose().Position;
			for (uint32_t tick = 0; tick < 60; ++tick)
			{
				character->Update(FixedDelta, asked, Gravity, CharacterMoveMode::Free);
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				CHECK(character->GetVelocity() == asked);
				CHECK(character->GetLastMove() == CharacterMoveMode::Free);
			}
			// A second at the velocity asked: no gravity bent the path.
			CHECK(Test::ApproxEqual(character->GetPose().Position - start, asked, 1.0e-3f));
			CHECK_FALSE(character->GetGroundState().IsGrounded);
		}

		TEST_CASE("CharacterController: a free move while grounded is a Move")
		{
			// Two identical worlds: one character walks, the other free-moves with the same velocities, a walk and then a
			// jump, both on the ground. Every update of the free mover is the walker's, bit for bit.
			Scope<PhysicsWorld> walkWorld = CreateCharacterWorld();
			Scope<PhysicsWorld> freeWorld = CreateCharacterWorld();
			for (PhysicsWorld* world : { walkWorld.get(), freeWorld.get() })
			{
				static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
				static_cast<void>(CreateBlock(*world, glm::vec3(3.0f, 0.2f, 0.0f), glm::vec3(1.0f, 0.1f, 2.0f)));
			}
			Scope<CharacterController> walker = CreateCharacter(*walkWorld, glm::vec3(0.0f, 0.05f, 0.0f));
			Scope<CharacterController> mover = CreateCharacter(*freeWorld, glm::vec3(0.0f, 0.05f, 0.0f));
			Walk(*walkWorld, *walker, glm::vec3(0.0f), 30);
			Walk(*freeWorld, *mover, glm::vec3(0.0f), 30);
			REQUIRE(mover->GetGroundState().IsGrounded);
			// Up onto a 0.2 m step at 2 m/s (stairs and floor sticking as a walk has them), then a jump.
			std::vector<glm::vec3> velocities(60, glm::vec3(2.0f, 0.0f, 0.0f));
			velocities.push_back(glm::vec3(0.0f, 5.0f, 0.0f));
			for (const glm::vec3& velocity : velocities)
			{
				walker->Update(FixedDelta, velocity, Gravity);
				mover->Update(FixedDelta, velocity, Gravity, CharacterMoveMode::Free);
				REQUIRE(walkWorld->Step(FixedDelta, 1).has_value());
				REQUIRE(freeWorld->Step(FixedDelta, 1).has_value());
				CHECK(mover->GetPose().Position == walker->GetPose().Position);
				CHECK(mover->GetVelocity() == walker->GetVelocity());
				CHECK(mover->GetLastMove() == CharacterMoveMode::Walk);
			}
			CHECK(mover->GetPose().Position.y > 0.15f);
			CHECK(mover->GetVelocity().y == doctest::Approx(5.0f).epsilon(1.0e-3));
			CHECK_FALSE(mover->GetGroundState().IsGrounded);
		}

		TEST_CASE("CharacterController: a free move in the air is not stuck to the floor")
		{
			// A character in the air that leans on a 60 degree slope (a steep contact supports it without its being grounded)
			// and then moves off it level, with the floor 0.2 m below its feet: within StepHeight, where floor sticking pulls
			// a character down the moment it loses its support. A free move keeps its height; a walk in the same place is
			// stuck to the floor at once.
			for (const CharacterMoveMode mode : { CharacterMoveMode::Free, CharacterMoveMode::Walk })
			{
				CAPTURE(static_cast<int>(mode));
				Scope<PhysicsWorld> world = CreateCharacterWorld();
				static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
				// The slope's toe at x 0 on the floor, rising at 60 degrees to 1.73 m at x 1.
				const std::vector<glm::vec3> points = { glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(1.0f, 0.0f, -2.0f),
					glm::vec3(1.0f, 0.0f, 2.0f), glm::vec3(1.0f, 1.7320508f, -2.0f), glm::vec3(1.0f, 1.7320508f, 2.0f) };
				Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{ .Geometry = ConvexHullShapeGeometry{ .Points = points } } } });
				REQUIRE_MESSAGE(shape.has_value(), shape.error().ToString());
				REQUIRE(world->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static }).has_value());
				// Hovering 0.2 m above the floor, then drifting into the slope at 0.6 m/s.
				Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(-0.2f, 0.2f, 0.0f));
				for (uint32_t tick = 0; tick < 30; ++tick)
				{
					character->Update(FixedDelta, glm::vec3(0.6f, 0.0f, 0.0f), Gravity, CharacterMoveMode::Free);
					REQUIRE(world->Step(FixedDelta, 1).has_value());
				}
				const float height = character->GetPose().Position.y;
				REQUIRE(height > 0.15f);
				REQUIRE(height < 0.3f);
				REQUIRE_FALSE(character->GetGroundState().IsGrounded);
				// Off the slope, level.
				character->Update(FixedDelta, glm::vec3(-6.0f, 0.0f, 0.0f), Gravity, mode);
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				if (mode == CharacterMoveMode::Free)
				{
					CHECK(character->GetPose().Position.y == doctest::Approx(height).epsilon(1.0e-5));
					CHECK_FALSE(character->GetGroundState().IsGrounded);
					CHECK(character->GetLastMove() == CharacterMoveMode::Free);
				}
				else
				{
					CHECK(character->GetPose().Position.y < 0.05f);
				}
			}
		}

		TEST_CASE("CharacterController: a character free-moved toward a moving target height follows it within 1 mm")
		{
			// S17's swimmer held at a depth under a swell: each tick asks for the vertical velocity that reaches the target
			// height of the next tick, while it swims 20 m along X.
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			Scope<CharacterController> swimmer = CreateCharacter(*world, glm::vec3(0.0f, 10.0f, 0.0f));
			const auto target = [](uint32_t tick)
			{
				const double seconds = static_cast<double>(tick) * static_cast<double>(FixedDelta);
				return static_cast<float>(10.0 + 0.5 * std::sin(seconds * 1.5));
			};
			float worst = 0.0f;
			for (uint32_t tick = 0; tick < 600; ++tick)
			{
				const float vertical = (target(tick + 1) - swimmer->GetPose().Position.y) / FixedDelta;
				swimmer->Update(FixedDelta, glm::vec3(2.0f, vertical, 0.0f), Gravity, CharacterMoveMode::Free);
				REQUIRE(world->Step(FixedDelta, 1).has_value());
				worst = std::max(worst, std::abs(swimmer->GetPose().Position.y - target(tick + 1)));
			}
			CHECK(worst < 1.0e-3f);
			CHECK(swimmer->GetPose().Position.x == doctest::Approx(20.0f).epsilon(1.0e-3));
		}

		TEST_CASE("CharacterController: a climber free-moved along a wall rises along it and steps off at the top onto the floor")
		{
			// S17's ladder: a 6 m wall whose face is at x 1 and whose top is the floor of a platform. The climber presses into
			// it and rises at 2 m/s with free moves, then walks off forwards at the top and lands on the platform.
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			static_cast<void>(CreateBlock(*world, glm::vec3(0.0f), glm::vec3(20.0f, 0.5f, 20.0f)));
			static_cast<void>(CreateBlock(*world, glm::vec3(3.0f, 6.0f, 0.0f), glm::vec3(2.0f, 3.0f, 2.0f)));
			Scope<CharacterController> climber = CreateCharacter(*world, glm::vec3(0.68f, 0.05f, 0.0f));
			Walk(*world, *climber, glm::vec3(0.0f), 30);
			REQUIRE(climber->GetGroundState().IsGrounded);
			float nearest = -std::numeric_limits<float>::infinity();
			float farthest = std::numeric_limits<float>::infinity();
			bool climbedFree = true;
			bool atTop = false;
			uint32_t ticks = 0;
			for (; ticks < 300; ++ticks)
			{
				const glm::vec3 base = climber->GetPose().Position;
				// Up until the feet are above the top, then forwards until it stands on the platform.
				atTop = atTop || base.y > 6.05f;
				if (atTop && base.x > 1.5f && climber->GetGroundState().IsGrounded)
					break;
				if (!atTop)
				{
					climber->Update(FixedDelta, glm::vec3(0.5f, 2.0f, 0.0f), Gravity, CharacterMoveMode::Free);
					// After the first update, which leaves the floor as a jump does, the climb is a free move.
					climbedFree = climbedFree && (ticks == 0 || climber->GetLastMove() == CharacterMoveMode::Free);
					// Along the wall's face, below its top edge.
					if (climber->GetPose().Position.y < 5.5f)
					{
						nearest = std::max(nearest, climber->GetPose().Position.x);
						farthest = std::min(farthest, climber->GetPose().Position.x);
					}
				}
				else
				{
					climber->Update(FixedDelta, glm::vec3(2.0f, 0.0f, 0.0f), Gravity);
				}
				REQUIRE(world->Step(FixedDelta, 1).has_value());
			}
			// Six metres at 2 m/s, then a short walk: well within five seconds.
			CHECK(ticks < 300);
			CHECK(climbedFree);
			// Pressed against the wall all the way up, never into it.
			CHECK(nearest < 1.0f - 0.3f);
			CHECK(farthest > 1.0f - 0.35f);
			CHECK(climber->GetGroundState().IsGrounded);
			CHECK(climber->GetPose().Position.y == doctest::Approx(6.0f).epsilon(0.01));
			CHECK(climber->GetLastMove() == CharacterMoveMode::Walk);
		}

		TEST_CASE("CharacterController: a Move in the air after a free move continues from its velocity plus gravity")
		{
			// A swimmer surging out of the water keeps rising briefly once it moves as a walker again.
			Scope<PhysicsWorld> world = CreateCharacterWorld();
			Scope<CharacterController> character = CreateCharacter(*world, glm::vec3(0.0f, 10.0f, 0.0f));
			character->Update(FixedDelta, glm::vec3(1.0f, 3.0f, 0.0f), Gravity, CharacterMoveMode::Free);
			REQUIRE(world->Step(FixedDelta, 1).has_value());
			REQUIRE(character->GetVelocity() == glm::vec3(1.0f, 3.0f, 0.0f));
			// The walk's own vertical part is ignored in the air, as ever; its horizontal part steers.
			const float before = character->GetPose().Position.y;
			character->Update(FixedDelta, glm::vec3(0.0f, 50.0f, 0.0f), Gravity);
			REQUIRE(world->Step(FixedDelta, 1).has_value());
			CHECK(character->GetLastMove() == CharacterMoveMode::Walk);
			CHECK(character->GetVelocity().y == doctest::Approx(3.0f + Gravity.y * FixedDelta).epsilon(1.0e-5));
			CHECK(character->GetVelocity().x == doctest::Approx(0.0f));
			CHECK(character->GetPose().Position.y - before == doctest::Approx((3.0f + Gravity.y * FixedDelta) * FixedDelta).epsilon(1.0e-3));
			// It rises to v^2 / 2g above, then falls.
			float apex = character->GetPose().Position.y;
			for (uint32_t tick = 0; tick < 60; ++tick)
			{
				Walk(*world, *character, glm::vec3(0.0f), 1);
				apex = std::max(apex, character->GetPose().Position.y);
			}
			CHECK(apex - before > 0.4f);
			CHECK(apex - before < 0.5f);
			CHECK(character->GetVelocity().y < 0.0f);
		}
	}

}
