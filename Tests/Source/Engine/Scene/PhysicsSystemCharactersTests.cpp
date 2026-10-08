#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Physics/CharacterController.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <limits>

// Characters in the play session (Architecture §9.6: CharacterController:Move, IsGrounded, GetGroundNormal, GetVelocity; the
// inner body seen by sensors and raycasts; character contacts as events). "Physics: trigger detects a character through its
// inner body" is a Roadmap M11 acceptance name, used verbatim.

namespace Engine {

	namespace {

		// Moves "/Player" at `velocity` in every fixed update, as a script's CharacterController:Move in OnFixedUpdate.
		class Walker final : public IPlaySessionObserver
		{
		public:
			explicit Walker(const glm::vec3& velocity)
				: m_Velocity(velocity)
			{
			}

			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				if (phase != PlaySessionPhase::FixedUpdate)
					return;
				const UUID player = Test::GetEntityId(session.GetScene(), "/Player");
				REQUIRE(session.GetPhysics().MoveCharacter(player, m_Velocity).has_value());
			}
		private:
			glm::vec3 m_Velocity = glm::vec3(0.0f);
		};

		// Writes "/Player"'s Transform at the start of the fixed update of tick `tick`, as a script's respawn would.
		class Respawner final : public IPlaySessionObserver
		{
		public:
			Respawner(uint64_t tick, const glm::vec3& position)
				: m_Tick(tick), m_Position(position)
			{
			}

			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				if (phase != PlaySessionPhase::FixedUpdate || tick != m_Tick)
					return;
				Entity player = session.GetScene().FindEntityByPath("/Player");
				player.GetComponent<TransformComponent>().Translation = m_Position;
			}
		private:
			uint64_t m_Tick = 0;
			glm::vec3 m_Position = glm::vec3(0.0f);
		};

		Entity AddPlayer(Scene& scene, const glm::vec3& position)
		{
			Entity player = scene.CreateEntity("Player");
			player.Patch<TransformComponent>([&position](TransformComponent& transform)
			{
				transform.Translation = position;
			});
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			return player;
		}

		Scope<PlaySession> StartCharacterSession(Test::SceneTestFixture& fixture, IPlaySessionObserver* observer, Test::RecordingPhysicsListener* listener = nullptr)
		{
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Observer = observer;
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			if (listener != nullptr)
				(*session)->GetPhysics().SetEventListener(listener);
			return std::move(*session);
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Physics: trigger detects a character through its inner body")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(AddPlayer(scene, glm::vec3(0.0f)));
			Entity goal = Test::AddBoxBody(scene, "Goal", glm::vec3(5.0f, 1.0f, 0.0f), glm::vec3(1.0f), std::nullopt);
			goal.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Walker walker(glm::vec3(3.0f, 0.0f, 0.0f));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartCharacterSession(fixture, &walker, &listener);
			// 2.5 s at 3 m/s: through the goal (x in [4, 6], entered with the capsule's 0.3 m radius at 3.7) and out (6.3).
			Test::RunTicks(*session, 150);
			const UUID player = Test::GetEntityId(session->GetScene(), "/Player");
			const std::vector<PhysicsEvent> entered = listener.Find(goal.GetUUID(), PhysicsEventType::TriggerEnter);
			REQUIRE(entered.size() == 1);
			CHECK(entered[0].Other == player);
			CHECK(listener.Find(player, PhysicsEventType::TriggerEnter).size() == 1);
			const std::vector<PhysicsEvent> exited = listener.Find(goal.GetUUID(), PhysicsEventType::TriggerExit);
			REQUIRE(exited.size() == 1);
			CHECK(exited[0].Other == player);
			CHECK_FALSE(exited[0].Synthesized);
			CHECK(listener.Find(player, PhysicsEventType::TriggerExit).size() == 1);
			CHECK(listener.Find(goal.GetUUID(), PhysicsEventType::CollisionEnter).empty());
			CHECK(Test::GetWorldPosition(*session, "/Player").x > 7.0f);
		}

		TEST_CASE("PhysicsSystem: MoveCharacter moves the character for one step and its pose is written back")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddPlayer(fixture.GetScene(), glm::vec3(0.0f)));
			Scope<PlaySession> session = StartCharacterSession(fixture, nullptr);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID player = Test::GetEntityId(session->GetScene(), "/Player");
			Test::RunTicks(*session, 30);
			REQUIRE(physics.MoveCharacter(player, glm::vec3(0.0f, 0.0f, 6.0f)).has_value());
			session->Tick();
			const float moved = Test::GetWorldPosition(*session, "/Player").z;
			CHECK(moved == doctest::Approx(0.1f).epsilon(0.05));
			// The desired velocity was consumed by that step: without another Move the character stops.
			Test::RunTicks(*session, 10);
			CHECK(Test::GetWorldPosition(*session, "/Player").z == doctest::Approx(moved).epsilon(0.01));
			CHECK(physics.MoveCharacter(Test::GetEntityId(session->GetScene(), "/Ground"), glm::vec3(1.0f)).error().GetCode() == ErrorCode::NotFound);
			CHECK(physics.MoveCharacter(player, glm::vec3(std::numeric_limits<float>::quiet_NaN())).error().GetCode() == ErrorCode::InvalidArgument);
			// A component beyond MaxCharacterSpeed is out of range (FLT_MAX would carry the character out of Jolt's bounds).
			CHECK(physics.MoveCharacter(player, glm::vec3(std::numeric_limits<float>::max(), 0.0f, 0.0f)).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.MoveCharacter(player, glm::vec3(0.0f, 0.0f, -MaxCharacterSpeed)).has_value());
			// A Transform written beyond MaxPhysicsCoordinate is no teleport: the character moves on from where it was, and its
			// write-back replaces the value.
			const glm::vec3 before = Test::GetWorldPosition(*session, "/Player");
			session->GetScene().FindEntityByPath("/Player").GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 1.0e30f);
			session->Tick();
			CHECK(Test::GetWorldPosition(*session, "/Player").z == doctest::Approx(before.z - MaxCharacterSpeed / 60.0f).epsilon(0.05));
		}

		TEST_CASE("PhysicsSystem: a character reports its grounded state, ground normal and velocity")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddPlayer(fixture.GetScene(), glm::vec3(0.0f, 3.0f, 0.0f)));
			Walker walker(glm::vec3(2.0f, 0.0f, 0.0f));
			Scope<PlaySession> session = StartCharacterSession(fixture, &walker);
			const UUID player = Test::GetEntityId(session->GetScene(), "/Player");
			Test::RunTicks(*session, 5);
			CHECK_FALSE(session->GetPhysics().GetCharacterState(player).value_or(PhysicsCharacterState{ .IsGrounded = true }).IsGrounded);
			Test::RunTicks(*session, 120);
			const Result<PhysicsCharacterState> state = session->GetPhysics().GetCharacterState(player);
			REQUIRE(state.has_value());
			CHECK(state->IsGrounded);
			CHECK(Test::ApproxEqual(state->GroundNormal, glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-3f));
			CHECK(state->Velocity.x == doctest::Approx(2.0f).epsilon(0.05));
			const std::optional<PhysicsBodyReport> info = session->GetPhysics().GetBodyInfo(player);
			REQUIRE(info.has_value());
			CHECK(info->Origin == PhysicsBodyOrigin::Character);
			REQUIRE(info->Character.has_value());
			CHECK(info->Character->IsGrounded);
		}

		TEST_CASE("PhysicsSystem: a Transform write teleports a character")
		{
			// PreStep compares the local Translation and Rotation with what PostStep wrote, as for Dynamic bodies, and moves
			// the character with CharacterController::SetPose instead of letting its update overwrite the write.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddPlayer(fixture.GetScene(), glm::vec3(0.0f)));
			Respawner respawner(30, glm::vec3(10.0f, 0.0f, 5.0f));
			Scope<PlaySession> session = StartCharacterSession(fixture, &respawner);
			Test::RunTicks(*session, 31);
			// PostStep wrote the character's own pose back after its update: had the controller not been moved, the entity
			// would be back near the origin.
			const glm::vec3 position = Test::GetWorldPosition(*session, "/Player");
			CHECK(position.x == doctest::Approx(10.0f).epsilon(1.0e-3));
			CHECK(position.z == doctest::Approx(5.0f).epsilon(1.0e-3));
		}

		TEST_CASE("PhysicsSystem: a trigger attached to a character never reports its own character")
		{
			// A pickup radius below the player (ADR 0014 decision 10) shares the character's collision group: it reports the
			// crate inside it, never the character's inner body.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity player = AddPlayer(scene, glm::vec3(0.0f));
			Entity pickup = Test::AddBoxBody(scene, "Pickup", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.5f), std::nullopt, player);
			pickup.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(1.0f, 0.25f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartCharacterSession(fixture, nullptr, &listener);
			Test::RunTicks(*session, 10);
			const std::vector<PhysicsEvent> entered = listener.Find(pickup.GetUUID(), PhysicsEventType::TriggerEnter);
			REQUIRE(entered.size() == 1);
			CHECK(entered[0].Other == Test::GetEntityId(session->GetScene(), "/Crate"));
			CHECK(listener.Find(player.GetUUID(), PhysicsEventType::TriggerEnter).empty());
		}

		TEST_CASE("PhysicsSystem: a character's contacts give collision events with the same rules as bodies")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(AddPlayer(scene, glm::vec3(0.0f)));
			static_cast<void>(Test::AddBoxBody(scene, "Wall", glm::vec3(3.0f, 1.0f, 0.0f), glm::vec3(0.5f, 1.0f, 3.0f), BodyType::Static));
			Walker walker(glm::vec3(3.0f, 0.0f, 0.0f));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartCharacterSession(fixture, &walker, &listener);
			Test::RunTicks(*session, 90);
			const UUID player = Test::GetEntityId(session->GetScene(), "/Player");
			const UUID wall = Test::GetEntityId(session->GetScene(), "/Wall");
			const std::vector<PhysicsEvent> hit = listener.Find(player, PhysicsEventType::CollisionEnter);
			CHECK(std::count_if(hit.begin(), hit.end(), [wall](const PhysicsEvent& event)
			{
				return event.Other == wall;
			}) == 1);
			CHECK(listener.Find(wall, PhysicsEventType::CollisionEnter).size() == 1);
			// Destroying the wall closes the pair with a synthesized exit for the character (§9.4).
			session->GetScene().DestroyEntity(session->GetScene().FindEntityByID(wall));
			listener.Events.clear();
			session->Tick();
			const std::vector<PhysicsEvent> exits = listener.Find(player, PhysicsEventType::CollisionExit);
			REQUIRE(exits.size() == 1);
			CHECK(exits[0].Other == wall);
			CHECK(exits[0].Synthesized);
		}

		TEST_CASE("PhysicsSystem: a jump is one MoveCharacter with an upward part")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddPlayer(fixture.GetScene(), glm::vec3(0.0f)));
			Scope<PlaySession> session = StartCharacterSession(fixture, nullptr);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID player = Test::GetEntityId(session->GetScene(), "/Player");
			Test::RunTicks(*session, 10);
			REQUIRE(physics.GetCharacterState(player).value_or(PhysicsCharacterState{}).IsGrounded);
			REQUIRE(physics.MoveCharacter(player, glm::vec3(0.0f, 5.0f, 0.0f)).has_value());
			float apex = 0.0f;
			for (uint32_t tick = 0; tick < 90; ++tick)
			{
				session->Tick();
				apex = std::max(apex, Test::GetWorldPosition(*session, "/Player").y);
			}
			// v^2 / 2g = 1.27 m: one Move lifted the character once, and it landed again.
			CHECK(apex > 1.2f);
			CHECK(apex < 1.4f);
			CHECK(physics.GetCharacterState(player).value_or(PhysicsCharacterState{}).IsGrounded);
			CHECK(Test::GetWorldPosition(*session, "/Player").y == doctest::Approx(0.0f).epsilon(0.05));
			CHECK(physics.GetCharacterState(Test::GetEntityId(session->GetScene(), "/Ground")).error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("PhysicsSystem: a character falls with its GravityFactor times the project's gravity")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddPlayer(scene, glm::vec3(0.0f, 50.0f, 0.0f)));
			Entity feather = AddPlayer(scene, glm::vec3(5.0f, 50.0f, 0.0f));
			feather.SetName("Feather");
			feather.Patch<CharacterControllerComponent>([](CharacterControllerComponent& controller)
			{
				controller.GravityFactor = 0.5f;
			});
			Scope<PlaySession> session = StartCharacterSession(fixture, nullptr);
			Test::RunTicks(*session, 60);
			const PhysicsSystem& physics = session->GetPhysics();
			const Result<PhysicsCharacterState> full = physics.GetCharacterState(Test::GetEntityId(session->GetScene(), "/Player"));
			const Result<PhysicsCharacterState> half = physics.GetCharacterState(feather.GetUUID());
			REQUIRE(full.has_value());
			REQUIRE(half.has_value());
			CHECK(full->Velocity.y == doctest::Approx(-9.81f).epsilon(1.0e-3));
			CHECK(half->Velocity.y == doctest::Approx(-4.905f).epsilon(1.0e-3));
			CHECK(Test::GetWorldPosition(*session, "/Feather").y > Test::GetWorldPosition(*session, "/Player").y);
		}

		TEST_CASE("PhysicsSystem: a character below a parent writes its pose back through the parent")
		{
			// The pose is world space; the entity's local Transform is written through the parent's inverse, so the entity
			// stays where the character is (and its own write-back never reads as a teleport).
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity rig = scene.CreateEntity("Rig");
			rig.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
			});
			scene.CreateEntity("Player", rig).AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			class RigWalker final : public IPlaySessionObserver
			{
			public:
				void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
				{
					if (phase == PlaySessionPhase::FixedUpdate)
						REQUIRE(session.GetPhysics().MoveCharacter(Test::GetEntityId(session.GetScene(), "/Rig/Player"), glm::vec3(3.0f, 0.0f, 0.0f)).has_value());
				}
			};
			RigWalker walker;
			Scope<PlaySession> session = StartCharacterSession(fixture, &walker);
			Test::RunTicks(*session, 30);
			const glm::vec3 world = Test::GetWorldPosition(*session, "/Rig/Player");
			CHECK(world.x == doctest::Approx(11.5f).epsilon(1.0e-3));
			const Entity player = session->GetScene().FindEntityByPath("/Rig/Player");
			REQUIRE(player.IsValid());
			CHECK(player.GetComponent<TransformComponent>().Translation.x == doctest::Approx(1.5f).epsilon(1.0e-3));
		}
	}

}
