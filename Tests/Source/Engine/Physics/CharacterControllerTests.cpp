#include "TestsPCH.h"

#include "Engine/Physics/CharacterController.h"

#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsWorld.h"

#include <format>
#include <limits>
#include <string>
#include <string_view>

// The character controller (Architecture §9.6: CharacterVirtual with ExtendedUpdate, a kinematic inner body seen by
// queries and sensors, a CharacterContactListener recording into the world's buffer). Skipped skeletons of the M11
// contract (Docs/Decisions/0014-m11-decisions.md): stream C implements the controller and removes the skips.

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

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("CharacterController: a character falls, lands and reports the ground" * doctest::skip(true))
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

		TEST_CASE("CharacterController: a character climbs a step below StepHeight but not a higher one" * doctest::skip(true))
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

		TEST_CASE("CharacterController: a character cannot walk up a slope steeper than MaxSlopeAngle" * doctest::skip(true))
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

		TEST_CASE("CharacterController: the inner body follows the character and is seen by raycasts" * doctest::skip(true))
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

		TEST_CASE("CharacterController: the character's contacts are recorded in the world's buffer" * doctest::skip(true))
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

		TEST_CASE("CharacterController: invalid descriptions are refused before Jolt sees them" * doctest::skip(true))
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
			CHECK(refusalOf({ .Layer = 3 }) == "InvalidArgument");
		}

		TEST_CASE("CharacterController: bodies of the character's collision group are ignored by it and by its inner body" * doctest::skip(true))
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
	}

}
