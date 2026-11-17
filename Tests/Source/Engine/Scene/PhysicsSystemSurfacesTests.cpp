#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/ColliderSurfaceComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/PhysicsSurfaceValidation.h"
#include "Engine/Scene/PhysicsValidation.h"
#include "Engine/Scene/Scene.h"
#include "Support/PhysicsTestScene.h"

#include <cmath>
#include <optional>
#include <vector>

// The collider surfaces of the play session (M36, Docs/Decisions/0053-m36-contact-surfaces.md decisions 3 to 5; Architecture
// §9.2 "Collider surfaces"): a ColliderSurface's material and velocity in whatever body gathers its colliders, written
// without recomposing the scene, the ground collider of characters and Physics.GetSurface. Three of the cases are Roadmap
// M36 acceptance names.

namespace Engine {

	namespace {

		// "/Level": a Static RigidBody gathering two slabs side by side, their tops at y 0 (World Tree's one static compound,
		// brief 15): "/Level/Marble" at x in [-4, 0] of the body's material (friction 0.5) and "/Level/Ice" at x in [0, 4]
		// with a ColliderSurface of friction 0.02.
		Entity AddLevel(Scene& scene)
		{
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			static_cast<void>(Test::AddBoxBody(scene, "Marble", glm::vec3(-2.0f, -0.5f, 0.0f), glm::vec3(2.0f, 0.5f, 10.0f), std::nullopt, level));
			Entity ice = Test::AddBoxBody(scene, "Ice", glm::vec3(2.0f, -0.5f, 0.0f), glm::vec3(2.0f, 0.5f, 10.0f), std::nullopt, level);
			ice.AddComponent<ColliderSurfaceComponent>(ColliderSurfaceComponent{ .Friction = 0.02f });
			return level;
		}

