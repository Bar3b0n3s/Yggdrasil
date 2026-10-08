#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"

#include <cmath>
#include <limits>
#include <tuple>

// The play session's physics (Architecture §9.2 to §9.4, §5.7 steps 5 to 8; §9.7 and Roadmap M11 acceptance): composition
// into bodies, the PreStep/PostStep sync, sorted events and synthesized exits, through PlaySession (lockstep ticks, Play
// mode). The names starting "Physics:" are Roadmap acceptance names, used verbatim; the others name their unit. The name
// function is complete; the
// rest are skipped skeletons of the M11 contract (Docs/Decisions/0014-m11-decisions.md): stream B implements the system and
// removes the skips (the trigger-detects-a-character acceptance is stream C's, PhysicsSystemCharactersTests.cpp).

namespace Engine {

	namespace {

		// Starts a lockstep-style session of `fixture`'s scene and installs `listener`.
		Scope<PlaySession> StartSession(Test::SceneTestFixture& fixture, Test::RecordingPhysicsListener* listener = nullptr, IPlaySessionObserver* observer = nullptr)
		{
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Observer = observer;
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			if (listener != nullptr)
				(*session)->GetPhysics().SetEventListener(listener);
			return std::move(*session);
		}

		// Calls `action` at the start of every phase `phase` of a tick in [first, last].
		class PhaseAction final : public IPlaySessionObserver
		{
		public:
			PhaseAction(PlaySessionPhase phase, uint64_t first, uint64_t last, std::function<void(PlaySession&, uint64_t)> action)
				: m_Phase(phase), m_First(first), m_Last(last), m_Action(std::move(action))
			{
			}

			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				if (phase == m_Phase && tick >= m_First && tick <= m_Last)
					m_Action(session, tick);
			}
		private:
			PlaySessionPhase m_Phase = PlaySessionPhase::FixedUpdate;
			uint64_t m_First = 0;
			uint64_t m_Last = 0;
			std::function<void(PlaySession&, uint64_t)> m_Action{};
		};

		// A track of `count` adjacent 1 m boxes along +X (centres x = 0 .. count - 1, top face y = 0.5): one static compound
		// under a level root when `shared`, else `count` separate static bodies.
		void AddTrack(Scene& scene, int count, bool shared)
		{
			Entity level = scene.CreateEntity("Level");
			if (shared)
				level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			for (int piece = 0; piece < count; ++piece)
			{
				static_cast<void>(Test::AddBoxBody(scene, "Piece", glm::vec3(static_cast<float>(piece), 0.0f, 0.0f), glm::vec3(0.5f),
					shared ? std::nullopt : std::optional<BodyType>(BodyType::Static), level));
			}
		}

