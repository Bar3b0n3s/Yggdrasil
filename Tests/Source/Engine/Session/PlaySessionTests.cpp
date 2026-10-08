#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Core/Json/Json.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// PlaySession (Architecture §5.6, §5.7; Roadmap M7 acceptance; Docs/Decisions/0012-m7-decisions.md decisions 2 and 3):
// the serializer copy, the documented step order, the entity cap, the state hash and the session's seeded ids.

namespace Engine {

	namespace {

		// Records every phase the session reports (IPlaySessionObserver) and, when asked, moves an entity in the phases where
		// scripts will act (M13).
		class RecordingObserver final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& /*session*/, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				Phases.push_back(phase);
			}

			std::vector<PlaySessionPhase> Phases;
		};

		// Records, in every fixed update, whether the step view saw Space pressed and released.
		class InputProbe final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				if (phase != PlaySessionPhase::FixedUpdate)
					return;
				const InputState& devices = session.GetInput().GetDevices();
				Pressed.push_back(devices.WasKeyPressed(InputPhase::Step, Key::Space));
				Released.push_back(devices.WasKeyReleased(InputPhase::Step, Key::Space));
			}

			std::vector<bool> Pressed;
			std::vector<bool> Released;
		};

		// Destroys "/Board" in the first fixed update (a script's Destroy, M13) and counts the entities marked for destruction
		// when the destroy flush starts and when the post-step transform update starts.
		class DestroyingObserver final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				Scene& scene = session.GetScene();
				if (phase == PlaySessionPhase::FixedUpdate && tick == 0)
					scene.DestroyEntity(scene.FindEntityByPath("/Board"));
				else if (phase == PlaySessionPhase::DestroyFlush)
					PendingAtFlush = CountPending(scene);
				else if (phase == PlaySessionPhase::PostStepTransformUpdate)
					PendingAfterFlush = CountPending(scene);
			}

			size_t PendingAtFlush = 0;
			size_t PendingAfterFlush = 0;
		private:
			static size_t CountPending(Scene& scene)
			{
				const auto pending = scene.GetRegistry().view<PendingDestroyTag>();
				return static_cast<size_t>(std::distance(pending.begin(), pending.end()));
			}
		};

		// The specification of a session over the fixture's registry.
		PlaySessionSpecification MakeSpecification(Test::SceneTestFixture& fixture, IPlaySessionObserver* observer = nullptr)
		{
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = 42;
			specification.Observer = observer;
			return specification;
		}

		// A small edit scene: a root with a child and a moved root.
		void PopulateScene(Scene& scene)
		{
			Entity board = scene.CreateEntity("Board");
			static_cast<void>(scene.CreateEntity("Cell", board));
			Entity ball = scene.CreateEntity("Ball");
			ball.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: play then stop leaves the edit scene byte-identical")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			const Result<std::string> before = SceneSerializer::SaveToString(fixture.GetScene());
			REQUIRE(before.has_value());
			const uint64_t revisionBefore = fixture.GetScene().GetRevision();
			{
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				// The session changes its own scene: none of it reaches the edit scene.
				Entity ball = (*session)->GetScene().FindEntityByPath("/Ball");
				REQUIRE(ball.IsValid());
				ball.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(9.0f);
				});
				REQUIRE((*session)->CreateEntity("Spawned").has_value());
				for (int tick = 0; tick < 10; ++tick)
					(*session)->Tick();
				CHECK((*session)->GetTick() == 10);
			}
			const Result<std::string> after = SceneSerializer::SaveToString(fixture.GetScene());
			REQUIRE(after.has_value());
			CHECK(*after == *before);
			CHECK(fixture.GetScene().GetRevision() == revisionBefore);
		}

		TEST_CASE("PlaySession: step order matches the documented sequence")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			RecordingObserver observer;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture, &observer), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());

			(*session)->Tick();
			// §5.7: the fixed step (0 to 9, TransformSystem::Update before PhysicsSystem::PreStep), then the frame phase.
			const std::vector<PlaySessionPhase> expected = {
				PlaySessionPhase::InterpolationSnapshot,
				PlaySessionPhase::ApplyInput,
				PlaySessionPhase::StartFlush,
				PlaySessionPhase::FixedUpdate,
				PlaySessionPhase::Tasks,
				PlaySessionPhase::PreStepTransformUpdate,
				PlaySessionPhase::PhysicsPreStep,
				PlaySessionPhase::PhysicsStep,
				PlaySessionPhase::PhysicsPostStep,
				PlaySessionPhase::DestroyFlush,
				PlaySessionPhase::PostStepTransformUpdate,
				PlaySessionPhase::LatchFrame,
				PlaySessionPhase::FrameStartFlush,
				PlaySessionPhase::Update,
				PlaySessionPhase::LateUpdate,
				PlaySessionPhase::FrameDestroyFlush,
				PlaySessionPhase::FrameTransformUpdate,
				PlaySessionPhase::AudioUpdate,
				PlaySessionPhase::RenderExtraction,
			};
			CHECK(observer.Phases == expected);

			// FixedStep alone runs the fixed step only; FrameUpdate alone the frame phase only.
			observer.Phases.clear();
			(*session)->FixedStep();
			CHECK(observer.Phases == std::vector<PlaySessionPhase>(expected.begin(), expected.begin() + 11));
			observer.Phases.clear();
			(*session)->FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 0.5, .FrameIndex = 0 });
			CHECK(observer.Phases == std::vector<PlaySessionPhase>(expected.begin() + 11, expected.end()));
		}

		TEST_CASE("PlaySession: Simulate mode skips the script and audio phases")
		{
			Test::SceneTestFixture fixture;
			RecordingObserver observer;
			PlaySessionSpecification specification = MakeSpecification(fixture, &observer);
			specification.Mode = PlayMode::Simulate;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			(*session)->Tick();
			const std::vector<PlaySessionPhase> expected = {
				PlaySessionPhase::InterpolationSnapshot,
				PlaySessionPhase::ApplyInput,
				PlaySessionPhase::PreStepTransformUpdate,
				PlaySessionPhase::PhysicsPreStep,
				PlaySessionPhase::PhysicsStep,
				PlaySessionPhase::PhysicsPostStep,
				PlaySessionPhase::DestroyFlush,
				PlaySessionPhase::PostStepTransformUpdate,
				PlaySessionPhase::LatchFrame,
				PlaySessionPhase::FrameDestroyFlush,
				PlaySessionPhase::FrameTransformUpdate,
				PlaySessionPhase::RenderExtraction,
			};
			CHECK(observer.Phases == expected);
		}

		TEST_CASE("PlaySession: entity cap raises an error, never crashes")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene()); // 3 entities
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.Project.Simulation.MaxEntities = 4;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlaySession& play = **session;
			CHECK(play.GetMaxEntities() == 4);
			CHECK(play.CheckEntityCapacity(1).has_value());
			REQUIRE(play.CreateEntity("Fourth").has_value());

			const Result<Entity> fifth = play.CreateEntity("Fifth");
			REQUIRE_FALSE(fifth.has_value());
			CHECK(fifth.error().GetCode() == ErrorCode::InvalidState);
			CHECK(fifth.error().GetMessageText().find("entity limit 4 reached") != std::string::npos);
			CHECK(play.GetScene().GetEntityCount() == 4);
			CHECK_FALSE(play.CheckEntityCapacity(1).has_value());
			play.Tick(); // the session keeps running after a refused spawn

			// A document over the cap does not start.
			specification.Project.Simulation.MaxEntities = 2;
			const Result<Scope<PlaySession>> tooMany = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_FALSE(tooMany.has_value());
			CHECK(tooMany.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("PlaySession: identical sessions report identical state hashes, different seeds different ones")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			const auto run = [&fixture](uint64_t seed)
			{
				PlaySessionSpecification specification = MakeSpecification(fixture);
				specification.Seed = seed;
				Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				REQUIRE((*session)->CreateEntity("Spawned").has_value());
				for (int tick = 0; tick < 30; ++tick)
					(*session)->Tick();
				static_cast<void>((*session)->GetRandom().NextU64());
				return (*session)->ComputeStateHash();
			};
			const uint64_t first = run(7);
			CHECK(first != 0);
			CHECK(run(7) == first);
			CHECK(run(8) != first);
		}

		TEST_CASE("PlaySession: runtime spawns get the seeded deterministic ids of the session")
		{
			Test::SceneTestFixture fixture;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			const Result<Entity> spawned = (*session)->CreateEntity("Spawned");
			REQUIRE(spawned.has_value());
			UUIDGenerator expected = UUIDGenerator::CreateDeterministic(42);
			CHECK(spawned->GetUUID() == expected.Next());
		}

		TEST_CASE("PlaySession: a session starts only from a strictly valid document")
		{
			Test::SceneTestFixture fixture;
			Json document = Json::object();
			document["Format"] = "Scene";
			document["Version"] = 1;
			document["Entities"] = "not an array";
			const Result<Scope<PlaySession>> session = PlaySession::Create(MakeSpecification(fixture), document);
			REQUIRE_FALSE(session.has_value());
			CHECK(session.error().GetCode() == ErrorCode::Validation);

			PlaySessionSpecification noRegistry = MakeSpecification(fixture);
			noRegistry.Registry = nullptr;
			const Result<Scope<PlaySession>> invalid = PlaySession::CreateFromScene(noRegistry, fixture.GetScene());
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PlaySession: the project settings are checked and kept with the session")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			PlaySessionSpecification specification = MakeSpecification(fixture);
			specification.Project.Simulation.FixedHz = 0;
			const Result<Scope<PlaySession>> noRate = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_FALSE(noRate.has_value());
			CHECK(noRate.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(noRate.error().GetMessageText().contains("FixedHz"));

			// A 30 Hz project: every tick lasts 1/30 s, and the session keeps its copy of the settings.
			specification.Project.Simulation.FixedHz = 30;
			specification.Project.Simulation.MaxStepsPerFrame = 3;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			CHECK((*session)->GetFixedDelta() == doctest::Approx(1.0 / 30.0));
			CHECK((*session)->GetProjectSettings().Simulation.FixedHz == 30);
			CHECK((*session)->GetProjectSettings().Simulation.MaxStepsPerFrame == 3);

			// The other members a session cannot run without.
			for (const auto& [member, change] : std::vector<std::pair<std::string, void (*)(PlaySessionSpecification&)>>{
					 { "MaxStepsPerFrame", [](PlaySessionSpecification& changed)
			{
				changed.Project.Simulation.MaxStepsPerFrame = 0;
			} },
					 { "MaxEntities", [](PlaySessionSpecification& changed)
			{
				changed.Project.Simulation.MaxEntities = 0;
			} },
					 { "ViewWidth", [](PlaySessionSpecification& changed)
			{
				changed.ViewWidth = 0;
			} } })
			{
				CAPTURE(member);
				PlaySessionSpecification invalid = MakeSpecification(fixture);
				change(invalid);
				const Result<Scope<PlaySession>> refused = PlaySession::CreateFromScene(invalid, fixture.GetScene());
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(refused.error().GetMessageText().contains(member));
			}
		}

		TEST_CASE("PlaySession: the input stamped for a tick is applied at its step 1, before its fixed update")
		{
			Test::SceneTestFixture fixture;
			InputProbe probe;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture, &probe), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			PlayInputEvent space;
			space.Type = PlayInputEventType::KeyInput;
			space.KeyCode = Key::Space;
			space.State = PlayInputEventState::Tap;
			REQUIRE((*session)->GetInput().Queue(1, space).has_value());
			for (int tick = 0; tick < 4; ++tick)
				(*session)->Tick();
			// Down at tick 1, up at tick 2 (a tap), seen by that tick's fixed update.
			CHECK(probe.Pressed == std::vector<bool>{ false, true, false, false });
			CHECK(probe.Released == std::vector<bool>{ false, false, true, false });
			CHECK((*session)->GetInput().GetNextTick() == 4);
		}

		TEST_CASE("PlaySession: paused and lockstep sessions advance only through Tick")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			const FrameTime frame{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 0.5, .FrameIndex = 0 };

			// A running session follows the host's loop.
			session.AdvanceLoopStep();
			session.AdvanceLoopFrame(frame);
			CHECK(session.GetTick() == 1);
			CHECK(session.GetExtractionCount() == 1);
			CHECK(session.GetViewAlpha() == doctest::Approx(0.5f));

			session.SetPaused(true);
			session.AdvanceLoopStep();
			session.AdvanceLoopFrame(frame);
			CHECK(session.GetTick() == 1);
			CHECK(session.GetExtractionCount() == 1);
			CHECK(session.GetViewAlpha() == 1.0f);

			session.SetPaused(false);
			session.SetLockstep(true, 5);
			CHECK(session.IsLockstep());
			CHECK(session.GetLockstepOwner() == 5);
			session.AdvanceLoopStep();
			session.AdvanceLoopFrame(frame);
			CHECK(session.GetTick() == 1);
			session.Tick();
			CHECK(session.GetTick() == 2);
			CHECK(session.GetExtractionCount() == 2);

			// Leaving lockstep keeps the paused state, and forgets the owner.
			session.SetLockstep(false, 5);
			CHECK(session.GetLockstepOwner() == NoClient);
			CHECK_FALSE(session.IsPaused());
			session.SetExtractionEnabled(false);
			session.Tick();
			CHECK(session.GetExtractionCount() == 2);
			CHECK(session.GetViewAlpha() == 1.0f);
		}

		TEST_CASE("PlaySession: entities destroyed in a tick are gone after its destroy flush")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			DestroyingObserver destroyer;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture, &destroyer), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			(*session)->FixedStep();
			CHECK(destroyer.PendingAtFlush == 2); // the board and its cell, marked in the fixed update
			CHECK(destroyer.PendingAfterFlush == 0);
			CHECK((*session)->GetScene().GetEntityCount() == 1);
			CHECK_FALSE((*session)->GetScene().FindEntityByPath("/Board").IsValid());
		}

		TEST_CASE("PlaySession: the state hash covers the scene, the tick, the random stream and the spawn counter")
		{
			Test::SceneTestFixture fixture;
			PopulateScene(fixture.GetScene());
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			std::vector<uint64_t> hashes = { session.ComputeStateHash() };
			session.Tick();
			hashes.push_back(session.ComputeStateHash());
			static_cast<void>(session.GetRandom().NextU64());
			hashes.push_back(session.ComputeStateHash());
			static_cast<void>(session.GetIdGenerator().Next());
			hashes.push_back(session.ComputeStateHash());
			Entity ball = session.GetScene().FindEntityByPath("/Ball");
			ball.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 5.0f;
			});
			hashes.push_back(session.ComputeStateHash());
			for (size_t first = 0; first < hashes.size(); ++first)
			{
				for (size_t second = first + 1; second < hashes.size(); ++second)
					CHECK(hashes[first] != hashes[second]);
			}
			// Reading the hash changes nothing.
			CHECK(session.ComputeStateHash() == hashes.back());
		}

		TEST_CASE("PlaySession: the run state is kept as set, and the time scale is checked")
		{
			Test::SceneTestFixture fixture;
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlaySession& session = **created;
			CHECK(session.GetMode() == PlayMode::Play);
			CHECK(session.GetSeed() == 42);
			CHECK(session.GetTimeScale() == 1.0);
			REQUIRE(session.SetTimeScale(0.0).has_value());
			REQUIRE(session.SetTimeScale(PlaySession::MaxTimeScale).has_value());
			for (const double invalid : { -0.5, PlaySession::MaxTimeScale + 1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
			{
				CAPTURE(invalid);
				const Status refused = session.SetTimeScale(invalid);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK(session.GetTimeScale() == PlaySession::MaxTimeScale);

			CHECK_FALSE(session.IsStepping());
			session.SetStepping(true);
			CHECK(session.IsStepping());
			CHECK_FALSE(session.IsModified());
			session.MarkModified();
			CHECK(session.IsModified());
			CHECK(session.IsExtractionEnabled());
			session.SetViewSize(320, 180);
			CHECK_FALSE(session.GetLastExtraction().HasCamera);
		}

		TEST_CASE("PlaySession: the session seed is Project Seed xor Scene Seed")
		{
			// Implemented by the contract (a constexpr in the frozen header).
			static_assert(PlaySession::ComputeSessionSeed(1337u, 42u) == (1337u ^ 42u));
			CHECK(PlaySession::ComputeSessionSeed(0u, 0u) == 0u);
			CHECK(PlaySession::ComputeSessionSeed(0xffffffffu, 0x0000ffffu) == 0xffff0000u);
		}

		TEST_CASE("PlaySession: the physics phases simulate bodies in Simulate mode as in Play mode")
		{
			// M11 (Docs/Decisions/0014-m11-decisions.md decision 14): Simulate is Play without scripts and audio, so a falling box
			// lands in both, at the same place.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Box", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			std::vector<glm::vec3> landed;
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture, 42, mode));
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				Test::RunTicks(**session, 120);
				landed.push_back(Test::GetWorldPosition(**session, "/Box"));
			}
			CHECK(landed[0].y == doctest::Approx(0.48f).epsilon(0.02));
			CHECK(landed[1] == landed[0]);
		}

		TEST_CASE("PlaySession: the state hash covers the bodies' velocities")
		{
			// §5.1 "+ physics velocities at runtime": a velocity that has not moved anything yet changes the hash.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Box", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			const uint64_t before = (*session)->ComputeStateHash();
			const UUID box = Test::GetEntityId((*session)->GetScene(), "/Box");
			REQUIRE((*session)->GetPhysics().SetLinearVelocity(box, glm::vec3(1.0f, 0.0f, 0.0f)).has_value());
			CHECK((*session)->ComputeStateHash() != before);
		}

		TEST_CASE("PlaySession: physics settings that cannot make a world fail the start with its context")
		{
			Test::SceneTestFixture fixture;
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Project.Physics.Layers = { "Track" };
			specification.Project.Physics.Collisions = {};
			const Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_FALSE(session.has_value());
			CHECK(session.error().GetCode() == ErrorCode::Validation);
			CHECK(session.error().ToString().contains("while starting the play session"));
		}

		TEST_CASE("PlaySessionPhase: every phase has its enumerator name")
		{
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::InterpolationSnapshot) == "InterpolationSnapshot");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::PreStepTransformUpdate) == "PreStepTransformUpdate");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::RenderExtraction) == "RenderExtraction");
		}
	}

}
