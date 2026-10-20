#include "TestsPCH.h"
#include "Engine/Session/NativeWorld.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Native/NativeComponents.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"
#include "Support/NativeTestModule.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		class NativeRecordingObserver final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				Phases.push_back(phase);
				// Where the mover is when the transform update before physics starts: NativeStep's writes are in the scene.
				if (phase == PlaySessionPhase::PreStepTransformUpdate && Mover.IsValid())
				{
					if (const Entity mover = session.GetScene().FindEntityByID(Mover); mover.IsValid())
						MoverXBeforeTransformUpdate.push_back(mover.GetComponent<TransformComponent>().Translation.x);
				}
			}

			std::vector<PlaySessionPhase> Phases{};
			UUID Mover{};
			std::vector<float> MoverXBeforeTransformUpdate{};
		};

		// Layout 1 without its Count system: the same scene effects, its resource never changes.
		void RegisterWithoutCount(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<Test::NativeTestSpinner>("Turns its entity about an axis.")
					.Field("DegreesPerSecond", &Test::NativeTestSpinner::DegreesPerSecond, "How fast it turns.", { .Min = -3600.0, .Max = 3600.0, .Unit = "deg/s" })
					.Field("Axis", &Test::NativeTestSpinner::Axis, "The local axis it turns about.")
					.Field("Turns", &Test::NativeTestSpinner::Turns, "The ticks it turned."));
			registrar.Add(NativeResource<Test::NativeTestCounter>("A counter nothing counts with.")
					.Field("Ticks", &Test::NativeTestCounter::Ticks, "Ticks stepped.")
					.Field("Distance", &Test::NativeTestCounter::Distance, "Metres moved.", { .Min = 0.0, .Unit = "m" }));
		}

		void DoNothing(INativeStepContext& /*context*/)
		{
		}

		// A module without resources and with a system that changes nothing.
		void RegisterWithoutResources(INativeRegistrar& registrar)
		{
			registrar.AddSystem({ .Name = "Idle", .Description = "Changes nothing.", .Run = &DoNothing });
		}

		[[nodiscard]] Ref<const NativeModuleTypes> RequireTestTypes(NativeModuleEntryPoint entry)
		{
			Result<Ref<const NativeModuleTypes>> types = BuildNativeModuleTypes(entry, nullptr);
			REQUIRE_MESSAGE(types.has_value(), types.error().ToString());
			return std::move(*types);
		}

		[[nodiscard]] Json ParseJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			return std::move(*json);
		}

		[[nodiscard]] PlaySessionSpecification MakeNativeSpecification(Test::SceneTestFixture& fixture, Ref<const NativeModuleTypes> types,
			IPlaySessionObserver* observer = nullptr)
		{
			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = 42;
			specification.Observer = observer;
			specification.Native = std::move(types);
			InputActionSettings move;
			move.Type = InputActionSettings::ActionType::Axis;
			move.Positive = { "Key.D" };
			move.Negative = { "Key.A" };
			specification.Project.Input.Actions.emplace(std::string(Test::NativeTestMoveAction), std::move(move));
			return specification;
		}

		[[nodiscard]] Scope<PlaySession> RequireSession(const PlaySessionSpecification& specification, const Scene& scene)
		{
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			return std::move(*session);
		}

		// A mover and a spinner with their native components, in `scene`.
		void PopulateNativeScene(Scene& scene, const NativeModuleTypes& types, UUID& mover, UUID& spinner)
		{
			Entity moverEntity = scene.CreateEntity("Mover");
			Entity spinnerEntity = scene.CreateEntity("Spinner");
			REQUIRE(PatchNativeComponents(moverEntity, types, ParseJson(R"({"Native.TestMover": {"Speed": 6}})"), "/components").has_value());
			REQUIRE(PatchNativeComponents(spinnerEntity, types, ParseJson(R"({"Native.TestSpinner": {}})"), "/components").has_value());
			mover = moverEntity.GetUUID();
			spinner = spinnerEntity.GetUUID();
		}

	}

	TEST_SUITE("Session")
	{
		// Docs/Decisions/0030-m19-contract.md decision 28 and the phase 1 ledger's PlaySessionPhase (N-B): NativeStep runs in
		// Play with a module, after Tasks and before PreStepTransformUpdate; Simulate has no native world.
		TEST_CASE("NativeWorld: systems run in the NativeStep phase, after Tasks and before PreStepTransformUpdate" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			NativeRecordingObserver observer;
			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, types, &observer), fixture.GetScene());
			REQUIRE(session->GetNative() != nullptr);
			CHECK(&session->GetNative()->GetTypes() == types.get());
			session->FixedStep();
			const std::vector<PlaySessionPhase> expected = {
				PlaySessionPhase::InterpolationSnapshot,
				PlaySessionPhase::ApplyInput,
				PlaySessionPhase::StartFlush,
				PlaySessionPhase::FixedUpdate,
				PlaySessionPhase::Tasks,
				PlaySessionPhase::NativeStep,
				PlaySessionPhase::PreStepTransformUpdate,
				PlaySessionPhase::PhysicsPreStep,
				PlaySessionPhase::PhysicsStep,
				PlaySessionPhase::PhysicsPostStep,
				PlaySessionPhase::DestroyFlush,
				PlaySessionPhase::PostStepTransformUpdate,
			};
			CHECK(observer.Phases == expected);
			CHECK(PlaySessionPhaseToString(PlaySessionPhase::NativeStep) == "NativeStep");

			NativeRecordingObserver simulateObserver;
			PlaySessionSpecification simulate = MakeNativeSpecification(fixture, types, &simulateObserver);
			simulate.Mode = PlayMode::Simulate;
			const Scope<PlaySession> simulated = RequireSession(simulate, fixture.GetScene());
			CHECK(simulated->GetNative() == nullptr);
			simulated->FixedStep();
			CHECK(std::ranges::find(simulateObserver.Phases, PlaySessionPhase::NativeStep) == simulateObserver.Phases.end());
		}

		// Decision 28 (N-B): a system reads the latched step view of the project's actions, its Transform writes are in the
		// scene when PreStepTransformUpdate begins, and the component data it changed is written back as canonical JSON.
		TEST_CASE("NativeWorld: a system reads the latched step input and its writes reach the scene before the transform update" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			UUID mover{};
			UUID spinner{};
			PopulateNativeScene(fixture.GetScene(), *types, mover, spinner);
			NativeRecordingObserver observer;
			observer.Mover = mover;
			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, types, &observer), fixture.GetScene());

			PlayInputEvent move;
			move.Type = PlayInputEventType::Action;
			move.Name = std::string(Test::NativeTestMoveAction);
			move.HasValue = true;
			move.Value = 1.0f;
			REQUIRE(session->GetInput().Queue(0, move).has_value());
			session->Tick();
			session->Tick();

			// 6 m/s for two ticks of 1/60 s, each step's move in the scene before that step's transform update.
			REQUIRE(observer.MoverXBeforeTransformUpdate.size() == 2);
			CHECK(observer.MoverXBeforeTransformUpdate[0] == doctest::Approx(0.1));
			CHECK(observer.MoverXBeforeTransformUpdate[1] == doctest::Approx(0.2));
			const Entity playSpinner = session->GetScene().FindEntityByID(spinner);
			REQUIRE(playSpinner.IsValid());
			const NativeEntityComponents written = ReadNativeComponents(playSpinner, types.get());
			REQUIRE(written.Registered.size() == 1);
			CHECK(written.Registered[0].second.Get() == ParseJson(R"({"DegreesPerSecond": 90, "Axis": [0, 1, 0], "Turns": 2})"));
			CHECK(playSpinner.GetComponent<TransformComponent>().Rotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			// The edit scene is never touched (§5.6).
			const NativeEntityComponents edit = ReadNativeComponents(fixture.GetScene().FindEntityByID(spinner), types.get());
			REQUIRE(edit.Registered.size() == 1);
			CHECK(edit.Registered[0].second.Get() == ParseJson(R"({"DegreesPerSecond": 90, "Axis": [0, 1, 0], "Turns": 0})"));
		}

		// Decision 28 and the ledger's state-hash block (N-B): the native block holds the module's resources after the
		// physics block, so equal sessions hash equally, a resource's value changes the hash, and a module without resources
		// leaves the hash of a session without a module.
		TEST_CASE("NativeWorld: the native block follows the physics block, hashes the resources and is empty without them" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> full = RequireTestTypes(Test::GetNativeTestModule());
			UUID mover{};
			UUID spinner{};
			PopulateNativeScene(fixture.GetScene(), *full, mover, spinner);

			const Scope<PlaySession> first = RequireSession(MakeNativeSpecification(fixture, full), fixture.GetScene());
			const Scope<PlaySession> second = RequireSession(MakeNativeSpecification(fixture, full), fixture.GetScene());
			const Scope<PlaySession> uncounted = RequireSession(MakeNativeSpecification(fixture, RequireTestTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterWithoutCount })),
				fixture.GetScene());
			for (int tick = 0; tick < 3; ++tick)
			{
				first->Tick();
				second->Tick();
				uncounted->Tick();
			}
			CHECK(first->ComputeStateHash() == second->ComputeStateHash());
			// Without Count the scene is the same and only TestCounter.Ticks differs (3 against 0).
			CHECK(first->ComputeStateHash() != uncounted->ComputeStateHash());
			REQUIRE(first->GetNative() != nullptr);
			CHECK(first->GetNative()->GetResources() == ParseJson(R"({"TestCounter": {"Ticks": 3, "Distance": 0}})"));

			Test::SceneTestFixture empty;
			PlaySessionSpecification bare = MakeNativeSpecification(empty, nullptr);
			const Scope<PlaySession> withoutModule = RequireSession(bare, empty.GetScene());
			const Scope<PlaySession> withoutResources = RequireSession(MakeNativeSpecification(empty, RequireTestTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterWithoutResources })),
				empty.GetScene());
			withoutModule->Tick();
			withoutResources->Tick();
			CHECK(withoutModule->ComputeStateHash() == withoutResources->ComputeStateHash());
		}

		// Decision 28 (N-B): NativeWorld calls each system inside the §4.6 boundary; an exception that escapes one stops the
		// session fatally, naming the system, and no later tick runs.
		TEST_CASE("NativeWorld: an exception that escapes a system stops the session fatally, naming the system" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, RequireTestTypes(Test::GetFailingNativeTestModule())), fixture.GetScene());
			session->Tick();
			REQUIRE(session->GetFatalError().has_value());
			CHECK(session->GetFatalError()->Message.contains("Fail"));
			const uint64_t tick = session->GetTick();
			session->Tick();
			CHECK(session->GetTick() == tick);
		}
	}

}
