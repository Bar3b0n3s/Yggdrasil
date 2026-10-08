#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <limits>

// Characters in the play session (Architecture §9.6: CharacterController:Move, IsGrounded, GetGroundNormal, GetVelocity; the
// inner body seen by sensors and raycasts; character contacts as events). "Physics: trigger detects a character through its
// inner body" is a Roadmap M11 acceptance name, used verbatim. Skipped skeletons of the M11 contract
// (Docs/Decisions/0014-m11-decisions.md): stream C implements the characters and removes the skips.

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
		TEST_CASE("Physics: trigger detects a character through its inner body" * doctest::skip(true))
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
			Test::RunTicks(*session, 120);
			const UUID player = Test::GetEntityId(session->GetScene(), "/Player");
			const std::vector<PhysicsEvent> entered = listener.Find(goal.GetUUID(), PhysicsEventType::TriggerEnter);
			REQUIRE(entered.size() == 1);
			CHECK(entered[0].Other == player);
			CHECK(listener.Find(player, PhysicsEventType::TriggerEnter).size() == 1);
			CHECK(Test::GetWorldPosition(*session, "/Player").x > 6.0f);
		}

		TEST_CASE("PhysicsSystem: MoveCharacter moves the character for one step and its pose is written back" * doctest::skip(true))
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
		}

		TEST_CASE("PhysicsSystem: a character reports its grounded state, ground normal and velocity" * doctest::skip(true))
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

		TEST_CASE("PhysicsSystem: a Transform write teleports a character" * doctest::skip(true))
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

		TEST_CASE("PhysicsSystem: a trigger attached to a character never reports its own character" * doctest::skip(true))
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

		TEST_CASE("PhysicsSystem: a character's contacts give collision events with the same rules as bodies" * doctest::skip(true))
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
	}

}
