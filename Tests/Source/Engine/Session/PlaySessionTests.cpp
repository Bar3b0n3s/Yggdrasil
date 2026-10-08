#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Core/Json/Json.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/SceneTestFixture.h"

#include <string>
#include <vector>

// PlaySession (Architecture §5.6, §5.7; Roadmap M7 acceptance). Skipped skeletons of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md): stream A implements PlaySession and removes the skips.

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
		TEST_CASE("PlaySession: play then stop leaves the edit scene byte-identical" * doctest::skip(true))
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

		TEST_CASE("PlaySession: step order matches the documented sequence" * doctest::skip(true))
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

		TEST_CASE("PlaySession: Simulate mode skips the script and audio phases" * doctest::skip(true))
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

		TEST_CASE("PlaySession: entity cap raises an error, never crashes" * doctest::skip(true))
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

		TEST_CASE("PlaySession: identical sessions report identical state hashes, different seeds different ones" * doctest::skip(true))
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

		TEST_CASE("PlaySession: runtime spawns get the seeded deterministic ids of the session" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			const Result<Entity> spawned = (*session)->CreateEntity("Spawned");
			REQUIRE(spawned.has_value());
			UUIDGenerator expected = UUIDGenerator::CreateDeterministic(42);
			CHECK(spawned->GetUUID() == expected.Next());
		}

		TEST_CASE("PlaySession: a session starts only from a strictly valid document" * doctest::skip(true))
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

		TEST_CASE("PlaySession: the project settings are checked and kept with the session" * doctest::skip(true))
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
		}

		TEST_CASE("PlaySession: the session seed is Project Seed xor Scene Seed")
		{
			// Implemented by the contract (a constexpr in the frozen header).
			static_assert(PlaySession::ComputeSessionSeed(1337u, 42u) == (1337u ^ 42u));
			CHECK(PlaySession::ComputeSessionSeed(0u, 0u) == 0u);
			CHECK(PlaySession::ComputeSessionSeed(0xffffffffu, 0x0000ffffu) == 0xffff0000u);
		}

		TEST_CASE("PlaySessionPhase: every phase has its enumerator name")
		{
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::InterpolationSnapshot) == "InterpolationSnapshot");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::PreStepTransformUpdate) == "PreStepTransformUpdate");
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::RenderExtraction) == "RenderExtraction");
		}
	}

}
