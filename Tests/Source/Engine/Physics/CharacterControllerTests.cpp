#include "TestsPCH.h"

#include "Engine/Physics/CharacterController.h"

#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"

#include <algorithm>
#include <format>
#include <limits>
#include <string>
#include <string_view>

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
	}

}