		// "/Belt": an implicit static box 20 m long along X, its top at y 0, with a ColliderSurface at rest, and "/Crate", a
		// Dynamic box resting on it.
		void AddBeltAndCrate(Scene& scene)
		{
			Entity belt = Test::AddBoxBody(scene, "Belt", glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(10.0f, 0.25f, 1.0f), std::nullopt);
			belt.AddComponent<ColliderSurfaceComponent>(ColliderSurfaceComponent{});
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 0.25f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));
		}

		Scope<PlaySession> StartSurfaceSession(Test::SceneTestFixture& fixture, IPlaySessionObserver* observer = nullptr)
		{
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Observer = observer;
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			return std::move(*session);
		}

		// Writes `velocity` into the session's "/Belt" ColliderSurface, as a script's write of the proxy field.
		void SetBeltVelocity(PlaySession& session, const glm::vec3& velocity)
		{
			session.GetScene().FindEntityByPath("/Belt").Patch<ColliderSurfaceComponent>([&velocity](ColliderSurfaceComponent& surface)
			{
				surface.SurfaceVelocity = velocity;
			});
		}

		// Moves "/Player" at `velocity` in every fixed update, as a script's CharacterController:Move in OnFixedUpdate.
		class Walker final : public IPlaySessionObserver
		{
		public:
			glm::vec3 Velocity = glm::vec3(0.0f);

			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				if (phase != PlaySessionPhase::FixedUpdate)
					return;
				const UUID player = Test::GetEntityId(session.GetScene(), "/Player");
				REQUIRE(session.GetPhysics().MoveCharacter(player, Velocity).has_value());
			}
		};

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsSystem: shards struck alike travel farther on ice than on marble of one static compound")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddLevel(scene));
			// Two shards struck alike along +Z, one on each piece (friction sqrt(0.5 * 0.5) against sqrt(0.02 * 0.5)).
			for (const auto& [name, x] : { std::pair{ "OnMarble", -2.0f }, std::pair{ "OnIce", 2.0f } })
			{
				Entity shard = Test::AddBoxBody(scene, name, glm::vec3(x, 0.1f, -9.0f), glm::vec3(0.1f), BodyType::Dynamic);
				Test::PatchRigidBody(shard, [](RigidBodyComponent& body)
				{
					body.InitialLinearVelocity = glm::vec3(0.0f, 0.0f, 5.0f);
				});
			}
			// One compound, so no seam to warn about, and the surface applies to a collider.
			CHECK(ValidateScenePhysics(scene, PhysicsLayerTable(), nullptr).empty());
			CHECK(ValidateSceneSurfaces(scene, PhysicsLayerTable()).empty());

			Scope<PlaySession> session = StartSurfaceSession(fixture);
			Test::RunTicks(*session, 240);
			const float marble = Test::GetWorldPosition(*session, "/OnMarble").z + 9.0f;
			const float ice = Test::GetWorldPosition(*session, "/OnIce").z + 9.0f;
			CHECK(marble > 1.0f);
			CHECK(ice > 3.0f * marble);
			// Both at rest on the floor, the ice shard still on the ice.
			CHECK(Test::GetWorldPosition(*session, "/OnIce").z < 10.0f);
			CHECK(Test::GetWorldPosition(*session, "/OnIce").y == doctest::Approx(0.1f).epsilon(0.25));
		}

		TEST_CASE("PhysicsSystem: a surface velocity carries a resting crate and wakes the debris asleep on a stopped belt when it starts")
		{
			Test::SceneTestFixture fixture;
			AddBeltAndCrate(fixture.GetScene());
			Scope<PlaySession> session = StartSurfaceSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID crate = Test::GetEntityId(session->GetScene(), "/Crate");
			Test::RunTicks(*session, 120);
			REQUIRE(physics.IsSleeping(crate).value_or(false));

			// The belt starts: its write wakes the crate asleep on it, and the crate rides at the belt's speed.
			SetBeltVelocity(*session, glm::vec3(2.0f, 0.0f, 0.0f));
			session->Tick();
			CHECK_FALSE(physics.IsSleeping(crate).value_or(true));
			Test::RunTicks(*session, 60);
			CHECK(physics.GetLinearVelocity(crate).value_or(glm::vec3(0.0f)).x == doctest::Approx(2.0f).epsilon(0.02));
			const float carried = Test::GetWorldPosition(*session, "/Crate").x;
			CHECK(carried > 1.5f);

			// It stops: friction stops the crate with it. It reverses: the crate rides back.
			SetBeltVelocity(*session, glm::vec3(0.0f));
			Test::RunTicks(*session, 30);
			CHECK(std::abs(physics.GetLinearVelocity(crate).value_or(glm::vec3(1.0f)).x) < 0.01f);
			SetBeltVelocity(*session, glm::vec3(-2.0f, 0.0f, 0.0f));
			Test::RunTicks(*session, 60);
			CHECK(physics.GetLinearVelocity(crate).value_or(glm::vec3(0.0f)).x == doctest::Approx(-2.0f).epsilon(0.02));
			CHECK(Test::GetWorldPosition(*session, "/Crate").x < carried);
		}

		TEST_CASE("PhysicsSystem: a gathered collider's surface velocity follows its own axes, never its body's")
		{
			// "/Line" (a Static RigidBody turned a quarter turn about +Y) gathers "/Line/Belt", turned back by its own quarter
			// turn, so the belt's axes are the world's: its (2, 0, 0) moves along world +X, which is the line's local +Z.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const glm::quat quarter = glm::normalize(glm::quat(0.70710678f, 0.0f, 0.70710678f, 0.0f));
			Entity line = scene.CreateEntity("Line");
			line.Patch<TransformComponent>([&quarter](TransformComponent& transform)
			{
				transform.Rotation = quarter;
			});
			line.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			Entity belt = Test::AddBoxBody(scene, "Belt", glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(10.0f, 0.25f, 1.0f), std::nullopt, line);
			belt.Patch<TransformComponent>([&quarter](TransformComponent& transform)
			{
				transform.Rotation = glm::conjugate(quarter);
			});
			belt.AddComponent<ColliderSurfaceComponent>(ColliderSurfaceComponent{ .SurfaceVelocity = glm::vec3(2.0f, 0.0f, 0.0f) });
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 0.25f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));
			Scope<PlaySession> session = StartSurfaceSession(fixture);
			Test::RunTicks(*session, 90);
			const glm::vec3 velocity = session->GetPhysics().GetLinearVelocity(Test::GetEntityId(session->GetScene(), "/Crate")).value_or(glm::vec3(0.0f));
			CHECK(velocity.x == doctest::Approx(2.0f).epsilon(0.02));
			CHECK(std::abs(velocity.z) < 0.02f);
			// Physics.GetSurface reads the same velocity in world space.
			const std::optional<PhysicsSurfaceInfo> surface = session->GetPhysics().GetSurface(Test::GetEntityId(session->GetScene(), "/Line/Belt"));
			REQUIRE(surface.has_value());
			CHECK(Test::ApproxEqual(surface->SurfaceVelocity, glm::vec3(2.0f, 0.0f, 0.0f), 1.0e-4f));
		}

		TEST_CASE("PhysicsSystem: a surface write keeps the body's handle and shape and composes nothing")
		{
			// "/Belt" is a Static RigidBody gathering "/Belt/Rail", a collider without a surface. Writes the system cannot
			// see (no signal: a component changed in place) to the belt's RigidBody and BoxCollider stay unapplied as long
			// as nothing composes the scene, rebuilds the body or replaces its shape.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity belt = Test::AddBoxBody(scene, "Belt", glm::vec3(0.0f, -0.25f, 0.0f), glm::vec3(10.0f, 0.25f, 1.0f), BodyType::Static);
			belt.AddComponent<ColliderSurfaceComponent>(ColliderSurfaceComponent{});
			static_cast<void>(Test::AddBoxBody(scene, "Rail", glm::vec3(0.0f, 0.5f, 1.0f), glm::vec3(10.0f, 0.25f, 0.1f), std::nullopt, belt));
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 0.25f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));
			Scope<PlaySession> session = StartSurfaceSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID rail = Test::GetEntityId(session->GetScene(), "/Belt/Rail");
			const UUID beltId = Test::GetEntityId(session->GetScene(), "/Belt");
			Test::RunTicks(*session, 10);
			const std::optional<Aabb> bounds = physics.GetBodyBounds(beltId);
			REQUIRE(bounds.has_value());
			REQUIRE(physics.GetSurface(rail).has_value());
			CHECK(physics.GetSurface(rail)->Friction == 0.5f);

			Entity runtimeBelt = session->GetScene().FindEntityByPath("/Belt");
			runtimeBelt.GetComponent<RigidBodyComponent>().Friction = 0.25f;
			runtimeBelt.GetComponent<BoxColliderComponent>().HalfExtents = glm::vec3(10.0f, 0.5f, 1.0f);
			// A belt written every tick, a new value each time.
			for (uint32_t tick = 0; tick < 30; ++tick)
			{
				SetBeltVelocity(*session, glm::vec3(0.1f * static_cast<float>(tick), 0.0f, 0.0f));
				session->Tick();
			}
			CHECK(physics.GetSurface(rail)->Friction == 0.5f);
			const std::optional<Aabb> after = physics.GetBodyBounds(beltId);
			REQUIRE(after.has_value());
			CHECK(after->Min == bounds->Min);
			CHECK(after->Max == bounds->Max);
			CHECK(physics.GetLinearVelocity(Test::GetEntityId(session->GetScene(), "/Crate")).value_or(glm::vec3(0.0f)).x > 1.0f);

			// The control: a write the system sees composes, and both unseen writes take effect with it.
			runtimeBelt.Patch<RigidBodyComponent>([](RigidBodyComponent& body)
			{
				body.Friction = 0.25f;
			});
			session->Tick();
			CHECK(physics.GetSurface(rail)->Friction == 0.25f);
			CHECK(physics.GetBodyBounds(beltId)->Min.y < bounds->Min.y);
		}

		TEST_CASE("PhysicsSystem: GetSurface reads a collider's surface or its body's material, and a character its ground collider")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddLevel(scene));
			Entity player = scene.CreateEntity("Player");
			player.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(2.0f, 0.05f, 0.0f);
			});
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			Entity sensor = Test::AddBoxBody(scene, "Sensor", glm::vec3(0.0f, 2.0f, 8.0f), glm::vec3(0.5f), std::nullopt);
			sensor.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			sensor.AddComponent<ColliderSurfaceComponent>(ColliderSurfaceComponent{});
			Entity hidden = Test::AddBoxBody(scene, "Hidden", glm::vec3(0.0f, -5.0f, 0.0f), glm::vec3(0.5f), std::nullopt);
			hidden.AddComponent<ColliderSurfaceComponent>(ColliderSurfaceComponent{ .Friction = 1.0f });
			hidden.SetActive(false);
			Walker walker;
			Scope<PlaySession> session = StartSurfaceSession(fixture, &walker);
			PhysicsSystem& physics = session->GetPhysics();
			const Scene& runtime = session->GetScene();
			const UUID ice = Test::GetEntityId(runtime, "/Level/Ice");
			const UUID marble = Test::GetEntityId(runtime, "/Level/Marble");
			const UUID character = Test::GetEntityId(runtime, "/Player");
			Test::RunTicks(*session, 30);

			// The ice's surface, the marble's body material, nothing for a character, a trigger or a disabled entity.
			const std::optional<PhysicsSurfaceInfo> onIce = physics.GetSurface(ice);
			REQUIRE(onIce.has_value());
			CHECK(onIce->Friction == 0.02f);
			CHECK(onIce->Restitution == 0.0f);
			CHECK(onIce->SurfaceVelocity == glm::vec3(0.0f));
			const std::optional<PhysicsSurfaceInfo> onMarble = physics.GetSurface(marble);
			REQUIRE(onMarble.has_value());
			CHECK(onMarble->Friction == 0.5f);
			CHECK_FALSE(physics.GetSurface(character).has_value());
			CHECK_FALSE(physics.GetSurface(Test::GetEntityId(runtime, "/Sensor")).has_value());
			CHECK_FALSE(physics.GetSurface(Test::GetEntityId(runtime, "/Hidden")).has_value());
			CHECK_FALSE(physics.GetSurface(Test::GetEntityId(runtime, "/Level")).has_value());

			// The character on the ice reports the ice, from its own update, and physics.bodyInfo's report the same.
			Result<PhysicsCharacterState> state = physics.GetCharacterState(character);
			REQUIRE(state.has_value());
			REQUIRE(state->IsGrounded);
			CHECK(state->GroundCollider == ice);
			const std::optional<PhysicsBodyReport> report = physics.GetBodyInfo(character);
			REQUIRE(report.has_value());
			REQUIRE(report->Character.has_value());
			CHECK(report->Character->GroundCollider == ice);

			// Walked onto the marble.
			walker.Velocity = glm::vec3(-3.0f, 0.0f, 0.0f);
			Test::RunTicks(*session, 60);
			state = physics.GetCharacterState(character);
			REQUIRE(state.has_value());
			CHECK(state->GroundCollider == marble);

			// A ground disabled under the character is no ground collider any more.
			walker.Velocity = glm::vec3(0.0f);
			session->GetScene().FindEntityByPath("/Level/Marble").SetActive(false);
			CHECK_FALSE(physics.GetCharacterState(character)->GroundCollider.IsValid());
		}

		// M36 W36.2 (Docs/Decisions/0053-m36-contact-surfaces.md decision 12): the session names the press's entity, in Play and
		// in Simulate, and physics.bodyInfo's report the same.
		TEST_CASE("PhysicsSystem: a squeezed character reports the press's entity, and a Simulate session's press too")
		{
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				CAPTURE(mode == PlayMode::Play);
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				static_cast<void>(Test::AddGround(scene));
				Entity player = scene.CreateEntity("Player");
				player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
				static_cast<void>(Test::AddBoxBody(scene, "Press", glm::vec3(0.0f, 2.35f, 0.0f), glm::vec3(1.0f, 0.25f, 1.0f), BodyType::Kinematic));
				PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, 42, mode);
				Result<Scope<PlaySession>> started = Test::StartPhysicsSession(fixture, specification);
				REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
				PlaySession& session = **started;
				PhysicsSystem& physics = session.GetPhysics();
				const UUID character = Test::GetEntityId(session.GetScene(), "/Player");
				const UUID press = Test::GetEntityId(session.GetScene(), "/Press");
				Test::RunTicks(session, 10);
				CHECK_FALSE(physics.GetCharacterState(character)->IsSqueezed);
				CHECK_FALSE(physics.GetCharacterState(character)->SqueezedBy.IsValid());
				// The press comes down at 1 m/s, as a script's MoveKinematic in every fixed update would bring it.
				bool squeezed = false;
				for (uint32_t tick = 0; tick < 40 && !squeezed; ++tick)
				{
					const float y = 2.35f - static_cast<float>(tick + 1) / 60.0f;
					REQUIRE(physics.MoveKinematic(press, glm::vec3(0.0f, y, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
					session.Tick();
					squeezed = physics.GetCharacterState(character)->IsSqueezed;
				}
				REQUIRE(squeezed);
				CHECK(physics.GetCharacterState(character)->SqueezedBy == press);
				const std::optional<PhysicsBodyReport> report = physics.GetBodyInfo(character);
				REQUIRE(report.has_value());
				REQUIRE(report->Character.has_value());
				CHECK(report->Character->IsSqueezed);
				CHECK(report->Character->SqueezedBy == press);
				// A press disabled meanwhile is named by nobody.
				session.GetScene().FindEntityByPath("/Press").SetActive(false);
				CHECK(physics.GetCharacterState(character)->IsSqueezed);
				CHECK_FALSE(physics.GetCharacterState(character)->SqueezedBy.IsValid());
				session.GetScene().FindEntityByPath("/Press").SetActive(true);
				CHECK(physics.GetCharacterState(character)->SqueezedBy == press);
				// A press destroyed in a tick that squeezes (its body squeezes in the PreStep and goes at the destroy flush) is
				// named by nobody after it, and the squeeze is still reported.
				REQUIRE(physics.MoveKinematic(press, glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
				session.GetScene().DestroyEntity(session.GetScene().FindEntityByPath("/Press"));
				session.Tick();
				CHECK(session.GetScene().FindEntityByPath("/Press").IsValid() == false);
				REQUIRE(physics.GetCharacterState(character).has_value());
				CHECK(physics.GetCharacterState(character)->IsSqueezed);
				CHECK_FALSE(physics.GetCharacterState(character)->SqueezedBy.IsValid());
			}
		}
	}

}
