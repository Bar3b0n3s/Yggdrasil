#include "TestsPCH.h"
#include "Engine/Session/NativeWorld.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Native/NativeComponents.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayPlayer.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/NativeTestModule.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
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

		// The test module's types (layout 1) without its systems, for modules that bring their own.
		void RegisterTestTypes(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<Test::NativeTestSpinner>("Turns its entity about an axis.")
					.Field("DegreesPerSecond", &Test::NativeTestSpinner::DegreesPerSecond, "How fast it turns.", { .Min = -3600.0, .Max = 3600.0, .Unit = "deg/s" })
					.Field("Axis", &Test::NativeTestSpinner::Axis, "The local axis it turns about.")
					.Field("Turns", &Test::NativeTestSpinner::Turns, "The ticks it turned."));
			registrar.Add(NativeComponent<Test::NativeTestMover>("Moves its entity along +X with the MoveX action.")
					.Field("Speed", &Test::NativeTestMover::Speed, "Metres per second at full input.", { .Min = 0.0, .Unit = "m/s" })
					.Field("Follow", &Test::NativeTestMover::Follow, "An entity it would follow."));
			registrar.Add(NativeResource<Test::NativeTestCounter>("What the systems counted.")
					.Field("Ticks", &Test::NativeTestCounter::Ticks, "Ticks stepped.")
					.Field("Distance", &Test::NativeTestCounter::Distance, "Metres moved.", { .Min = 0.0, .Unit = "m" }));
		}

		// Leaves every TestMover's Speed below its Min of 0.
		void BreakSpeed(INativeStepContext& context)
		{
			for (const NativeEntity entity : context.Query<Test::NativeTestMover>())
			{
				if (Test::NativeTestMover* mover = context.Get<Test::NativeTestMover>(entity); mover != nullptr)
					mover->Speed = -1.0f;
			}
		}

		void RegisterBreakingModule(INativeRegistrar& registrar)
		{
			RegisterTestTypes(registrar);
			registrar.AddSystem({ .Name = "Break", .Description = "Writes a speed below its minimum.", .Run = &BreakSpeed });
		}

		// On tick 0: spawns "Spawned" with a TestMover of speed 4, removes every TestSpinner, destroys "/Doomed", turns a
		// rotation that is not unit length away and counts the active TestMovers in TestCounter.Ticks.
		void Build(INativeStepContext& context)
		{
			if (context.GetTick() != 0)
				return;
			const NativeEntity spawned = context.CreateEntity("Spawned", NativeEntity{});
			CHECK(context.IsValid(spawned));
			if (Test::NativeTestMover* mover = context.Add<Test::NativeTestMover>(spawned); mover != nullptr)
				mover->Speed = 4.0f;
			for (const NativeEntity entity : context.Query<Test::NativeTestSpinner>())
				CHECK(context.Remove<Test::NativeTestSpinner>(entity));
			const NativeEntity doomed = context.FindEntity("/Doomed");
			CHECK(context.DestroyEntity(doomed));
			CHECK_FALSE(context.IsValid(doomed));
			NativeTransform transform{};
			transform.Rotation = glm::quat(2.0f, 0.0f, 0.0f, 0.0f);
			CHECK_FALSE(context.SetTransform(spawned, transform));
			CHECK(context.GetName(spawned) == "Spawned");
			if (Test::NativeTestCounter* counter = context.GetResource<Test::NativeTestCounter>(); counter != nullptr)
				counter->Ticks = static_cast<uint32_t>(context.Query<Test::NativeTestMover>().size());
		}

		void RegisterBuildingModule(INativeRegistrar& registrar)
		{
			RegisterTestTypes(registrar);
			registrar.AddSystem({ .Name = "Build", .Description = "Changes the scene's structure once.", .Run = &Build });
		}

		// Reads an action the project does not define (up and 0) and writes a line; counts in TestCounter.Ticks.
		void Probe(INativeStepContext& context)
		{
			Test::NativeTestCounter* counter = context.GetResource<Test::NativeTestCounter>();
			if (counter == nullptr)
				return;
			const bool down = context.IsActionDown("Missing") || context.WasActionPressed("Missing") || context.WasActionReleased("Missing");
			counter->Ticks += down || context.GetActionAxis("Missing") != 0.0f ? 100u : 1u;
			context.Write(NativeLogLevel::Warning, "probed");
		}

		void RegisterProbingModule(INativeRegistrar& registrar)
		{
			RegisterTestTypes(registrar);
			registrar.AddSystem({ .Name = "Probe", .Description = "Reads an undefined action.", .Run = &Probe });
		}

		// How many lines more than the step's budget (MaxScriptLogEntriesPerFrame, Architecture §11.11) Flood writes.
		constexpr uint32_t FloodExcessLines = 5;

		// Writes the step's line budget and FloodExcessLines lines more, every step.
		void Flood(INativeStepContext& context)
		{
			for (uint32_t line = 0; line < MaxScriptLogEntriesPerFrame + FloodExcessLines; ++line)
				context.Write(NativeLogLevel::Info, "flooded");
		}

		void RegisterFloodingModule(INativeRegistrar& registrar)
		{
			RegisterTestTypes(registrar);
			registrar.AddSystem({ .Name = "Flood", .Description = "Writes more lines than a step may log.", .Run = &Flood });
		}

		// Replaces the JSON of the entity's preserved component `name`, as a scene file Scene.Load loaded would hold it.
		void SetStoredJson(Entity entity, std::string_view name, std::string_view text)
		{
			for (UnknownComponentData& data : entity.GetComponent<UnknownComponentsComponent>().Components)
			{
				if (data.Name == name)
					data.Data = VariantValue(ParseJson(text));
			}
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

		// Evaluates nothing: the replays below have no expectations.
		class NoExpectations final : public IReplayEvaluator
		{
		public:
			[[nodiscard]] Result<Json> Evaluate(PlaySession& /*session*/, const ReplayBytecodeExpectation& /*expectation*/) override
			{
				return MakeError(ErrorCode::InvalidState, "the replay has no expectations");
			}
		};

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
		TEST_CASE("NativeWorld: systems run in the NativeStep phase, after Tasks and before PreStepTransformUpdate")
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
		TEST_CASE("NativeWorld: a system reads the latched step input and its writes reach the scene before the transform update")
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
		TEST_CASE("NativeWorld: the native block follows the physics block, hashes the resources and is empty without them")
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
		TEST_CASE("NativeWorld: an exception that escapes a system stops the session fatally, naming the system")
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

		// Decision 28 (N-B): every value a system changed is checked against its type when it returns; a rejected one is put
		// back as the system found it and stops the session, naming the system. The review of M20's merge with M19: the fault
		// is published as a runtime error of script "native:<System>" and callback "NativeStep" with its entity and slot 0,
		// which names no behaviour (Architecture §11.7).
		TEST_CASE("NativeWorld: a system that leaves a value its type rejects stops the session, and the value is put back")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterBreakingModule });
			UUID mover{};
			UUID spinner{};
			PopulateNativeScene(fixture.GetScene(), *types, mover, spinner);
			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, types), fixture.GetScene());
			session->Tick();
			REQUIRE(session->GetFatalError().has_value());
			CHECK(session->GetFatalError()->Message.contains("Break"));
			CHECK(session->GetFatalError()->Entity == mover);
			CHECK(session->GetFatalError()->Kind == ScriptErrorKind::Runtime);
			CHECK(session->GetFatalError()->Script == "native:Break");
			CHECK(session->GetFatalError()->Callback == "NativeStep");
			const Json reported = ScriptErrorToJson(*session->GetFatalError());
			CHECK(reported["entity"]["id"] == Json(mover.ToString()));
			CHECK(reported["slot"] == Json(0));
			const NativeEntityComponents written = ReadNativeComponents(session->GetScene().FindEntityByID(mover), types.get());
			REQUIRE(written.Registered.size() == 1);
			CHECK(written.Registered[0].second.Get() == ParseJson(R"({"Speed": 6, "Follow": null})"));
			CHECK(session->ComputeStateHash() != 0);
		}

		// Decision 28 (N-B): only the starting scene is canonicalized, so the step reads what it finds; a value its type rejects
		// stops the session, naming the entity and the component.
		TEST_CASE("NativeWorld: a stored value its type rejects stops the session, naming the entity and the component")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			UUID mover{};
			UUID spinner{};
			PopulateNativeScene(fixture.GetScene(), *types, mover, spinner);
			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, types), fixture.GetScene());
			SetStoredJson(session->GetScene().FindEntityByID(mover), "Native.TestMover", R"({"Speed": -2})");
			session->Tick();
			REQUIRE(session->GetFatalError().has_value());
			CHECK(session->GetFatalError()->Message.contains("Native.TestMover"));
			CHECK(session->GetFatalError()->Message.contains("/Mover"));
			CHECK(session->GetFatalError()->Message.contains(NativeComponentInvalidCode));
			CHECK(session->GetFatalError()->Entity == mover);
			const uint64_t tick = session->GetTick();
			session->Tick();
			CHECK(session->GetTick() == tick);
		}

		// Decision 27 (N-B): a Play session's start makes its scene's native components canonical, and one its type rejects
		// keeps the session from starting, as a strict load's error does.
		TEST_CASE("NativeWorld: a scene whose native component its type rejects does not start a session")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			UUID mover{};
			UUID spinner{};
			PopulateNativeScene(fixture.GetScene(), *types, mover, spinner);
			SetStoredJson(fixture.GetScene().FindEntityByID(mover), "Native.TestMover", R"({"Speed": -2})");
			const Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(MakeNativeSpecification(fixture, types), fixture.GetScene());
			REQUIRE_FALSE(session.has_value());
			CHECK(session.error().GetCode() == ErrorCode::Validation);
			REQUIRE_FALSE(session.error().GetIssues().empty());
			CHECK(session.error().GetIssues()[0].JsonPointer == "/Components/Native.TestMover/Speed");

			// Simulate ignores the module and keeps the component as written.
			PlaySessionSpecification simulate = MakeNativeSpecification(fixture, types);
			simulate.Mode = PlayMode::Simulate;
			const Scope<PlaySession> simulated = RequireSession(simulate, fixture.GetScene());
			const NativeEntityComponents kept = ReadNativeComponents(simulated->GetScene().FindEntityByID(mover), types.get());
			REQUIRE(kept.Registered.size() == 1);
			CHECK(kept.Registered[0].second.Get() == ParseJson(R"({"Speed": -2})"));
		}

		// Decision 28 (N-B): a value read with notes is used migrated and written back canonical when the phase ends, and each
		// note is logged once per session, component and field.
		TEST_CASE("NativeWorld: a stored value read with notes is written back canonical, and each note is logged once")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			UUID mover{};
			UUID spinner{};
			PopulateNativeScene(fixture.GetScene(), *types, mover, spinner);
			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, types), fixture.GetScene());
			const Test::ExpectLog dropped(LogLevel::Warn, NativeFieldDroppedCode);
			for (int tick = 0; tick < 2; ++tick)
			{
				SetStoredJson(session->GetScene().FindEntityByID(spinner), "Native.TestSpinner", R"({"DegreesPerSecond": 45, "Wobble": true})");
				session->Tick();
				REQUIRE_FALSE(session->GetFatalError().has_value());
				const NativeEntityComponents written = ReadNativeComponents(session->GetScene().FindEntityByID(spinner), types.get());
				REQUIRE(written.Registered.size() == 1);
				CHECK(written.Registered[0].second.Get() == ParseJson(R"({"DegreesPerSecond": 45, "Axis": [0, 1, 0], "Turns": 1})"));
			}
			CHECK(dropped.GetMatchCount() == 1);
		}

		// Decision 28 (N-B): the step context's entity and component operations, written back when the phase ends: an added
		// component on a new entity, a removed one, a destroyed entity, and an inactive entity left out of Query.
		TEST_CASE("NativeWorld: systems add and remove components, create and destroy entities, and query active entities only")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterBuildingModule });
			UUID mover{};
			UUID spinner{};
			PopulateNativeScene(fixture.GetScene(), *types, mover, spinner);
			Entity sleeper = fixture.GetScene().CreateEntity("Sleeper");
			REQUIRE(PatchNativeComponents(sleeper, *types, ParseJson(R"({"Native.TestMover": {"Speed": 1}})"), "/components").has_value());
			sleeper.SetActive(false);
			const UUID doomed = fixture.GetScene().CreateEntity("Doomed").GetUUID();

			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, types), fixture.GetScene());
			session->Tick();
			REQUIRE_FALSE(session->GetFatalError().has_value());
			Scene& scene = session->GetScene();
			const Entity spawned = scene.FindEntityByPath("/Spawned");
			REQUIRE(spawned.IsValid());
			const NativeEntityComponents added = ReadNativeComponents(spawned, types.get());
			REQUIRE(added.Registered.size() == 1);
			CHECK(added.Registered[0].first == "Native.TestMover");
			CHECK(added.Registered[0].second.Get() == ParseJson(R"({"Speed": 4, "Follow": null})"));
			CHECK(ReadNativeComponents(scene.FindEntityByID(spinner), types.get()).Registered.empty());
			CHECK_FALSE(scene.FindEntityByID(doomed).IsValid());
			// The mover and the spawned entity, not the inactive sleeper.
			CHECK(session->GetNative()->GetResources() == ParseJson(R"({"TestCounter": {"Ticks": 2, "Distance": 0}})"));
			CHECK(ReadNativeComponents(scene.FindEntityByID(sleeper.GetUUID()), types.get()).Registered[0].second.Get() == ParseJson(R"({"Speed": 1, "Follow": null})"));
		}

		// Decision 28 (N-B): an action the project does not define reads as up and 0 and logs one error per session; a
		// system's lines are attributed to it.
		TEST_CASE("NativeWorld: an action the project does not define reads as up and 0 and logs one error per session")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterProbingModule });
			const Test::ExpectLog missing(LogLevel::Error, "'Missing'");
			const Test::ExpectLog probed(LogLevel::Warn, "native:Probe: probed");
			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, types), fixture.GetScene());
			session->Tick();
			session->Tick();
			CHECK(missing.GetMatchCount() == 1);
			CHECK(probed.GetMatchCount() == 2);
			CHECK(session->GetNative()->GetResources() == ParseJson(R"({"TestCounter": {"Ticks": 2, "Distance": 0}})"));
		}

		// Architecture §11.11 (decision 28, N-B; the review of W19.1): the native systems' log takes at most 1,024 lines
		// (MaxScriptLogEntriesPerFrame) per step; the lines beyond it are dropped and counted in one warning per step, and the
		// next step logs from zero again.
		TEST_CASE("NativeWorld: the systems' log takes at most its budget of lines per step and names how many it dropped")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterFloodingModule });
			const Test::ExpectLog lines(LogLevel::Info, "native:Flood: flooded");
			const Test::ExpectLog dropped(LogLevel::Warn, std::format("{} log lines of the native module's systems were dropped", FloodExcessLines));
			const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, types), fixture.GetScene());
			session->Tick();
			CHECK(lines.GetMatchCount() == MaxScriptLogEntriesPerFrame);
			CHECK(dropped.GetMatchCount() == 1);
			session->Tick();
			CHECK(lines.GetMatchCount() == 2 * MaxScriptLogEntriesPerFrame);
			CHECK(dropped.GetMatchCount() == 2);
		}

		// Decision 28 (N-B): the native world lives as long as the session, so resources keep their values across Scene.Load;
		// the loaded scene is not canonicalized, and the step reads its components as its file holds them.
		TEST_CASE("NativeWorld: resources keep their values across Scene.Load, and the step reads the loaded scene as its file holds it")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			Test::SceneTestFixture next;
			Entity loaded = next.GetScene().CreateEntity("Loaded");
			REQUIRE(PatchNativeComponents(loaded, *types, ParseJson(R"({"Native.TestSpinner": {}})"), "/components").has_value());
			SetStoredJson(loaded, "Native.TestSpinner", R"({"DegreesPerSecond": 10, "Wobble": true})");
			Result<Json> document = SceneSerializer::ToJson(next.GetScene());
			REQUIRE(document.has_value());
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			Test::InMemoryAssetManager assets;
			const AssetHandle scene(0x4E570001);
			assets.Publish(scene, data, "Assets/Scenes/Next.scene");

			PlaySessionSpecification specification = MakeNativeSpecification(fixture, types);
			specification.Assets = &assets;
			const Scope<PlaySession> session = RequireSession(specification, fixture.GetScene());
			session->Tick();
			REQUIRE(session->GetScripts()->GetHost().RequestSceneLoad(scene, Json::object()).has_value());
			session->Tick();
			const Entity replaced = session->GetScene().FindEntityByPath("/Loaded");
			REQUIRE(replaced.IsValid());
			CHECK(ReadNativeComponents(replaced, types.get()).Registered[0].second.Get() == ParseJson(R"({"DegreesPerSecond": 10, "Wobble": true})"));

			const Test::ExpectLog dropped(LogLevel::Warn, NativeFieldDroppedCode);
			session->Tick();
			REQUIRE_FALSE(session->GetFatalError().has_value());
			CHECK(session->GetNative()->GetResources() == ParseJson(R"({"TestCounter": {"Ticks": 3, "Distance": 0}})"));
			const NativeEntityComponents written = ReadNativeComponents(session->GetScene().FindEntityByPath("/Loaded"), types.get());
			REQUIRE(written.Registered.size() == 1);
			CHECK(written.Registered[0].second.Get() == ParseJson(R"({"DegreesPerSecond": 10, "Axis": [0, 1, 0], "Turns": 1})"));
		}

		// The milestone review (Docs/Decisions/0030-m19-contract.md decisions 2 and 28): a game's native module is simulation
		// code that the start fingerprint does not record, so a replay recorded with one module and played with a changed one
		// fails at its first checkpoint naming no change, and its divergence causes name the native module.
		TEST_CASE("NativeWorld: a replay played with a changed native module names the module among its divergence causes")
		{
			Test::SceneTestFixture fixture;
			const Ref<const NativeModuleTypes> recordedModule = RequireTestTypes(Test::GetNativeTestModule());
			UUID mover{};
			UUID spinner{};
			PopulateNativeScene(fixture.GetScene(), *recordedModule, mover, spinner);
			ReplayHeader header;
			header.Scene = { UUID(0x4E570002), "Assets/Scenes/Native.scene" };
			header.Seed = MakeNativeSpecification(fixture, recordedModule).Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";

			const Scope<PlaySession> recorded = RequireSession(MakeNativeSpecification(fixture, recordedModule), fixture.GetScene());
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(header, *recorded));
			for (uint64_t tick = 0; tick < 70; ++tick)
			{
				recorder.CaptureCheckpoint(*recorded);
				recorded->Tick();
				REQUIRE(recorder.CaptureAppliedInput(tick, recorded->GetInput().GetLastAppliedEvents()));
			}
			const Result<ReplayDocument> document = recorder.Finish(*recorded);
			REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
			REQUIRE(document->Fingerprint.has_value());
			REQUIRE(document->Checkpoints.size() == 1);
			auto replay = CreateRef<ReplayData>();
			replay->Header = header;
			replay->FinalTick = 70;
			replay->FinalStateHash = document->FinalStateHash;
			replay->Checkpoints = document->Checkpoints;
			replay->Fingerprint = document->Fingerprint;

			const auto play = [&fixture, &replay](Ref<const NativeModuleTypes> module)
			{
				const Scope<PlaySession> session = RequireSession(MakeNativeSpecification(fixture, std::move(module)), fixture.GetScene());
				ReplayPlayer player;
				REQUIRE(player.Begin(replay, *session, { .Verify = true, .StrictHash = true }));
				NoExpectations evaluator;
				bool complete = false;
				for (uint32_t step = 0; step < 1000 && !complete; ++step)
				{
					const Result<bool> advanced = player.Advance(*session, evaluator);
					REQUIRE_MESSAGE(advanced.has_value(), advanced.error().ToString());
					complete = *advanced;
				}
				REQUIRE(complete);
				Result<ReplayPlaybackResult> result = player.GetResult();
				REQUIRE(result.has_value());
				return std::move(*result);
			};
			CHECK(play(recordedModule).Passed);

			// The same types without their systems: the module's code changed, its scene data did not.
			const ReplayPlaybackResult changed = play(RequireTestTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterTestTypes }));
			CHECK_FALSE(changed.Passed);
			CHECK(changed.CheckpointFailed);
			CHECK(changed.Changes.empty());
			CHECK(changed.StopReason.find(GetReplayDivergenceCauses()) != std::string::npos);
			CHECK(GetReplayDivergenceCauses().find("the game's native module") != std::string_view::npos);
			CHECK(GetReplayDivergenceHint().find(GetReplayDivergenceCauses()) != std::string_view::npos);
		}
	}

}