		// The largest |v.y| of "/Ball" while a 0.5 m sphere rolls along the track at 6 m/s (§9.2 "Seams"). It REQUIREs, so
		// callers keep it out of CHECK expressions.
		float MeasureRollingBump(bool shared)
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 20, shared);
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 1.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
			{
				body.MotionQuality = MotionQuality::LinearCast;
				body.EnhancedInternalEdgeRemoval = true;
				body.Friction = 0.8f;
				body.MaxAngularVelocity = 120.0f;
				body.AllowSleeping = false;
			});
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID id = ball.GetUUID();
			// Settle on the first piece, then roll.
			Test::RunTicks(*session, 30);
			REQUIRE(physics.SetLinearVelocity(id, glm::vec3(6.0f, 0.0f, 0.0f)).has_value());
			REQUIRE(physics.SetAngularVelocity(id, glm::vec3(0.0f, 0.0f, -12.0f)).has_value());
			float largest = 0.0f;
			for (int tick = 0; tick < 240 && Test::GetWorldPosition(*session, "/Ball").x < 18.5f; ++tick)
			{
				session->Tick();
				largest = std::max(largest, std::abs(physics.GetLinearVelocity(id).value_or(glm::vec3(0.0f)).y));
			}
			CHECK(Test::GetWorldPosition(*session, "/Ball").x >= 18.5f);
			return largest;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsEventType: every type has its enumerator name")
		{
			CHECK(PhysicsEventTypeToString(PhysicsEventType::CollisionEnter) == "CollisionEnter");
			CHECK(PhysicsEventTypeToString(PhysicsEventType::CollisionExit) == "CollisionExit");
			CHECK(PhysicsEventTypeToString(PhysicsEventType::TriggerEnter) == "TriggerEnter");
			CHECK(PhysicsEventTypeToString(PhysicsEventType::TriggerExit) == "TriggerExit");
		}

		TEST_CASE("PhysicsSystem: the session's physics follows the project's gravity")
		{
			Test::SceneTestFixture fixture;
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Project.Physics.Gravity = glm::vec3(0.0f, -20.0f, 1.0f);
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			CHECK(Test::ApproxEqual((*session)->GetPhysics().GetGravity(), glm::vec3(0.0f, -20.0f, 1.0f)));
		}

		TEST_CASE("PhysicsSystem: Create refuses a missing scene, a zero FixedHz and invalid layers" * doctest::skip(true))
		{
			CHECK(PhysicsSystem::Create({}).error().GetCode() == ErrorCode::InvalidArgument);
			Test::SceneTestFixture fixture;
			PhysicsSystemSpecification zeroHz;
			zeroHz.RuntimeScene = &fixture.GetScene();
			zeroHz.FixedHz = 0;
			CHECK(PhysicsSystem::Create(zeroHz).error().GetCode() == ErrorCode::InvalidArgument);
			PhysicsSystemSpecification badLayers;
			badLayers.RuntimeScene = &fixture.GetScene();
			badLayers.Layers = { "Track" };
			CHECK(PhysicsSystem::Create(badLayers).error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("PhysicsSystem: bodies exist from tick 0 and Dynamic bodies fall and write their pose back" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			// A dynamic box under a moved and rotated static parent: the write-back goes through the parent's inverse.
			Entity parent = scene.CreateEntity("Parent");
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
				transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			});
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic, parent));
			Scope<PlaySession> session = StartSession(fixture);
			const UUID box = Test::GetEntityId(session->GetScene(), "/Parent/Box");
			REQUIRE(session->GetPhysics().GetBodyInfo(box).has_value());
			CHECK(session->GetPhysics().GetStats().BodyCount == 2);

			Test::RunTicks(*session, 240);
			const std::optional<PhysicsBodyReport> info = session->GetPhysics().GetBodyInfo(box);
			REQUIRE(info.has_value());
			CHECK(info->Pose.Position.y == doctest::Approx(0.48f).epsilon(0.02));
			// The entity's world pose is the body's; its local translation is relative to the rotated parent.
			CHECK(Test::ApproxEqual(Test::GetWorldPosition(*session, "/Parent/Box"), info->Pose.Position, 1.0e-4f));
			const Entity entity = session->GetScene().FindEntityByID(box);
			CHECK(entity.GetComponent<TransformComponent>().Translation.y == doctest::Approx(0.48f).epsilon(0.02));
		}

		TEST_CASE("Physics: trigger detects a sleeping body" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Sleeper", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID sleeper = Test::GetEntityId(session->GetScene(), "/Sleeper");
			Test::RunTicks(*session, 300);
			REQUIRE(physics.IsSleeping(sleeper).value_or(false));

			// A checkpoint appears around the sleeping box: an implicit sensor body, created at the next PreStep.
			Result<Entity> checkpoint = session->CreateEntity("Checkpoint");
			REQUIRE(checkpoint.has_value());
			checkpoint->AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(2.0f), .IsTrigger = true });
			listener.Events.clear();
			Test::RunTicks(*session, 2);
			const UUID trigger = checkpoint->GetUUID();
			CHECK(listener.Find(trigger, PhysicsEventType::TriggerEnter).size() == 1);
			const std::vector<PhysicsEvent> entered = listener.Find(sleeper, PhysicsEventType::TriggerEnter);
			REQUIRE(entered.size() == 1);
			CHECK(entered[0].Other == trigger);
			// Detecting it does not wake it.
			CHECK(physics.IsSleeping(sleeper).value_or(false));
		}

		TEST_CASE("PhysicsSystem: a Static RigidBody trigger detects a sleeping body" * doctest::skip(true))
		{
			// The variant of "Physics: trigger detects a sleeping body" with an authored Static RigidBody: it is planned as a
			// Kinematic sensor, kept active (§9.2), so it sees the sleeping box too.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Sleeper", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID sleeper = Test::GetEntityId(session->GetScene(), "/Sleeper");
			Test::RunTicks(*session, 300);
			REQUIRE(physics.IsSleeping(sleeper).value_or(false));

			Result<Entity> goal = session->CreateEntity("Goal");
			REQUIRE(goal.has_value());
			goal->AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(2.0f), .IsTrigger = true });
			goal->AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			listener.Events.clear();
			Test::RunTicks(*session, 2);
			const std::vector<PhysicsEvent> entered = listener.Find(sleeper, PhysicsEventType::TriggerEnter);
			REQUIRE(entered.size() == 1);
			CHECK(entered[0].Other == goal->GetUUID());
			const std::optional<PhysicsBodyReport> report = physics.GetBodyInfo(goal->GetUUID());
			REQUIRE(report.has_value());
			CHECK(report->MotionType == PhysicsMotionType::Kinematic);
			CHECK(report->IsSensor);
			CHECK(physics.IsSleeping(sleeper).value_or(false));
		}

		TEST_CASE("PhysicsSystem: an attached trigger never reports its own body" * doctest::skip(true))
		{
			// A trigger child of a moving ball (§5.3) overlaps the ball all the time; their shared collision group keeps the pair
			// out, so the trigger reports only others.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 0.5f, 0.0f), 0.5f, BodyType::Dynamic);
			Entity aura = Test::AddSphereBody(scene, "Aura", glm::vec3(0.0f), 2.0f, std::nullopt, ball);
			aura.Patch<SphereColliderComponent>([](SphereColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(1.5f, 0.25f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 30);
			const UUID crate = Test::GetEntityId(session->GetScene(), "/Crate");
			const std::vector<PhysicsEvent> auraEnters = listener.Find(aura.GetUUID(), PhysicsEventType::TriggerEnter);
			REQUIRE(auraEnters.size() == 1);
			CHECK(auraEnters[0].Other == crate);
			CHECK(listener.Find(ball.GetUUID(), PhysicsEventType::TriggerEnter).empty());
		}

		TEST_CASE("PhysicsSystem: trigger enter and exit are reported exactly once for both entities" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity gate = Test::AddBoxBody(scene, "Gate", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(2.0f, 0.5f, 2.0f), std::nullopt);
			gate.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 10.0f, 0.0f), 0.25f, BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 120);
			const UUID gateId = gate.GetUUID();
			const UUID ballId = Test::GetEntityId(session->GetScene(), "/Ball");
			for (const UUID self : { gateId, ballId })
			{
				CHECK(listener.Find(self, PhysicsEventType::TriggerEnter).size() == 1);
				CHECK(listener.Find(self, PhysicsEventType::TriggerExit).size() == 1);
				CHECK(listener.Find(self, PhysicsEventType::CollisionEnter).empty());
			}
			const std::vector<PhysicsEvent> exits = listener.Find(ballId, PhysicsEventType::TriggerExit);
			REQUIRE(exits.size() == 1);
			CHECK_FALSE(exits[0].Synthesized);
		}

		TEST_CASE("Physics: kinematic platform carries a resting box" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(scene, "Platform", glm::vec3(0.0f), glm::vec3(2.0f, 0.1f, 2.0f), BodyType::Kinematic));
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 0.6f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			// The platform moves along +X at 1 m/s, driven like a script's RigidBody:MoveKinematic in OnFixedUpdate.
			PhaseAction mover(PlaySessionPhase::FixedUpdate, 60, 179, [](PlaySession& session, uint64_t tick)
			{
				const UUID platform = Test::GetEntityId(session.GetScene(), "/Platform");
				const float x = static_cast<float>(tick - 59) / 60.0f;
				REQUIRE(session.GetPhysics().MoveKinematic(platform, glm::vec3(x, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
			});
			Scope<PlaySession> session = StartSession(fixture, nullptr, &mover);
			Test::RunTicks(*session, 60);
			const float restX = Test::GetWorldPosition(*session, "/Crate").x;
			Test::RunTicks(*session, 120);
			CHECK(Test::GetWorldPosition(*session, "/Platform").x == doctest::Approx(2.0f).epsilon(1.0e-3));
			// The crate rode along (within the friction's slip) and stayed on top.
			CHECK(Test::GetWorldPosition(*session, "/Crate").x - restX == doctest::Approx(2.0f).epsilon(0.1));
			CHECK(Test::GetWorldPosition(*session, "/Crate").y > 0.5f);
		}

		TEST_CASE("Physics: destroy inside a contact callback is safe" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 2.0f, 0.0f), 0.5f, BodyType::Dynamic));
			static_cast<void>(Test::AddSphereBody(scene, "Other", glm::vec3(3.0f, 2.0f, 0.0f), 0.5f, BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PlaySession* sessionPointer = session.get();
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			const UUID ground = Test::GetEntityId(session->GetScene(), "/Ground");
			// The ball destroys itself when it lands, as a script's OnCollisionEnter would (deferred, §5.7).
			listener.OnEvent = [sessionPointer, ball](const PhysicsEvent& event)
			{
				if (event.Type == PhysicsEventType::CollisionEnter && event.Self == ball)
				{
					Entity entity = sessionPointer->GetScene().FindEntityByID(ball);
					if (entity.IsValid())
						sessionPointer->GetScene().DestroyEntity(entity);
				}
			};
			Test::RunTicks(*session, 90);
			CHECK_FALSE(session->GetScene().FindEntityByID(ball).IsValid());
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(ball).has_value());
			// The ground received the landing's enter (its Self stayed alive), and the destroy flush closed the pair: one
			// synthesized exit for the ground.
			const std::vector<PhysicsEvent> exits = listener.Find(ground, PhysicsEventType::CollisionExit);
			REQUIRE(exits.size() == 1);
			CHECK(exits[0].Other == ball);
			CHECK(exits[0].Synthesized);
			// The rest of the scene keeps simulating.
			CHECK(session->GetPhysics().GetBodyInfo(Test::GetEntityId(session->GetScene(), "/Other")).has_value());
			CHECK(session->GetPhysics().GetStats().BodyCount == 2);
		}

		TEST_CASE("Physics: sphere rolling across 20 boxes of one compound keeps |vy| < 0.05" * doctest::skip(true))
		{
			const float bump = MeasureRollingBump(true);
			CHECK(bump < 0.05f);
		}

		TEST_CASE("PhysicsSystem: a roll across 20 separate static bodies bumps at the seams" * doctest::skip(true))
		{
			// The control case of §9.2: what the shared static compound avoids.
			const float bump = MeasureRollingBump(false);
			CHECK(bump >= 0.05f);
		}

		TEST_CASE("Physics: compound contact names the child collider" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 3, true);
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(2.0f, 3.0f, 0.0f), 0.25f, BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 90);
			const Scene& runtime = session->GetScene();
			const UUID ball = Test::GetEntityId(runtime, "/Ball");
			const UUID level = Test::GetEntityId(runtime, "/Level");
			const UUID third = Test::GetEntityId(runtime, "/Level/Piece[2]");
			const std::vector<PhysicsEvent> ballEnter = listener.Find(ball, PhysicsEventType::CollisionEnter);
			REQUIRE(ballEnter.size() == 1);
			// `other` is the body owner; the contact names the piece that was hit (§9.4).
			CHECK(ballEnter[0].Other == level);
			CHECK(ballEnter[0].Contact.Collider == ball);
			CHECK(ballEnter[0].Contact.OtherCollider == third);
			CHECK(ballEnter[0].Contact.Normal.y < -0.9f);
			const std::vector<PhysicsEvent> levelEnter = listener.Find(level, PhysicsEventType::CollisionEnter);
			REQUIRE(levelEnter.size() == 1);
			CHECK(levelEnter[0].Contact.Collider == third);
			CHECK(levelEnter[0].Contact.OtherCollider == ball);
			CHECK(levelEnter[0].Contact.Normal.y > 0.9f);
		}

		TEST_CASE("Physics: destroying or disabling a partner synthesizes exits" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Doomed", glm::vec3(-3.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Napper", glm::vec3(3.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(3.0f, 0.5f, 0.0f), glm::vec3(1.5f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Scene& runtime = session->GetScene();
			const UUID ground = Test::GetEntityId(runtime, "/Ground");
			const UUID doomed = Test::GetEntityId(runtime, "/Doomed");
			const UUID napper = Test::GetEntityId(runtime, "/Napper");
			const UUID zoneId = Test::GetEntityId(runtime, "/Zone");
			Test::RunTicks(*session, 30);
			REQUIRE(listener.Find(zoneId, PhysicsEventType::TriggerEnter).size() == 1);
			listener.Events.clear();

			// Destroyed: the ground receives OnCollisionExit with an invalid `other`.
			runtime.DestroyEntity(runtime.FindEntityByID(doomed));
			// Disabled: the zone receives OnTriggerExit with an inactive `other`.
			runtime.FindEntityByID(napper).SetActive(false);
			session->Tick();
			const std::vector<PhysicsEvent> groundExits = listener.Find(ground, PhysicsEventType::CollisionExit);
			REQUIRE(groundExits.size() == 2);
			const auto doomedExit = std::find_if(groundExits.begin(), groundExits.end(), [doomed](const PhysicsEvent& event)
			{
				return event.Other == doomed;
			});
			REQUIRE(doomedExit != groundExits.end());
			CHECK(doomedExit->Synthesized);
			CHECK_FALSE(runtime.FindEntityByID(doomed).IsValid());
			const std::vector<PhysicsEvent> zoneExits = listener.Find(zoneId, PhysicsEventType::TriggerExit);
			REQUIRE(zoneExits.size() == 1);
			CHECK(zoneExits[0].Other == napper);
			CHECK(zoneExits[0].Synthesized);
			CHECK_FALSE(runtime.FindEntityByID(napper).IsActive());
			// No callback goes to the destroyed or disabled entity itself.
			CHECK(listener.Find(doomed, PhysicsEventType::CollisionExit).empty());
			CHECK(listener.Find(napper, PhysicsEventType::TriggerExit).empty());
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(napper).has_value());

			// Enabled again, the body is back at the next PreStep and the zone sees it enter once more.
			runtime.FindEntityByID(napper).SetActive(true);
			listener.Events.clear();
			Test::RunTicks(*session, 2);
			CHECK(session->GetPhysics().GetBodyInfo(napper).has_value());
			CHECK(listener.Find(zoneId, PhysicsEventType::TriggerEnter).size() == 1);
		}

		TEST_CASE("Physics: Transform write in OnFixedUpdate teleports the body in the same tick" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddSphereBody(fixture.GetScene(), "Ball", glm::vec3(0.0f, 10.0f, 0.0f), 0.5f, BodyType::Dynamic));
			// In the fixed update of tick 30 a script writes the ball's Transform.
			PhaseAction writer(PlaySessionPhase::FixedUpdate, 30, 30, [](PlaySession& session, uint64_t /*tick*/)
			{
				Entity ball = session.GetScene().FindEntityByPath("/Ball");
				ball.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 20.0f, 0.0f);
			});
			Scope<PlaySession> session = StartSession(fixture, nullptr, &writer);
			Test::RunTicks(*session, 30);
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			const glm::vec3 velocityBefore = session->GetPhysics().GetLinearVelocity(ball).value_or(glm::vec3(0.0f));
			session->Tick();
			// The body moved to the written pose in tick 30 itself (then fell for one step), keeping its velocity.
			const glm::vec3 position = session->GetPhysics().GetBodyInfo(ball).value_or(PhysicsBodyReport{}).Pose.Position;
			CHECK(position.x == doctest::Approx(5.0f));
			CHECK(position.y == doctest::Approx(20.0f + (velocityBefore.y - 9.81f / 60.0f) / 60.0f).epsilon(1.0e-3));
			CHECK(session->GetPhysics().GetLinearVelocity(ball).value_or(glm::vec3(0.0f)).y < velocityBefore.y);
		}

		TEST_CASE("Physics: invalid shapes, all-DOF-locked and Dynamic-trigger bodies give diagnostics, never asserts" * doctest::skip(true))
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A mesh collider without a mesh (no Mesh and no MeshRenderer): PHYSICS_INVALID_SHAPE.
			Entity mesh = scene.CreateEntity("NoMesh");
			mesh.AddComponent<MeshColliderComponent>(MeshColliderComponent{}).Convex = true;
			mesh.AddComponent<RigidBodyComponent>(RigidBodyComponent{});
			// A character whose capsule has no cylinder.
			Entity squat = scene.CreateEntity("Squat");
			squat.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{ .Height = 0.4f, .Radius = 0.3f });
			Entity locked = Test::AddBoxBody(scene, "Locked", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(locked, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});
			Entity trigger = Test::AddBoxBody(scene, "DynamicTrigger", glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			trigger.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddSphereBody(scene, "Fine", glm::vec3(6.0f, 0.0f, 0.0f), 0.5f, BodyType::Dynamic));
			// Settings Jolt itself would assert on, which are valid content and simulate (§9.1): a Kinematic mesh body and an
			// implicit mesh trigger in the same place (Jolt has no mesh-versus-mesh collision), a Kinematic body with every
			// degree of freedom locked, and a starting velocity above the maximum.
			const TypedAssetHandle<AssetType::Mesh> cube(BuiltinAssetHandles::CubeMesh);
			Entity kinematicMesh = scene.CreateEntity("KinematicMesh");
			kinematicMesh.AddComponent<MeshColliderComponent>(MeshColliderComponent{ .Mesh = cube });
			kinematicMesh.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Kinematic });
			Entity meshTrigger = scene.CreateEntity("MeshTrigger");
			meshTrigger.AddComponent<MeshColliderComponent>(MeshColliderComponent{ .Mesh = cube, .IsTrigger = true });
			Entity lockedKinematic = Test::AddBoxBody(scene, "LockedKinematic", glm::vec3(9.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Kinematic);
			Test::PatchRigidBody(lockedKinematic, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});
			Entity fast = Test::AddSphereBody(scene, "Fast", glm::vec3(12.0f, 0.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(fast, [](RigidBodyComponent& body)
			{
				body.GravityFactor = 0.0f;
				body.LinearDamping = 0.0f;
				body.InitialLinearVelocity = glm::vec3(600.0f, 0.0f, 0.0f);
			});

			Test::ExpectLog invalidShape(LogLevel::Error, "PHYSICS_INVALID_SHAPE");
			Test::ExpectLog allLocked(LogLevel::Error, "PHYSICS_ALL_DOFS_LOCKED");
			Test::ExpectLog dynamicTrigger(LogLevel::Error, "PHYSICS_DYNAMIC_TRIGGER");
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Assets = &assets.GetManager();
			Result<Scope<PlaySession>> started = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			Scope<PlaySession> session = std::move(*started);
			Test::RunTicks(*session, 10);
			const PhysicsSystem& physics = session->GetPhysics();
			const auto hasDiagnostic = [&physics](UUID entity, std::string_view code)
			{
				const std::span<const PhysicsDiagnostic> diagnostics = physics.GetDiagnostics();
				return std::any_of(diagnostics.begin(), diagnostics.end(), [entity, code](const PhysicsDiagnostic& diagnostic)
				{
					return diagnostic.Entity == entity && diagnostic.Code == code;
				});
			};
			CHECK(hasDiagnostic(mesh.GetUUID(), PhysicsInvalidShapeCode));
			CHECK(hasDiagnostic(squat.GetUUID(), PhysicsInvalidShapeCode));
			CHECK(hasDiagnostic(locked.GetUUID(), PhysicsAllDofsLockedCode));
			CHECK(hasDiagnostic(trigger.GetUUID(), PhysicsDynamicTriggerCode));
			for (const UUID refused : { mesh.GetUUID(), squat.GetUUID(), locked.GetUUID(), trigger.GetUUID() })
				CHECK_FALSE(physics.GetBodyInfo(refused).has_value());
			// Each diagnostic is raised once, however many ticks run; the valid body simulates.
			CHECK(invalidShape.GetMatchCount() == 2);
			CHECK(allLocked.GetMatchCount() == 1);
			CHECK(dynamicTrigger.GetMatchCount() == 1);
			CHECK(physics.GetBodyInfo(Test::GetEntityId(session->GetScene(), "/Fine")).has_value());
			// The bodies Jolt would have asserted on exist, without a diagnostic; the fast start was clamped to the maximum.
			for (const UUID valid : { kinematicMesh.GetUUID(), meshTrigger.GetUUID(), lockedKinematic.GetUUID(), fast.GetUUID() })
			{
				CHECK(physics.GetBodyInfo(valid).has_value());
				CHECK_FALSE(hasDiagnostic(valid, PhysicsInvalidShapeCode));
				CHECK_FALSE(hasDiagnostic(valid, PhysicsAllDofsLockedCode));
			}
			CHECK(physics.GetLinearVelocity(fast.GetUUID()).value_or(glm::vec3(0.0f)).x == doctest::Approx(500.0f));
		}

		TEST_CASE("PhysicsSystem: changing a RigidBody or a collider rebuilds the body at the next PreStep" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Box", glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			Entity box = session->GetScene().FindEntityByPath("/Box");
			const UUID id = box.GetUUID();
			box.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.HalfExtents = glm::vec3(2.0f);
			});
			session->Tick();
			const std::optional<Aabb> bounds = physics.GetBodyBounds(id);
			REQUIRE(bounds.has_value());
			CHECK(bounds->GetSize().x == doctest::Approx(4.0f).epsilon(1.0e-3));
			box.Patch<RigidBodyComponent>([](RigidBodyComponent& body)
			{
				body.Type = BodyType::Static;
			});
			session->Tick();
			const float y = Test::GetWorldPosition(*session, "/Box").y;
			Test::RunTicks(*session, 30);
			CHECK(Test::GetWorldPosition(*session, "/Box").y == doctest::Approx(y));
			CHECK(physics.GetBodyInfo(id).value_or(PhysicsBodyReport{}).MotionType == PhysicsMotionType::Static);
			// Removing the RigidBody leaves the collider as an implicit static body (§5.3).
			box.RemoveComponent<RigidBodyComponent>();
			session->Tick();
			CHECK(physics.GetBodyInfo(id).value_or(PhysicsBodyReport{}).Origin == PhysicsBodyOrigin::ImplicitStatic);
		}

		TEST_CASE("PhysicsSystem: reparenting and scale changes rebuild the bodies they affect" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 1, true);
			static_cast<void>(Test::AddBoxBody(scene, "Loose", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt));
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			Scene& runtime = session->GetScene();
			Entity level = runtime.FindEntityByPath("/Level");
			Entity loose = runtime.FindEntityByPath("/Loose");
			REQUIRE(physics.GetBodyInfo(loose.GetUUID()).value_or(PhysicsBodyReport{}).Owner == loose.GetUUID());
			// Moved under the level root, the loose piece joins its compound.
			REQUIRE(runtime.SetParent(loose, level).has_value());
			session->Tick();
			const std::optional<PhysicsBodyReport> joined = physics.GetBodyInfo(loose.GetUUID());
			REQUIRE(joined.has_value());
			CHECK(joined->Owner == level.GetUUID());
			CHECK(joined->Colliders.size() == 2);
			// Scale is baked into shapes: scaling the root rebuilds its compound twice as large (x from -1 to 11).
			level.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f);
			});
			session->Tick();
			const std::optional<Aabb> bounds = physics.GetBodyBounds(level.GetUUID());
			REQUIRE(bounds.has_value());
			CHECK(bounds->GetSize().x == doctest::Approx(12.0f).epsilon(1.0e-3));
		}

		TEST_CASE("PhysicsSystem: a rebuild keeps the body's pairs and velocity, and a removal ends its pairs with synthesized exits" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Entity drifter = Test::AddSphereBody(scene, "Drifter", glm::vec3(0.0f, 20.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(drifter, [](RigidBodyComponent& body)
			{
				body.GravityFactor = 0.0f;
				body.LinearDamping = 0.0f;
				body.AllowSleeping = false;
			});
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PhysicsSystem& physics = session->GetPhysics();
			Scene& runtime = session->GetScene();
			Entity crate = runtime.FindEntityByPath("/Crate");
			const UUID ground = Test::GetEntityId(runtime, "/Ground");
			Test::RunTicks(*session, 30);
			REQUIRE(listener.Find(crate.GetUUID(), PhysicsEventType::CollisionEnter).size() == 1);
			listener.Events.clear();

			// A collider change replaces the shape in place; a RigidBody change creates the body again. Neither ends nor
			// restarts the pair (pairs are keyed by owner, §9.4).
			crate.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.HalfExtents = glm::vec3(0.5f, 0.5f, 0.45f);
			});
			session->Tick();
			Test::PatchRigidBody(crate, [](RigidBodyComponent& body)
			{
				body.Friction = 0.9f;
			});
			Test::RunTicks(*session, 2);
			CHECK(listener.Events.empty());
			const std::optional<PhysicsBodyReport> rebuilt = physics.GetBodyInfo(crate.GetUUID());
			REQUIRE(rebuilt.has_value());
			CHECK(rebuilt->Contacts.size() == 1);

			// A body created again keeps its current velocity, not the RigidBody's InitialLinearVelocity.
			const UUID drifterId = drifter.GetUUID();
			REQUIRE(physics.SetLinearVelocity(drifterId, glm::vec3(3.0f, 0.0f, 0.0f)).has_value());
			Test::PatchRigidBody(runtime.FindEntityByID(drifterId), [](RigidBodyComponent& body)
			{
				body.AngularDamping = 0.1f;
			});
			session->Tick();
			CHECK(physics.GetLinearVelocity(drifterId).value_or(glm::vec3(0.0f)).x == doctest::Approx(3.0f));

			// Removing the crate's only collider removes its body: no contact re-establishes the pair, which ends with a
			// synthesized exit for both sides.
			crate.RemoveComponent<BoxColliderComponent>();
			session->Tick();
			CHECK_FALSE(physics.GetBodyInfo(crate.GetUUID()).has_value());
			const std::vector<PhysicsEvent> groundExits = listener.Find(ground, PhysicsEventType::CollisionExit);
			REQUIRE(groundExits.size() == 1);
			CHECK(groundExits[0].Other == crate.GetUUID());
			CHECK(groundExits[0].Synthesized);
			CHECK(listener.Find(crate.GetUUID(), PhysicsEventType::CollisionExit).size() == 1);
		}

		TEST_CASE("PhysicsSystem: an exit callback that destroys an entity in contact still synthesizes that entity's exits" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Doomed", glm::vec3(-3.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Chained", glm::vec3(3.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Scene& runtime = session->GetScene();
			const UUID ground = Test::GetEntityId(runtime, "/Ground");
			const UUID doomed = Test::GetEntityId(runtime, "/Doomed");
			const UUID chained = Test::GetEntityId(runtime, "/Chained");
			Test::RunTicks(*session, 30);
			listener.Events.clear();
			// The ground's exit from the doomed box destroys the chained box, which also rests on the ground, as a script's
			// OnCollisionExit would: the same flush must close that pair too.
			Scene* runtimePointer = &runtime;
			listener.OnEvent = [runtimePointer, doomed, chained](const PhysicsEvent& event)
			{
				if (event.Type != PhysicsEventType::CollisionExit || event.Other != doomed)
					return;
				Entity entity = runtimePointer->FindEntityByID(chained);
				if (entity.IsValid())
					runtimePointer->DestroyEntity(entity);
			};
			runtime.DestroyEntity(runtime.FindEntityByID(doomed));
			session->Tick();
			const std::vector<PhysicsEvent> exits = listener.Find(ground, PhysicsEventType::CollisionExit);
			REQUIRE(exits.size() == 2);
			CHECK(std::all_of(exits.begin(), exits.end(), [](const PhysicsEvent& event)
			{
				return event.Synthesized;
			}));
			CHECK(std::any_of(exits.begin(), exits.end(), [chained](const PhysicsEvent& event)
			{
				return event.Other == chained;
			}));
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(chained).has_value());
			CHECK(session->GetPhysics().GetStats().BodyCount == 1);
		}

		TEST_CASE("PhysicsSystem: an entity destroyed earlier in the tick gets no enter and leaves no exit" * doctest::skip(true))
		{
			// The ball starts inside the zone; OnFixedUpdate of tick 0 destroys it, so the step's enter has a pending entity:
			// dropped for both sides, and nothing is synthesized at the flush (§9.4 step 3).
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(0.0f), glm::vec3(2.0f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f), 0.5f, BodyType::Dynamic));
			PhaseAction destroyer(PlaySessionPhase::FixedUpdate, 0, 0, [](PlaySession& session, uint64_t /*tick*/)
			{
				session.GetScene().DestroyEntity(session.GetScene().FindEntityByPath("/Ball"));
			});
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener, &destroyer);
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			session->Tick();
			CHECK(listener.Events.empty());
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(ball).has_value());
		}

		TEST_CASE("PhysicsSystem: a rotated Dynamic body comes to rest and falls asleep" * doctest::skip(true))
		{
			// The teleport check compares the local values PostStep wrote, bit for bit; a comparison through the world matrix
			// would see rounding in the decomposed rotation and teleport (and wake) the box every tick.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			box.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			});
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 300);
			CHECK(session->GetPhysics().IsSleeping(box.GetUUID()).value_or(false));
		}

		TEST_CASE("PhysicsSystem: a Dynamic body under a moving parent stays where it is simulated" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity platform = Test::AddBoxBody(scene, "Platform", glm::vec3(0.0f), glm::vec3(1.0f, 0.1f, 1.0f), BodyType::Kinematic);
			Entity drone = Test::AddSphereBody(scene, "Drone", glm::vec3(0.0f, 5.0f, 0.0f), 0.25f, BodyType::Dynamic, platform);
			Test::PatchRigidBody(drone, [](RigidBodyComponent& body)
			{
				body.GravityFactor = 0.0f;
			});
			PhaseAction mover(PlaySessionPhase::FixedUpdate, 1, 120, [](PlaySession& session, uint64_t tick)
			{
				const UUID id = Test::GetEntityId(session.GetScene(), "/Platform");
				REQUIRE(session.GetPhysics().MoveKinematic(id, glm::vec3(static_cast<float>(tick) / 60.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
			});
			Test::ExpectLog movingParent(LogLevel::Warn, "PHYSICS_DYNAMIC_UNDER_MOVING_PARENT");
			Scope<PlaySession> session = StartSession(fixture, nullptr, &mover);
			Test::RunTicks(*session, 121);
			CHECK(Test::GetWorldPosition(*session, "/Platform").x == doctest::Approx(2.0f).epsilon(1.0e-3));
			// The parent's motion never moved the drone, and PostStep kept its entity where its (sleeping) body is.
			const glm::vec3 position = Test::GetWorldPosition(*session, "/Platform/Drone");
			CHECK(Test::ApproxEqual(position, glm::vec3(0.0f, 5.0f, 0.0f), 1.0e-4f));
			CHECK(Test::ApproxEqual(position, session->GetPhysics().GetBodyInfo(drone.GetUUID()).value_or(PhysicsBodyReport{}).Pose.Position, 1.0e-4f));
		}

		TEST_CASE("PhysicsSystem: a teleported Kinematic body is placed, not swept" * doctest::skip(true))
		{
			// A script teleports the platform in OnFixedUpdate (Transform write plus PlaySession::MarkTeleported, as
			// Transform:Teleport does): PreStep places it with SetPose, so the crate resting on it is not flung.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Platform", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(2.0f, 0.1f, 2.0f), BodyType::Kinematic));
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 1.6f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			PhaseAction teleporter(PlaySessionPhase::FixedUpdate, 60, 60, [](PlaySession& session, uint64_t /*tick*/)
			{
				Entity platform = session.GetScene().FindEntityByPath("/Platform");
				platform.GetComponent<TransformComponent>().Translation = glm::vec3(50.0f, 1.0f, 0.0f);
				session.MarkTeleported(platform);
			});
			Scope<PlaySession> session = StartSession(fixture, nullptr, &teleporter);
			Test::RunTicks(*session, 61);
			CHECK(Test::GetWorldPosition(*session, "/Platform").x == doctest::Approx(50.0f));
			const UUID crate = Test::GetEntityId(session->GetScene(), "/Crate");
			CHECK(std::abs(session->GetPhysics().GetLinearVelocity(crate).value_or(glm::vec3(100.0f)).x) < 0.1f);
			CHECK(std::abs(Test::GetWorldPosition(*session, "/Crate").x) < 0.1f);
		}

		TEST_CASE("PhysicsSystem: the listener hears each diagnostic raised at run time once" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			// An entity created during play with every degree of freedom locked (M13 turns this into a script error).
			Result<Entity> locked = session->CreateEntity("Locked");
			REQUIRE(locked.has_value());
			locked->AddComponent<BoxColliderComponent>(BoxColliderComponent{});
			locked->AddComponent<RigidBodyComponent>(RigidBodyComponent{});
			Test::PatchRigidBody(*locked, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});
			Test::ExpectLog allLocked(LogLevel::Error, "PHYSICS_ALL_DOFS_LOCKED");
			Test::RunTicks(*session, 3);
			REQUIRE(listener.Diagnostics.size() == 1);
			CHECK(listener.Diagnostics[0].Code == PhysicsAllDofsLockedCode);
			CHECK(listener.Diagnostics[0].Entity == locked->GetUUID());
			CHECK(session->GetPhysics().GetDiagnostics().size() == 1);
		}

		TEST_CASE("PhysicsSystem: events are sorted by UUID pair and type and reach both entities, lower UUID first" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			for (int ball = 0; ball < 8; ++ball)
				static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(static_cast<float>(ball) * 1.5f, 1.0f, 0.0f), 0.5f, BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 60);
			REQUIRE(listener.Events.size() >= 16);
			// Events come in pairs (Self = lower UUID first), and the pairs in (lower, higher, type) order within a tick.
			for (size_t index = 0; index + 1 < listener.Events.size(); index += 2)
			{
				const PhysicsEvent& first = listener.Events[index];
				const PhysicsEvent& second = listener.Events[index + 1];
				CHECK(first.Self < first.Other);
				CHECK(second.Self == first.Other);
				CHECK(second.Other == first.Self);
				CHECK(second.Type == first.Type);
				if (index >= 2 && listener.Events[index - 2].Tick == first.Tick)
				{
					const PhysicsEvent& previous = listener.Events[index - 2];
					CHECK(std::tie(previous.Self, previous.Other, previous.Type) < std::tie(first.Self, first.Other, first.Type));
				}
			}
		}

		TEST_CASE("PhysicsSystem: body functions refuse missing bodies, wrong motion types and non-finite values" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 2.0f, 0.0f), 0.5f, BodyType::Dynamic));
			static_cast<void>(scene.CreateEntity("Empty"));
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			const UUID ground = Test::GetEntityId(session->GetScene(), "/Ground");
			const UUID empty = Test::GetEntityId(session->GetScene(), "/Empty");
			CHECK(physics.AddForce(empty, glm::vec3(1.0f)).error().GetCode() == ErrorCode::NotFound);
			CHECK(physics.AddForce(ground, glm::vec3(1.0f)).error().GetCode() == ErrorCode::InvalidState);
			CHECK(physics.MoveKinematic(ball, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).error().GetCode() == ErrorCode::InvalidState);
			CHECK(physics.SetLinearVelocity(ground, glm::vec3(1.0f)).error().GetCode() == ErrorCode::InvalidState);
			const float nan = std::numeric_limits<float>::quiet_NaN();
			CHECK(physics.AddImpulse(ball, glm::vec3(nan)).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.Teleport(ball, glm::vec3(std::numeric_limits<float>::infinity()), std::nullopt).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.SetGravity(glm::vec3(nan)).error().GetCode() == ErrorCode::InvalidArgument);
			// Valid calls act.
			REQUIRE(physics.AddImpulse(ball, glm::vec3(2.0f, 0.0f, 0.0f)).has_value());
			CHECK(physics.GetLinearVelocity(ball).value_or(glm::vec3(0.0f)).x == doctest::Approx(2.0f));
			REQUIRE(physics.Teleport(ball, glm::vec3(0.0f, 8.0f, 0.0f), std::nullopt).has_value());
			CHECK(Test::GetWorldPosition(*session, "/Ball").y == doctest::Approx(8.0f));
			REQUIRE(physics.WakeUp(ball).has_value());
			CHECK_FALSE(physics.IsSleeping(ball).value_or(true));
		}

		TEST_CASE("PhysicsSystem: GetBodyInfo reports a compound child's owner, the layer and the active contacts" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 3, true);
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(1.0f, 1.0f, 0.0f), 0.5f, BodyType::Dynamic));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 30);
			const Scene& runtime = session->GetScene();
			const UUID piece = Test::GetEntityId(runtime, "/Level/Piece[1]");
			const UUID level = Test::GetEntityId(runtime, "/Level");
			const UUID ball = Test::GetEntityId(runtime, "/Ball");
			const std::optional<PhysicsBodyReport> track = session->GetPhysics().GetBodyInfo(piece);
			REQUIRE(track.has_value());
			CHECK(track->Owner == level);
			CHECK(track->Colliders.size() == 3);
			CHECK(track->LayerName == "Default");
			REQUIRE(track->Contacts.size() == 1);
			CHECK(track->Contacts[0].Other == ball);
			CHECK(track->Contacts[0].Collider == piece);
			CHECK_FALSE(track->Contacts[0].IsTrigger);
			const std::optional<PhysicsBodyReport> rolling = session->GetPhysics().GetBodyInfo(ball);
			REQUIRE(rolling.has_value());
			CHECK(rolling->MotionType == PhysicsMotionType::Dynamic);
			CHECK_FALSE(rolling->Character.has_value());
		}
	}

}
