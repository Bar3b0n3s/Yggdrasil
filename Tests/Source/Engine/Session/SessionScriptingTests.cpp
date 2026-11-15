#include "TestsPCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Test {

		static void AttachSessionScript(Entity entity, AssetHandle script)
		{
			ScriptSlot component{};
			component.Script.SetHandle(script);
			entity.AddComponent<ScriptComponent>(ScriptComponent{ { component } });
		}

		static PlaySessionSpecification ScriptSessionSpecification(ScriptTestFixture& fixture)
		{
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 47;
			specification.Serial = 19;
			return specification;
		}

		static std::vector<std::string> SessionTrace(Scene& scene)
		{
			std::vector<std::string> names;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const Entity entity = scene.FindEntityByID(id);
				if (entity && entity.GetName().starts_with("trace:"))
					names.push_back(entity.GetName().substr(6));
			}
			return names;
		}

		// Records the script phases and fatal errors a session reports to its host.
		class PhaseRecordingHost final : public IPlaySessionHost
		{
		public:
			ScriptEnvironment GetScriptEnvironment() const override { return {}; }
			Status SetScriptCursorMode(CursorMode) override { return {}; }
			CursorMode GetScriptCursorMode() const override { return CursorMode::Normal; }
			void OnScriptError(const ScriptError& error, bool fatal) override
			{
				if (fatal)
					FatalErrors.push_back(error);
			}
			void OnScriptPhase(std::string_view script, std::string_view callback) override
			{
				std::string phase;
				if (!script.empty() || !callback.empty())
					FormatScriptPhase(phase, script, callback);
				Phases.push_back(std::move(phase));
			}
			std::vector<std::string> Phases{};
			std::vector<ScriptError> FatalErrors{};
		};

		// Runs `ticks` ticks of `session` in frames of `stepsPerFrame` fixed steps each (the last frame holds the rest), each
		// frame ending with its frame phase, as a ScriptedClock groups ticks into frames; one step a frame is lockstep's.
		static void RunGroupedTicks(PlaySession& session, uint32_t ticks, uint32_t stepsPerFrame)
		{
			for (uint32_t done = 0; done < ticks;)
			{
				const uint32_t steps = std::min(stepsPerFrame, ticks - done);
				for (uint32_t step = 0; step < steps; ++step)
					session.FixedStep();
				done += steps;
				const double delta = static_cast<double>(steps) * session.GetFixedDelta();
				session.FrameUpdate(FrameTime{ .DeltaTime = delta, .UnscaledDeltaTime = delta, .Alpha = 1.0, .FrameIndex = 0 });
			}
		}

		// Nest.luau: an entity named "Level<n>" records its OnStart's tick and its first OnFixedUpdate's tick, moves one unit
		// along X in every OnFixedUpdate (so the state hash counts them), and its OnStart creates "Level<n + 1>" with the same
		// behaviour up to "Level<depth>".
		static std::string NestSource(int depth)
		{
			return std::format(R"(local T = {{}}
local Depth = {}
local function trace(self, what: string)
	Scene.CreateEntity("trace:" .. self.Entity.Name .. ":" .. what)
end
function T:OnStart()
	self.Fixed = 0
	trace(self, "start:" .. Time.GetTick())
	local level = tonumber(string.sub(self.Entity.Name, 6)) :: number
	if level < Depth then
		local child = Scene.CreateEntity("Level" .. (level + 1))
		child:AddComponent("Script", {{Scripts = {{{{Script = Assets.Load("Assets/Scripts/Nest.luau"), Fields = {{}}, ExecutionOrder = 0}}}}}})
	end
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed == 1 then
		trace(self, "fixed:" .. Time.GetTick())
	end
	self.Entity.Transform:Translate(vector.create(1, 0, 0))
end
return Script.Define("Nest", T)
)",
				depth);
		}

		// The Luau that creates Level1 with Nest.luau.
		inline constexpr std::string_view SpawnLevel1 = R"(local level = Scene.CreateEntity("Level1")
	level:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/Nest.luau"), Fields = {}, ExecutionOrder = 0}}}))";

		// The Luau of a local Spawn(name, x): a Kinematic probe "<name>" at (x, 0, 0) with Probe.luau, its body in the 3D world
		// or, with `is2D`, the 2D one.
		static std::string ProbeSpawnSource(bool is2D)
		{
			return std::format(R"(local function Spawn(name: string, x: number)
	local entity = Scene.CreateEntity(name)
	entity.Transform.Translation = vector.create(x, 0, 0)
	entity:AddComponent("{}", {{HalfExtents = vector.create(1, 0.25, {})}})
	entity:AddComponent("{}", {{Type = "Kinematic"}})
	entity:AddComponent("Script", {{Scripts = {{{{Script = Assets.Load("Assets/Scripts/Probe.luau"), Fields = {{}}, ExecutionOrder = 0}}}}}})
end
)",
				is2D ? "BoxCollider2D" : "BoxCollider", is2D ? 0 : 1, is2D ? "RigidBody2D" : "RigidBody");
		}

		// Probe.luau: records "trace:<name>:start:<body|none>" in OnStart (a velocity read, which raises without a body) and
		// "trace:<name>:fixed:<body|none>" in its first two OnFixedUpdates (a MoveKinematic to where it is), through the 3D
		// RigidBody or, with `is2D`, the RigidBody2D.
		static std::string ProbeSource(bool is2D)
		{
			const std::string_view read = is2D ? "self.Entity.RigidBody2D:GetLinearVelocity()" : "self.Entity.RigidBody:GetLinearVelocity()";
			const std::string_view move = is2D ? "self.Entity.RigidBody2D:MoveKinematic(vector.create(transform.WorldPosition.x, transform.WorldPosition.y, 0), 0)"
											   : "self.Entity.RigidBody:MoveKinematic(transform.WorldPosition, transform.WorldRotation)";
			return std::format(R"(local T = {{}}
local function record(self, phase: string, ok: boolean)
	Scene.CreateEntity("trace:" .. self.Entity.Name .. ":" .. phase .. ":" .. (if ok then "body" else "none"))
end
function T:OnStart()
	self.Fixed = 0
	record(self, "start", (pcall(function() return {} end)))
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed <= 2 then
		local transform = self.Entity.Transform
		record(self, "fixed", (pcall(function() {} end)))
	end
end
return Script.Define("Probe", T)
)",
				read, move);
		}

		// Reader.luau: its OnStart records "trace:<name>:axis:<Move>:dt:<delta in fixed steps>" and writes both into its
		// Transform (so the state hash sees them); "Early"'s OnStart also destroys the "Floor" box the "Ball" rests on.
		inline constexpr std::string_view ReaderSource = R"(local T = {}
function T:OnStart()
	local axis = Input.GetAxis("Move")
	local steps = math.floor(Time.GetDeltaTime() / Time.GetFixedDeltaTime() + 0.5)
	Scene.CreateEntity("trace:" .. self.Entity.Name .. ":axis:" .. tostring(axis) .. ":dt:" .. tostring(steps))
	self.Entity.Transform.Translation = vector.create(axis, steps, 0)
	if self.Entity.Name == "Early" then
		(Scene.FindByName("Floor") :: Entity):Destroy()
	end
end
return Script.Define("Reader", T)
)";

		struct GroupedRun
		{
			std::vector<std::string> Trace{};
			uint64_t StateHash = 0;
		};

		// A session of `fixture`'s scene run for `ticks` ticks in frames of `stepsPerFrame` steps: its trace and state hash.
		static GroupedRun RunGrouped(ScriptTestFixture& fixture, uint32_t ticks, uint32_t stepsPerFrame)
		{
			auto created = PlaySession::CreateFromScene(ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			RunGroupedTicks(*session, ticks, stepsPerFrame);
			CHECK(session->GetScriptErrors().GetErrors().empty());
			return { SessionTrace(session->GetScene()), session->ComputeStateHash() };
		}

		// A session of `fixture`'s scene with the axis action "Move", which an injected value sets to 1 from tick 1 on, run
		// for `ticks` ticks in frames of `stepsPerFrame` steps: its trace and state hash.
		static GroupedRun RunGroupedWithMove(ScriptTestFixture& fixture, uint32_t ticks, uint32_t stepsPerFrame)
		{
			PlaySessionSpecification specification = ScriptSessionSpecification(fixture);
			specification.Project.Input.Actions["Move"].Type = InputActionSettings::ActionType::Axis;
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			REQUIRE(session->GetInput().Queue(1, { .Type = PlayInputEventType::Action, .Name = "Move", .HasValue = true, .Value = 1.0f }));
			RunGroupedTicks(*session, ticks, stepsPerFrame);
			CHECK(session->GetScriptErrors().GetErrors().empty());
			return GroupedRun{ SessionTrace(session->GetScene()), session->ComputeStateHash() };
		}

		class ScriptPhaseHook final : public IPlaySessionTestHook
		{
		public:
			void AfterTasks(PlaySession& session, const SimStep& step) override
			{
				static_cast<void>(step);
				Observations.push_back(SessionTrace(session.GetScene()));
			}
			void OnQuit(int32_t exitCode) override { Quit = exitCode; }
			std::vector<std::vector<std::string>> Observations{};
			std::optional<int32_t> Quit{};
		};

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: the host receives each running gameplay entry as its script phase")
		{
			// §13.2: a Busy answer names the gameplay entry the main thread is stuck in; the session forwards every
			// protected entry and exit of its VM to the host, and one-shot evaluations report none.
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x710031);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Board.luau", R"(
local Board = {}
function Board:OnFixedUpdate() Task.Spawn(function() Task.WaitTicks(1) end) end
return Script.Define("Board", Board)
)"));
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Board"), script);
			Test::PhaseRecordingHost host;
			auto specification = Test::ScriptSessionSpecification(fixture);
			specification.Host = &host;
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(created);
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			session->Tick();
			host.Phases.clear();
			session->Tick();
			const auto& phases = host.Phases;
			CHECK(std::ranges::find(phases, "Script:Board.OnFixedUpdate") != phases.end());
			CHECK(std::ranges::find(phases, "Script:Board.Task") != phases.end());
			REQUIRE_FALSE(phases.empty());
			// Every entry is closed again: the phase is cleared once no gameplay script runs.
			CHECK(phases.back().empty());
			host.Phases.clear();
			REQUIRE(session->GetScripts()->Evaluate("return 1 + 2", {}));
			CHECK(std::ranges::none_of(host.Phases, [](const std::string& phase)
			{
				return !phase.empty();
			}));
		}

		TEST_CASE("PlaySession: a second memory breach stops the session with a structured error")
		{
			// §11.1 through a hosted session: the first soft breach disables the faulting instance and the VM recovers; the
			// second stops the session with a structured memory error the host and play.state see (GetFatalError).
			Test::ScriptTestFixture fixture;
			constexpr std::string_view Leak = R"(
local Leak = {}
function Leak:OnFixedUpdate()
	local retained = string.rep("x", 2 * 1024 * 1024)
	while true do retained = retained end
end
return Script.Define("Leak", Leak)
)";
			// Two files, so the two breaches stay two entries of the error stream (it merges repeats of one location).
			const AssetHandle first(0x710032), second(0x710033);
			REQUIRE(fixture.AddScript(first, "Assets/Scripts/LeakFirst.luau", std::string(Leak)));
			REQUIRE(fixture.AddScript(second, "Assets/Scripts/LeakSecond.luau", std::string(Leak)));
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("First"), first);
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Second"), second);
			Test::PhaseRecordingHost host;
			auto specification = Test::ScriptSessionSpecification(fixture);
			specification.Host = &host;
			specification.Project.Scripting.MemoryLimitMB = 1;
			specification.Project.Scripting.PauseOnError = false;
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(created);
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			CHECK_FALSE(session->GetFatalError().has_value());
			{
				Test::ExpectLog breach(LogLevel::Error, "memory limit");
				session->Tick();
			}
			const auto& errors = session->GetScriptErrors().GetErrors();
			REQUIRE(errors.size() == 2);
			CHECK(errors[0].Kind == ScriptErrorKind::Memory);
			CHECK(errors[0].EntityName == "First");
			REQUIRE(session->GetFatalError().has_value());
			const ScriptError& fatal = *session->GetFatalError();
			CHECK(fatal.Kind == ScriptErrorKind::Memory);
			CHECK(fatal.EntityName == "Second");
			CHECK(fatal.Script == "Assets/Scripts/LeakSecond.luau");
			CHECK(fatal.ID == errors[1].ID);
			REQUIRE(host.FatalErrors.size() == 1);
			CHECK(host.FatalErrors[0].ID == fatal.ID);
			CHECK(session->IsPaused());
			CHECK(session->GetScripts()->IsStopped());
			const uint64_t tick = session->GetTick();
			session->Tick();
			CHECK(session->GetTick() == tick);
		}

		TEST_CASE("PlaySession: scene load transfers maximum-depth parameters into the replacement VM")
		{
			Test::ScriptTestFixture fixture;
			auto document = SceneSerializer::ToJson(fixture.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			const AssetHandle scene(0x710024);
			fixture.GetAssetManager().Publish(scene, data, "Assets/Scenes/Deep.scene");
			auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE(created);
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			const auto requested = session->GetScripts()->Evaluate(std::format(R"(
local value = 9
for i = 1,{} do value = {{value}} end
local parameters = {{Payload = value}}
Scene.Load(Assets.Load("Assets/Scenes/Deep.scene"), parameters)
parameters.Payload = {{999}}
return true
)",
																	   MaxJsonDepth - 1),
				{});
			REQUIRE_MESSAGE(requested.has_value(), (requested ? "" : requested.error().ToString()));
			CHECK(session->GetSceneGeneration() == 1);
			CHECK(session->GetLoadParameters().empty());
			session->Tick();
			CHECK(session->GetSceneGeneration() == 2);
			const auto observed = session->GetScripts()->Evaluate(std::format(R"(
local value = Scene.GetLoadParameters().Payload
for i = 1,{} do value = value[1] end
assert(value == 9)
return true
)",
																	  MaxJsonDepth - 1),
				{});
			REQUIRE_MESSAGE(observed.has_value(), (observed ? "" : observed.error().ToString()));
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		TEST_CASE("PlaySession: startup audio gains survive callbacks and restore across scene replacement")
		{
			Test::ScriptTestFixture fixture;
			VirtualFileSystem vfs;
			const auto createdAudio = AudioEngine::Create(AudioEngineSpecification{}, vfs);
			REQUIRE(createdAudio);
			AudioEngine& audio = **createdAudio;
			REQUIRE(audio.SetGroupVolume(AudioGroup::Music, 0.75f));
			const AssetHandle script(0x710021), scene(0x710022);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Mix.luau", R"(
local T = {}
function T:OnCreate()
	assert(Audio.GetGroupVolume("Music") == 1)
	Audio.SetGroupVolume("Music", 0.25)
end
function T:OnStart()
	assert(Audio.GetGroupVolume("Music") == 0.25)
	Audio.SetGroupVolume("Music", 0.5)
end
function T:OnDestroy() Audio.SetGroupVolume("Music", 0.125) end
return Script.Define("Mix", T)
)"));
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Mixer"), script);
			const auto document = SceneSerializer::ToJson(fixture.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(*document);
			fixture.GetAssetManager().Publish(scene, data, "Assets/Scenes/Mix.scene");
			auto specification = Test::ScriptSessionSpecification(fixture);
			specification.Audio = &audio;
			specification.OwnsAudioTime = true;
			auto result = PlaySession::Create(specification, *document);
			REQUIRE(result);
			auto session = std::move(*result);
			session->SetExtractionEnabled(false);
			CHECK(audio.GetGroupVolume(AudioGroup::Music) == 0.5f);
			CHECK(session->GetScriptErrors().GetErrors().empty());
			REQUIRE(session->GetScripts()->GetHost().RequestSceneLoad(scene, Json::object()));
			session->Tick();
			CHECK(session->GetSceneGeneration() == 2);
			CHECK(audio.GetGroupVolume(AudioGroup::Music) == 0.5f);
			CHECK(session->GetScriptErrors().GetErrors().empty());
			session.reset();
			CHECK(audio.GetGroupVolume(AudioGroup::Music) == 0.75f);
			CHECK_FALSE(audio.IsSimulationTimeOwned());
		}

		TEST_CASE("PlaySession: preparation preserves the live session and activation acquires audio after retirement")
		{
			Test::ScriptTestFixture fixture;
			VirtualFileSystem vfs;
			const auto createdAudio = AudioEngine::Create(AudioEngineSpecification{}, vfs);
			REQUIRE(createdAudio);
			AudioEngine& audio = **createdAudio;
			REQUIRE(audio.SetGroupVolume(AudioGroup::Music, 0.75f));
			const AssetHandle script(0x710023);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Activation.luau", R"(
local T = {}
function T:OnCreate()
	assert(Audio.GetGroupVolume("Music") == 1)
	self.Entity.Name = "Activated"
end
function T:OnDestroy() Audio.SetGroupVolume("Music", 0.125) end
return Script.Define("Activation", T)
)"));
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Prepared"), script);
			const auto document = SceneSerializer::ToJson(fixture.GetScene());
			REQUIRE(document);
			auto specification = Test::ScriptSessionSpecification(fixture);
			specification.Audio = &audio;
			specification.OwnsAudioTime = true;
			auto initial = PlaySession::Create(specification, *document);
			REQUIRE(initial);
			auto current = std::move(*initial);
			current->SetPaused(true);
			current->SetLockstep(true, 17);
			REQUIRE(current->GetAudioSystem()->SetGroupVolume(AudioGroup::Music, 0.25f));
			const uint64_t originalHash = current->ComputeStateHash();
			auto invalid = specification;
			invalid.Project.Scripting.MemoryLimitMB = 0;
			CHECK_FALSE(PlaySession::Prepare(invalid, *document));
			CHECK(current->ComputeStateHash() == originalHash);
			CHECK(current->IsPaused());
			CHECK(current->GetLockstepOwner() == 17);
			CHECK(audio.GetGroupVolume(AudioGroup::Music) == 0.25f);
			CHECK(audio.IsSimulationTimeOwned());
			{
				auto discarded = PlaySession::Prepare(specification, *document);
				REQUIRE(discarded);
				CHECK((*discarded)->GetAudioSystem() == nullptr);
				CHECK((*discarded)->GetScene().FindEntityByPath("/Prepared").IsValid());
			}
			CHECK(audio.GetGroupVolume(AudioGroup::Music) == 0.25f);
			CHECK(audio.IsSimulationTimeOwned());
			for (uint64_t replacement = 0; replacement < 2; ++replacement)
			{
				++specification.Serial;
				auto candidate = PlaySession::Prepare(specification, *document);
				REQUIRE(candidate);
				CHECK((*candidate)->GetScene().FindEntityByPath("/Prepared").IsValid());
				CHECK((*candidate)->GetAudioSystem() == nullptr);
				CHECK_FALSE((*candidate)->IsAudioTimeOwned());
				CHECK(audio.GetGroupVolume(AudioGroup::Music) == (replacement == 0 ? 0.25f : 1.0f));
				current.reset();
				CHECK(audio.GetGroupVolume(AudioGroup::Music) == 0.75f);
				CHECK_FALSE(audio.IsSimulationTimeOwned());
				current = std::move(*candidate);
				current->Activate();
				CHECK(current->GetScene().FindEntityByPath("/Activated").IsValid());
				CHECK(current->GetScriptErrors().GetErrors().empty());
				CHECK(audio.GetGroupVolume(AudioGroup::Music) == 1.0f);
				CHECK(audio.IsSimulationTimeOwned());
			}
			current.reset();
			CHECK(audio.GetGroupVolume(AudioGroup::Music) == 0.75f);
			CHECK_FALSE(audio.IsSimulationTimeOwned());
		}

		// Docs/Decisions/0031-m20-contract.md decision 5 (W-P06, ADR 0026 P06): Scene.Load of the played scene loaded its saved
		// file, dropping the play copy's unsaved edits. Scene.Reload restarts the current scene from the document it started
		// from (here the play copy of a scene that has no asset at all), with new parameters or, without them, the current
		// ones; one transition per frame.
		TEST_CASE("PlaySession: Scene.Reload restarts the current scene from its start document with its parameters")
		{
			Test::ScriptTestFixture fixture;
			static_cast<void>(fixture.GetScene().CreateEntity("Unsaved"));
			auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE(created);
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			// What play changed is not in the start document; a second transition in the same frame is refused.
			const auto requested = session->GetScripts()->Evaluate(R"(
Scene.CreateEntity("Spawned")
Scene.FindByName("Unsaved").Name = "Renamed"
Scene.Reload({ Round = 2 })
assert(not pcall(function() Scene.Reload() end))
return true
)",
				{});
			REQUIRE_MESSAGE(requested.has_value(), (requested ? "" : requested.error().ToString()));
			CHECK(session->GetSceneGeneration() == 1);
			session->Tick();
			CHECK(session->GetSceneGeneration() == 2);
			CHECK(session->GetLoadParameters() == Json{ { "Round", 2 } });
			CHECK(session->GetScene().FindEntityByPath("/Unsaved").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/Renamed").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/Spawned").IsValid());

			// Without parameters the current scene's are kept.
			REQUIRE(session->GetScripts()->GetHost().RequestSceneReload(std::nullopt));
			session->Tick();
			CHECK(session->GetSceneGeneration() == 3);
			CHECK(session->GetLoadParameters() == Json{ { "Round", 2 } });
			CHECK(session->GetScene().FindEntityByPath("/Unsaved").IsValid());
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		// Docs/Decisions/0031-m20-contract.md decision 5: Scene.GetCurrent names the scene asset the current scene came from:
		// the session's, then the one Scene.Load named, kept by Scene.Reload; nil for a scene without an asset.
		TEST_CASE("PlaySession: Scene.GetCurrent names the scene asset the session plays")
		{
			Test::ScriptTestFixture fixture;
			auto document = SceneSerializer::ToJson(fixture.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(*document);
			const AssetHandle start(0x710051);
			const AssetHandle next(0x710052);
			fixture.GetAssetManager().Publish(start, data, "Assets/Scenes/Start.scene");
			fixture.GetAssetManager().Publish(next, data, "Assets/Scenes/Next.scene");
			{
				auto specification = Test::ScriptSessionSpecification(fixture);
				specification.SceneAsset = start;
				auto created = PlaySession::Create(specification, *document);
				REQUIRE(created);
				auto session = std::move(*created);
				session->SetExtractionEnabled(false);
				CHECK(session->GetCurrentSceneAsset() == start);
				const auto loaded = session->GetScripts()->Evaluate(R"(
assert(Scene.GetCurrent() == Assets.Load("Assets/Scenes/Start.scene"))
Scene.Load(Assets.Load("Assets/Scenes/Next.scene"))
return true
)",
					{});
				REQUIRE_MESSAGE(loaded.has_value(), (loaded ? "" : loaded.error().ToString()));
				session->Tick();
				CHECK(session->GetCurrentSceneAsset() == next);
				REQUIRE(session->GetScripts()->GetHost().RequestSceneReload(std::nullopt));
				session->Tick();
				CHECK(session->GetSceneGeneration() == 3);
				CHECK(session->GetCurrentSceneAsset() == next);
				const auto kept = session->GetScripts()->Evaluate(R"(
assert(Scene.GetCurrent() == Assets.Load("Assets/Scenes/Next.scene"))
return true
)",
					{});
				REQUIRE_MESSAGE(kept.has_value(), (kept ? "" : kept.error().ToString()));
			}

			// A session of a scene without an asset (an editor scene never saved).
			auto unsaved = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE(unsaved);
			CHECK_FALSE((*unsaved)->GetCurrentSceneAsset().IsValid());
			const auto none = (*unsaved)->GetScripts()->Evaluate("assert(Scene.GetCurrent() == nil)\nreturn true", {});
			REQUIRE_MESSAGE(none.has_value(), (none ? "" : none.error().ToString()));
		}

		// Docs/Decisions/0031-m20-contract.md decision 5's second branch: once a Scene.Load replaced the start scene,
		// Scene.Reload restarts the loaded asset's document as it was when Scene.Load loaded it, never the session's start
		// document and never the asset's version published since (a reimport while playing).
		TEST_CASE("PlaySession: Scene.Reload after Scene.Load restarts the loaded scene's document as it was loaded")
		{
			Test::ScriptTestFixture fixture;
			Scene& authored = fixture.GetScene();
			// Three documents of one scene file's story: the start scene, the loaded one, and the loaded one changed later.
			const auto documentWith = [&authored](std::string_view name)
			{
				const Entity entity = authored.CreateEntity(std::string(name));
				Result<Json> document = SceneSerializer::ToJson(authored);
				REQUIRE(document);
				authored.DestroyEntity(entity);
				return CreateRef<const Json>(std::move(*document));
			};
			const Ref<const Json> startDocument = documentWith("InStart");
			const Ref<const Json> nextDocument = documentWith("InNext");
			const Ref<const Json> republishedDocument = documentWith("Republished");
			const auto publish = [&fixture](AssetHandle handle, const Ref<const Json>& document, std::string path)
			{
				auto data = CreateRef<SceneData>();
				data->Document = document;
				fixture.GetAssetManager().Publish(handle, data, std::move(path));
			};
			const AssetHandle start(0x710061);
			const AssetHandle next(0x710062);
			publish(start, startDocument, "Assets/Scenes/Start.scene");
			publish(next, nextDocument, "Assets/Scenes/Next.scene");

			auto specification = Test::ScriptSessionSpecification(fixture);
			specification.SceneAsset = start;
			auto created = PlaySession::Create(specification, *startDocument);
			REQUIRE(created);
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			REQUIRE(session->GetScene().FindEntityByPath("/InStart").IsValid());
			REQUIRE(session->GetScripts()->GetHost().RequestSceneLoad(next, Json{ { "Level", 2 } }));
			session->Tick();
			REQUIRE(session->GetScene().FindEntityByPath("/InNext").IsValid());

			// The asset changes while the loaded scene plays; the reload keeps the document it was loaded from.
			publish(next, republishedDocument, "Assets/Scenes/Next.scene");
			const auto reloaded = session->GetScripts()->Evaluate(R"(
Scene.FindByName("InNext").Name = "Renamed"
Scene.Reload()
return true
)",
				{});
			REQUIRE_MESSAGE(reloaded.has_value(), (reloaded ? "" : reloaded.error().ToString()));
			session->Tick();
			CHECK(session->GetSceneGeneration() == 3);
			CHECK(session->GetCurrentSceneAsset() == next);
			CHECK(session->GetLoadParameters() == Json{ { "Level", 2 } });
			CHECK(session->GetScene().FindEntityByPath("/InNext").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/Renamed").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/InStart").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/Republished").IsValid());
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		// Docs/Decisions/0031-m20-contract.md decision 4 (W-P02, ADR 0026 P02): a partner's callback that read an entity
		// destroyed earlier in the same dispatch raised "was destroyed". An entity pending destruction stays readable until
		// the flush (its name, its components' fields), refuses writes, is not IsValid, and is unreadable once flushed.
		TEST_CASE("ScriptProxy: an entity pending destruction stays readable until the flush and refuses writes")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x710053);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Reader.luau", R"(
local Reader = {}
function Reader:OnFixedUpdate()
	if self.Target == nil then
		local target = Scene.FindByName("Target")
		self.Target = target
		Scene.Destroy(target)
		local name = target.Name
		local x = target.Transform.Translation.x
		local wrote = pcall(function() target.Transform.Translation = vector.create(9, 9, 9) end)
		self.Entity.Name = string.format("read:%s:%d:%s:%s", name, x, tostring(wrote), tostring(target:IsValid()))
	elseif not self.Checked then
		self.Checked = true
		local readable = pcall(function() return self.Target.Name end)
		self.Entity.Name = self.Entity.Name .. ":after:" .. tostring(readable)
	end
end
return Script.Define("Reader", Reader)
)"));
			Entity target = fixture.GetScene().CreateEntity("Target");
			target.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(3.0f, 0.0f, 0.0f);
			});
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Reader"), script);
			auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE(created);
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			session->Tick();
			CHECK(session->GetScene().FindEntityByPath("/read:Target:3:false:false").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/Target").IsValid());
			session->Tick();
			CHECK(session->GetScene().FindEntityByPath("/read:Target:3:false:false:after:false").IsValid());
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		TEST_CASE("PlaySession: quit from OnDestroy cancels a pending scene replacement")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x710011);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/QuitOnDestroy.luau", R"(
local T = {}
function T:OnDestroy() Application.Quit(7) end
return Script.Define("QuitOnDestroy", T)
)"));
			Test::SceneTestFixture target;
			static_cast<void>(target.GetScene().CreateEntity("MustNotLoad"));
			const auto document = SceneSerializer::ToJson(target.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(*document);
			const AssetHandle scene(0x710012);
			fixture.GetAssetManager().Publish(scene, data, "Assets/Scenes/Next.scene");
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Original"), script);
			auto result = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE(result);
			auto session = std::move(*result);
			session->SetExtractionEnabled(false);
			const uint64_t generation = session->GetSceneGeneration();
			REQUIRE(session->GetScripts()->GetHost().RequestSceneLoad(scene, Json::object()));
			session->Tick();
			CHECK(session->GetQuitRequest() == 7);
			CHECK(session->GetSceneGeneration() == generation);
			CHECK(session->GetScene().FindEntityByPath("/Original").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/MustNotLoad").IsValid());
		}

		TEST_CASE("PlaySession: scripts receive phase input and tasks precede the test driver")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x710001);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Phases.luau", R"(
local T = {}
local function trace(value) Scene.CreateEntity("trace:" .. value) end
function T:OnCreate() trace("create") end
function T:OnStart()
	trace("start")
	Task.Spawn(function() Task.WaitTicks(1); trace("task") end)
end
function T:OnFixedUpdate(dt)
	assert(dt == Time.GetFixedDeltaTime())
	trace("fixed:" .. tostring(Time.GetTick()))
	if Input.IsKeyPressed("Space") then trace("step-press") end
	if Input.IsKeyReleased("Space") then trace("step-release") end
end
function T:OnUpdate(dt)
	assert(dt == Time.GetDeltaTime())
	trace("update")
	if Input.IsKeyPressed("Space") then trace("frame-press") end
end
function T:OnLateUpdate() trace("late") end
return Script.Define("Phases", T)
)"));
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Scripted"), script);
			Test::ScriptPhaseHook hook;
			auto specification = Test::ScriptSessionSpecification(fixture);
			specification.TestHook = &hook;
			auto result = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(result);
			auto session = std::move(*result);
			session->SetExtractionEnabled(false);
			CHECK(Test::SessionTrace(session->GetScene()) == std::vector<std::string>{ "create", "start" });
			REQUIRE(session->GetInput().Queue(0, { .Type = PlayInputEventType::KeyInput, .State = PlayInputEventState::Tap, .KeyCode = Key::Space }));
			session->Tick();
			session->Tick();
			CHECK(Test::SessionTrace(session->GetScene()) == std::vector<std::string>{ "create", "start", "fixed:0", "step-press", "update", "frame-press", "late", "fixed:1", "step-release", "task", "update", "late" });
			REQUIRE(hook.Observations.size() == 2);
			CHECK(hook.Observations[0].back() == "step-press");
			CHECK(hook.Observations[1].back() == "task");
			CHECK(session->GetScriptErrors().GetErrors().empty());
			CHECK(fixture.GetScene().GetEntityCount() == 1);
		}

		TEST_CASE("PlaySession: gameplay scene loads replace the VM at frame end and retain the recording timeline")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle sourceScript(0x710002), targetScript(0x710003), targetScene(0x710004);
			REQUIRE(fixture.AddScript(sourceScript, "Assets/Scripts/First.luau", R"(
local T = {}
function T:OnFixedUpdate()
	Scene.Load(Assets.Load("Assets/Scenes/Second.scene"), { Score = 42 })
	self.Entity.Name = "StillAlive"
end
function T:OnLateUpdate() assert(self.Entity.Name == "StillAlive") end
return Script.Define("First", T)
)"));
			REQUIRE(fixture.AddScript(targetScript, "Assets/Scripts/Second.luau", R"(
local T = {}
function T:OnCreate()
	assert(Scene.GetLoadParameters().Score == 42)
	assert(Time.GetTick() == 1)
	self.Entity.Name = "Loaded"
end
return Script.Define("Second", T)
)"));
			Test::SceneTestFixture target;
			Test::AttachSessionScript(target.GetScene().CreateEntity("Second"), targetScript);
			auto document = SceneSerializer::ToJson(target.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			fixture.GetAssetManager().Publish(targetScene, data, "Assets/Scenes/Second.scene");
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("First"), sourceScript);
			auto specification = Test::ScriptSessionSpecification(fixture);
			specification.Parameters.Set(Json{ { "Original", true } });
			auto result = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(result);
			auto session = std::move(*result);
			session->SetExtractionEnabled(false);
			const uint64_t vm = session->GetScripts()->GetGeneration();
			const uint64_t sceneGeneration = session->GetSceneGeneration();
			ReplayHeader header{};
			header.Scene = { AssetHandle(0x710005), "Assets/Scenes/First.scene" };
			header.Parameters = specification.Parameters;
			header.Seed = specification.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";
			REQUIRE(session->GetRecorder()->Begin(header, *session));
			REQUIRE(session->GetInput().Queue(0, { .Type = PlayInputEventType::KeyInput, .State = PlayInputEventState::Tap, .KeyCode = Key::Space }));
			session->Tick();
			CHECK(session->GetScene().FindEntityByPath("/Loaded").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/StillAlive").IsValid());
			CHECK(session->GetScripts()->GetGeneration() != vm);
			CHECK(session->GetSceneGeneration() == sceneGeneration + 1);
			CHECK(session->GetLoadParameters() == Json{ { "Score", 42 } });
			CHECK(session->GetSerial() == specification.Serial);
			session->Tick();
			auto recorded = session->GetRecorder()->Finish(*session);
			REQUIRE(recorded);
			CHECK(recorded->Header.Parameters == specification.Parameters);
			CHECK(recorded->FinalTick == 2);
			REQUIRE(recorded->Events.size() == 2);
			CHECK(recorded->Events[0].State == "Down");
			CHECK(recorded->Events[1].State == "Up");
			CHECK(recorded->Events[1].Tick == 1);
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		// ADR 0038, friction F17: a Scene.Load or Scene.Reload takes effect at the end of the frame (ConsumeRequests), so in a
		// frame that runs several fixed steps the later ones still run in the current scene and VM, and a second request made
		// in them is refused with InvalidState while the first one still applies (Rolling Ball's Restart and delayed load).
		TEST_CASE("PlaySession: a scene transition waits for the frame's end, so the frame's later fixed steps run in the current scene and refuse another request")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle sourceScript(0x710012), targetScript(0x710013), targetScene(0x710014);
			REQUIRE(fixture.AddScript(sourceScript, "Assets/Scripts/First.luau", R"(
local T = {}
function T:OnCreate()
	self.Steps = 0
end
function T:OnFixedUpdate()
	self.Steps += 1
	self.Entity.Name = "Step" .. self.Steps
	if self.Steps == 1 then
		Scene.Load(Assets.Load("Assets/Scenes/Second.scene"), { From = 1 })
	elseif self.Steps == 2 then
		local ok, message = pcall(function() Scene.Reload({ From = 2 }) end)
		if not ok and string.find(tostring(message), "InvalidState", 1, true) then
			Scene.CreateEntity("Refused")
		end
	end
end
return Script.Define("First", T)
)"));
			REQUIRE(fixture.AddScript(targetScript, "Assets/Scripts/Second.luau", R"(
local T = {}
function T:OnCreate()
	self.Entity.Name = "Loaded" .. Scene.GetLoadParameters().From
end
return Script.Define("Second", T)
)"));
			Test::SceneTestFixture target;
			Test::AttachSessionScript(target.GetScene().CreateEntity("Second"), targetScript);
			auto document = SceneSerializer::ToJson(target.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			fixture.GetAssetManager().Publish(targetScene, data, "Assets/Scenes/Second.scene");
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("First"), sourceScript);
			auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE(created);
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			const uint64_t vm = session->GetScripts()->GetGeneration();
			const uint64_t sceneGeneration = session->GetSceneGeneration();

			// One frame of two fixed steps: the load requested in the first waits, and the second runs in the same scene and VM.
			session->FixedStep();
			CHECK(session->GetScene().FindEntityByPath("/Step1").IsValid());
			session->FixedStep();
			CHECK(session->GetScene().FindEntityByPath("/Step2").IsValid());
			CHECK(session->GetScene().FindEntityByPath("/Refused").IsValid());
			CHECK(session->GetScripts()->GetGeneration() == vm);
			CHECK(session->GetSceneGeneration() == sceneGeneration);
			session->FrameUpdate(FrameTime{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 1.0, .FrameIndex = 0 });

			// The first request applied at the frame's end; the refused one changed nothing.
			CHECK(session->GetSceneGeneration() == sceneGeneration + 1);
			CHECK(session->GetScripts()->GetGeneration() != vm);
			CHECK(session->GetLoadParameters() == Json{ { "From", 1 } });
			CHECK(session->GetScene().FindEntityByPath("/Loaded1").IsValid());
			CHECK_FALSE(session->GetScene().FindEntityByPath("/Refused").IsValid());
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		TEST_CASE("PlaySession: error cursors survive scene replacement and external eval invalidates recording")
		{
			Test::ScriptTestFixture fixture;
			auto document = SceneSerializer::ToJson(fixture.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			const AssetHandle scene(0x710006);
			fixture.GetAssetManager().Publish(scene, data, "Assets/Scenes/Empty.scene");
			auto specification = Test::ScriptSessionSpecification(fixture);
			auto result = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(result);
			auto session = std::move(*result);
			session->SetExtractionEnabled(false);
			Test::ExpectLog expected(LogLevel::Error, "session cursor probe");
			// A replay expectation's failure is the game's, which the session's stream retains (a failed external Evaluate is
			// its caller's: ADR 0038, friction F01).
			const auto compiled = ScriptCompiler::Compile({ .Source = "error('session cursor probe')", .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=probe" });
			REQUIRE_MESSAGE(compiled.has_value(), (compiled ? "" : compiled.error().ToString()));
			ScriptData probe{};
			probe.Bytecode = compiled->Bytecode;
			probe.SourceMap = compiled->SourceMap;
			CHECK_FALSE(session->GetScripts()->ExecuteBytecode(probe));
			const uint64_t cursor = session->GetScriptErrors().GetCursor();
			REQUIRE(cursor > 0);
			REQUIRE(session->GetScripts()->GetHost().RequestSceneLoad(scene, Json::object()));
			session->Tick();
			CHECK_FALSE(session->GetScripts()->ExecuteBytecode(probe));
			const auto repeated = session->GetScriptErrors().Read(cursor);
			REQUIRE(repeated.size() == 1);
			CHECK(repeated[0].Count == 2);
			CHECK(repeated[0].ID > cursor);
			// A new tick-zero session is required for recording. Reads and calls rejected before mutation leave it valid;
			// an external write invalidates it even if a later error is caught or the write is reversed.
			auto fresh = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(fresh);
			ReplayHeader header{};
			header.Scene = { scene, "Assets/Scenes/Empty.scene" };
			header.Seed = specification.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";
			const Status recording = (*fresh)->GetRecorder()->Begin(header, **fresh);
			REQUIRE_MESSAGE(recording, (recording ? "" : recording.error().ToString()));
			REQUIRE((*fresh)->GetScripts()->Evaluate("return Scene.GetEntityCount()", {}));
			REQUIRE((*fresh)->GetScripts()->Evaluate("pcall(function() Time.SetTimeScale(-1) end)", {}));
			CHECK((*fresh)->GetTimeScale() == 1.0);
			REQUIRE((*fresh)->GetRecorder()->Finish(**fresh));
			REQUIRE((*fresh)->GetRecorder()->Begin(header, **fresh));
			REQUIRE((*fresh)->GetScripts()->Evaluate("pcall(function() Time.SetTimeScale(0.5); error('after write') end); Time.SetTimeScale(1)", {}));
			CHECK_FALSE((*fresh)->GetRecorder()->Finish(**fresh));
		}

		// ADR 0038 decision 14 (the S3 fix review's frame-grouping finding): a start flush also starts, round after round, the
		// instances its own OnStart callbacks create, so a nested spawn first runs OnStart and OnFixedUpdate at the same tick
		// however the ticks are grouped into frames. Before it, Level2 (created by Level1's OnStart) started at the next
		// flush: the next tick's in lockstep, the frame phase's when that tick shared Level1's frame, a tick later.
		TEST_CASE("PlaySession: nested spawns from OnStart first run at the same tick however the ticks are grouped into frames")
		{
			for (const int depth : { 2, 3 })
			{
				CAPTURE(depth);
				Test::ScriptTestFixture fixture;
				const AssetHandle nestScript(0x710080), spawnerScript(0x710081);
				REQUIRE(fixture.AddScript(nestScript, "Assets/Scripts/Nest.luau", Test::NestSource(depth)));
				REQUIRE(fixture.AddScript(spawnerScript, "Assets/Scripts/Spawner.luau", std::format(R"(local T = {{}}
function T:OnStart()
	self.Fixed = 0
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed == 1 then
	{}
	end
end
return Script.Define("Spawner", T)
)",
																							Test::SpawnLevel1)));
				Test::AttachSessionScript(fixture.GetScene().CreateEntity("Spawner"), spawnerScript);
				// Level1 is created in tick 0's OnFixedUpdate; every level starts before tick 1's OnFixedUpdate.
				std::vector<std::string> expected;
				for (int level = 1; level <= depth; ++level)
					expected.push_back(std::format("Level{}:start:1", level));
				for (int level = 1; level <= depth; ++level)
					expected.push_back(std::format("Level{}:fixed:1", level));
				const Test::GroupedRun lockstep = Test::RunGrouped(fixture, 6, 1);
				CHECK(lockstep.Trace == expected);
				for (const uint32_t steps : { 2u, 3u })
				{
					CAPTURE(steps);
					const Test::GroupedRun grouped = Test::RunGrouped(fixture, 6, steps);
					CHECK(grouped.Trace == lockstep.Trace);
					CHECK(grouped.StateHash == lockstep.StateHash);
				}
			}
		}

		// Decision 14: the start flush of the session's setup starts the entities its OnStart callbacks spawn, before tick 0.
		TEST_CASE("PlaySession: entities spawned by a startup OnStart start before tick 0 however the ticks are grouped into frames")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle nestScript(0x710082), spawnerScript(0x710083);
			REQUIRE(fixture.AddScript(nestScript, "Assets/Scripts/Nest.luau", Test::NestSource(3)));
			REQUIRE(fixture.AddScript(spawnerScript, "Assets/Scripts/Spawner.luau", std::format(R"(local T = {{}}
function T:OnStart()
	{}
end
return Script.Define("Spawner", T)
)",
																						Test::SpawnLevel1)));
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Spawner"), spawnerScript);
			const std::vector<std::string> expected = { "Level1:start:0", "Level2:start:0", "Level3:start:0", "Level1:fixed:0", "Level2:fixed:0", "Level3:fixed:0" };
			const Test::GroupedRun lockstep = Test::RunGrouped(fixture, 6, 1);
			CHECK(lockstep.Trace == expected);
			for (const uint32_t steps : { 2u, 3u })
			{
				CAPTURE(steps);
				const Test::GroupedRun grouped = Test::RunGrouped(fixture, 6, steps);
				CHECK(grouped.Trace == lockstep.Trace);
				CHECK(grouped.StateHash == lockstep.StateHash);
			}
		}

		// Decision 14: after Scene.Load, the new scene's start flush starts the entities its OnStart callbacks spawn, before
		// its first tick. The load is requested in tick 5, the last of a frame at one, two and three steps a frame, so the new
		// scene's first tick is 6 in every grouping (a request waits for its frame's end, friction F17).
		TEST_CASE("PlaySession: entities spawned by OnStart after Scene.Load start before the new scene's first tick however the ticks are grouped into frames")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle nestScript(0x710084), spawnerScript(0x710085), loaderScript(0x710086), secondScene(0x710087);
			REQUIRE(fixture.AddScript(nestScript, "Assets/Scripts/Nest.luau", Test::NestSource(3)));
			REQUIRE(fixture.AddScript(spawnerScript, "Assets/Scripts/Spawner.luau", std::format(R"(local T = {{}}
function T:OnStart()
	{}
end
return Script.Define("Spawner", T)
)",
																						Test::SpawnLevel1)));
			REQUIRE(fixture.AddScript(loaderScript, "Assets/Scripts/Loader.luau", R"(local T = {}
function T:OnStart()
	self.Fixed = 0
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed == 6 then
		Scene.Load(Assets.Load("Assets/Scenes/Second.scene"))
	end
end
return Script.Define("Loader", T)
)"));
			Test::SceneTestFixture second;
			Test::AttachSessionScript(second.GetScene().CreateEntity("Spawner"), spawnerScript);
			auto document = SceneSerializer::ToJson(second.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			fixture.GetAssetManager().Publish(secondScene, data, "Assets/Scenes/Second.scene");
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Loader"), loaderScript);
			const std::vector<std::string> expected = { "Level1:start:6", "Level2:start:6", "Level3:start:6", "Level1:fixed:6", "Level2:fixed:6", "Level3:fixed:6" };
			const Test::GroupedRun lockstep = Test::RunGrouped(fixture, 12, 1);
			CHECK(lockstep.Trace == expected);
			for (const uint32_t steps : { 2u, 3u })
			{
				CAPTURE(steps);
				const Test::GroupedRun grouped = Test::RunGrouped(fixture, 12, steps);
				CHECK(grouped.Trace == lockstep.Trace);
				CHECK(grouped.StateHash == lockstep.StateHash);
			}
		}

		// ADR 0038 decision 14 (the lead's follow-up decision): an entity created during a session starts only in a fixed
		// step's start flush, never in the frame phase, so its OnStart reads the tick's input view and delta, and what it
		// destroys leaves before the tick's physics step, however the ticks are grouped into frames. Before it, an entity
		// created in the fixed step that ended a frame (every tick in lockstep) started in the frame phase: Early read the
		// frame view (Move still 0) in lockstep and the next tick's step view (1) on two or three steps a frame, Late read a
		// delta of two fixed steps when its tick ended a frame of two, and the Floor Early destroys still held the Ball in
		// the next tick's physics step when that tick shared Early's frame.
		TEST_CASE("PlaySession: what a spawned entity's OnStart reads and destroys is the same however the ticks are grouped into frames")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle readerScript(0x71008a), spawnerScript(0x71008b);
			REQUIRE(fixture.AddScript(readerScript, "Assets/Scripts/Reader.luau", std::string(Test::ReaderSource)));
			REQUIRE(fixture.AddScript(spawnerScript, "Assets/Scripts/Spawner.luau", R"(local T = {}
local function spawn(name: string)
	local entity = Scene.CreateEntity(name)
	entity:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/Reader.luau"), Fields = {}, ExecutionOrder = 0}}})
end
function T:OnStart()
	self.Fixed = 0
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed == 1 then
		spawn("Early")
	elseif self.Fixed == 2 then
		spawn("Late")
	end
end
return Script.Define("Spawner", T)
)"));
			Scene& editScene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(editScene, "Floor", glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(2.0f, 0.5f, 2.0f), BodyType::Static));
			static_cast<void>(Test::AddSphereBody(editScene, "Ball", glm::vec3(0.0f, 0.5f, 0.0f), 0.5f, BodyType::Dynamic));
			Test::AttachSessionScript(editScene.CreateEntity("Spawner"), spawnerScript);
			// Early is created in tick 0's OnFixedUpdate and starts at tick 1's flush, Late in tick 1's and starts at tick 2's,
			// each with the step view (Move 1 from tick 1 on) and the fixed delta.
			const std::vector<std::string> expected = { "Early:axis:1:dt:1", "Late:axis:1:dt:1" };
			const Test::GroupedRun lockstep = Test::RunGroupedWithMove(fixture, 6, 1);
			CHECK(lockstep.Trace == expected);
			for (const uint32_t steps : { 2u, 3u })
			{
				CAPTURE(steps);
				const Test::GroupedRun grouped = Test::RunGroupedWithMove(fixture, 6, steps);
				CHECK(grouped.Trace == lockstep.Trace);
				CHECK(grouped.StateHash == lockstep.StateHash);
			}
		}

		// Decision 14: a scene transition's startup callbacks run as a session's do, in the step view of the tick the frame
		// ended with and with the fixed delta. Before it, the new scene's OnStart read the delta of the frame that requested
		// the load: two or three fixed steps on two or three steps a frame. The load is requested in tick 5, the last of a
		// frame in every grouping here, so the new scene starts after tick 5 in all of them (friction F17).
		TEST_CASE("PlaySession: a scene's startup callbacks after Scene.Load read the same input and delta however the ticks are grouped into frames")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle readerScript(0x71008c), loaderScript(0x71008d), secondScene(0x71008e);
			REQUIRE(fixture.AddScript(readerScript, "Assets/Scripts/Reader.luau", std::string(Test::ReaderSource)));
			REQUIRE(fixture.AddScript(loaderScript, "Assets/Scripts/Loader.luau", R"(local T = {}
function T:OnStart()
	self.Fixed = 0
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed == 6 then
		Scene.Load(Assets.Load("Assets/Scenes/Second.scene"))
	end
end
return Script.Define("Loader", T)
)"));
			Test::SceneTestFixture second;
			Test::AttachSessionScript(second.GetScene().CreateEntity("Loaded"), readerScript);
			auto document = SceneSerializer::ToJson(second.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			fixture.GetAssetManager().Publish(secondScene, data, "Assets/Scenes/Second.scene");
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Loader"), loaderScript);
			const Test::GroupedRun lockstep = Test::RunGroupedWithMove(fixture, 12, 1);
			CHECK(lockstep.Trace == std::vector<std::string>{ "Loaded:axis:1:dt:1" });
			for (const uint32_t steps : { 2u, 3u })
			{
				CAPTURE(steps);
				const Test::GroupedRun grouped = Test::RunGroupedWithMove(fixture, 12, steps);
				CHECK(grouped.Trace == lockstep.Trace);
				CHECK(grouped.StateHash == lockstep.StateHash);
			}
		}

		// Decision 14: an entity created in a fixed step starts at the next fixed step's start flush, so the frame phase between
		// passes it by: its first OnUpdate follows its OnStart and first OnFixedUpdate.
		TEST_CASE("PlaySession: an entity created in a fixed step starts at the next fixed step's start flush and runs no OnUpdate before it")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle spawnerScript(0x71008f), probeScript(0x710090);
			REQUIRE(fixture.AddScript(probeScript, "Assets/Scripts/Phases.luau", R"(local T = {}
local function trace(self, what: string)
	Scene.CreateEntity("trace:" .. self.Entity.Name .. ":" .. what .. ":" .. Time.GetTick())
end
function T:OnStart()
	self.Updates = 0
	trace(self, "start")
end
function T:OnFixedUpdate()
	if not self.Fixed then
		self.Fixed = true
		trace(self, "fixed")
	end
end
function T:OnUpdate()
	self.Updates += 1
	if self.Updates == 1 then
		trace(self, "update")
	end
end
return Script.Define("Phases", T)
)"));
			REQUIRE(fixture.AddScript(spawnerScript, "Assets/Scripts/Spawner.luau", R"(local T = {}
function T:OnFixedUpdate()
	if not self.Spawned then
		self.Spawned = true
		local entity = Scene.CreateEntity("Spawned")
		entity:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/Phases.luau"), Fields = {}, ExecutionOrder = 0}}})
	end
end
return Script.Define("Spawner", T)
)"));
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Spawner"), spawnerScript);
			auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			Test::RunTicks(*session, 3);
			// Created in tick 0's OnFixedUpdate: frame 0's OnUpdate passes it by; it starts at tick 1's start flush.
			CHECK(Test::SessionTrace(session->GetScene()) == std::vector<std::string>{ "Spawned:start:1", "Spawned:fixed:1", "Spawned:update:2" });
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		// ADR 0038 decision 14 (friction F19 and the S3 fix review's finding 1): the start flush synchronizes the physics bodies
		// of both worlds before each of its rounds, so an entity has its body from its OnStart on, wherever it was created:
		// before the tick's pre-step (OnFixedUpdate, a task), after it (a 3D or a 2D physics callback, an OnDestroy in the
		// destroy flush, OnUpdate, OnLateUpdate, an evaluation between ticks) or by the session's startup callbacks (OnCreate,
		// OnStart). This case was F19's "an entity created after a tick's physics pre-step gets its body only at the next
		// tick's pre-step, after its OnStart and first OnFixedUpdate", which pinned the window the decision closed.
		TEST_CASE("PlaySession: an entity has its physics body from its OnStart on, wherever it was created")
		{
			for (const bool is2D : { false, true })
			{
				CAPTURE(is2D);
				Test::ScriptTestFixture fixture;
				const AssetHandle spawnerScript(0x710070), probeScript(0x710071), toucherScript(0x710073), doomedScript(0x710074);
				const std::string spawn = Test::ProbeSpawnSource(is2D);
				REQUIRE(fixture.AddScript(spawnerScript, "Assets/Scripts/Spawner.luau", spawn + R"(
local T = {}
function T:OnCreate()
	Spawn("InCreate", 10)
end
function T:OnStart()
	self.Fixed = 0
	self.Frames = 0
	self.LateFrames = 0
	Spawn("InStart", 15)
	Task.Spawn(function()
		Task.WaitTicks(1)
		Spawn("InTask", 20)
	end)
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed == 1 then
		Spawn("InFixedUpdate", 30)
	end
end
function T:OnTriggerEnter(other)
	if not self.Entered then
		self.Entered = true
		Spawn("InTriggerEnter", 40)
	end
end
function T:OnUpdate()
	self.Frames += 1
	if self.Frames == 1 then
		Spawn("InUpdate", 50)
	end
end
function T:OnLateUpdate()
	self.LateFrames += 1
	if self.LateFrames == 1 then
		Spawn("InLateUpdate", 55)
	end
end
return Script.Define("Spawner", T)
)"));
				REQUIRE(fixture.AddScript(toucherScript, "Assets/Scripts/Toucher.luau", spawn + R"(
local T = {}
function T:OnTriggerEnter2D(other)
	if not self.Entered then
		self.Entered = true
		Spawn("InTriggerEnter2D", 45)
	end
end
return Script.Define("Toucher", T)
)"));
				REQUIRE(fixture.AddScript(doomedScript, "Assets/Scripts/Doomed.luau", spawn + R"(
local T = {}
function T:OnFixedUpdate()
	self.Entity:Destroy()
end
function T:OnDestroy()
	Spawn("InDestroy", 60)
end
return Script.Define("Doomed", T)
)"));
				REQUIRE(fixture.AddScript(probeScript, "Assets/Scripts/Probe.luau", Test::ProbeSource(is2D)));
				Scene& editScene = fixture.GetScene();
				// A 3D trigger the Spawner overlaps and a 2D one the Toucher overlaps: both enter at tick 0's post-steps.
				const Entity zone = Test::AddBoxBody(editScene, "Zone", glm::vec3(0.0f), glm::vec3(5.0f), BodyType::Static);
				zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
				{
					collider.IsTrigger = true;
				});
				const Entity spawner = Test::AddSphereBody(editScene, "Spawner", glm::vec3(0.0f), 0.5f, BodyType::Dynamic);
				Test::AttachSessionScript(spawner, spawnerScript);
				Entity zone2D = editScene.CreateEntity("Zone2D");
				zone2D.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
				zone2D.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(5.0f), .IsTrigger = true });
				Entity toucher = editScene.CreateEntity("Toucher");
				toucher.AddComponent<RigidBody2DComponent>();
				toucher.AddComponent<CircleCollider2DComponent>();
				Test::AttachSessionScript(toucher, toucherScript);
				Test::AttachSessionScript(editScene.CreateEntity("Doomed"), doomedScript);
				auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
				REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
				auto session = std::move(*created);
				session->SetExtractionEnabled(false);

				// Tick 0: InFixedUpdate in OnFixedUpdate, InTriggerEnter and InTriggerEnter2D in the post-steps' dispatches,
				// InDestroy in the destroy flush, InUpdate and InLateUpdate in the frame phase.
				session->Tick();
				// Between ticks, as script.eval {context: Play} runs in a lockstep session.
				const auto evaluated = session->GetScripts()->Evaluate(spawn + "Spawn(\"InEval\", 70)", VfsPath{});
				REQUIRE_MESSAGE(evaluated, (evaluated ? "" : evaluated.error().ToString()));
				for (int tick = 0; tick < 4; ++tick)
					session->Tick();
				CHECK(session->GetScriptErrors().GetErrors().empty());

				const std::vector<std::string> trace = Test::SessionTrace(session->GetScene());
				const auto traceOf = [&trace](std::string_view name)
				{
					std::vector<std::string> phases;
					const std::string prefix = std::string(name) + ":";
					for (const std::string& entry : trace)
					{
						if (entry.starts_with(prefix))
							phases.push_back(entry.substr(prefix.size()));
					}
					return phases;
				};
				const std::vector<std::string> withBody = { "start:body", "fixed:body", "fixed:body" };
				for (const std::string_view path : { "InCreate", "InStart", "InFixedUpdate", "InTask", "InTriggerEnter", "InTriggerEnter2D", "InDestroy",
						 "InUpdate", "InLateUpdate", "InEval" })
				{
					CAPTURE(path);
					CHECK(traceOf(path) == withBody);
				}
			}
		}

		// Decision 14: the startup callbacks of a scene that Scene.Load or Scene.Reload made current run in the new scene's
		// start flush, whose rounds synchronize the bodies too, so what their OnCreate and OnStart spawn has its body from its
		// OnStart on, in 3D and 2D.
		TEST_CASE("PlaySession: entities spawned by the startup callbacks after Scene.Load and Scene.Reload have their physics bodies from their OnStart on")
		{
			for (const bool is2D : { false, true })
			{
				CAPTURE(is2D);
				Test::ScriptTestFixture fixture;
				const AssetHandle loaderScript(0x710075), spawnerScript(0x710076), probeScript(0x710077), secondScene(0x710078);
				const std::string spawn = Test::ProbeSpawnSource(is2D);
				REQUIRE(fixture.AddScript(loaderScript, "Assets/Scripts/Loader.luau", R"(local T = {}
function T:OnFixedUpdate()
	Scene.Load(Assets.Load("Assets/Scenes/Second.scene"))
end
return Script.Define("Loader", T)
)"));
				REQUIRE(fixture.AddScript(spawnerScript, "Assets/Scripts/Spawner.luau", spawn + R"(
local T = {}
function T:OnCreate()
	self.Prefix = if Scene.GetLoadParameters().Reloaded then "Reload" else "Load"
	Spawn(self.Prefix .. "InCreate", 10)
end
function T:OnStart()
	self.Fixed = 0
	Spawn(self.Prefix .. "InStart", 20)
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed == 3 and self.Prefix == "Load" then
		Scene.Reload({ Reloaded = true })
	end
end
return Script.Define("Spawner", T)
)"));
				REQUIRE(fixture.AddScript(probeScript, "Assets/Scripts/Probe.luau", Test::ProbeSource(is2D)));
				Test::SceneTestFixture second;
				Test::AttachSessionScript(second.GetScene().CreateEntity("Spawner"), spawnerScript);
				auto document = SceneSerializer::ToJson(second.GetScene());
				REQUIRE(document);
				auto data = CreateRef<SceneData>();
				data->Document = CreateRef<const Json>(std::move(*document));
				fixture.GetAssetManager().Publish(secondScene, data, "Assets/Scenes/Second.scene");
				Test::AttachSessionScript(fixture.GetScene().CreateEntity("Loader"), loaderScript);
				auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
				REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
				auto session = std::move(*created);
				session->SetExtractionEnabled(false);
				const std::vector<std::string> withBody = { "start:body", "fixed:body", "fixed:body" };
				const auto check = [&session, &withBody](std::string_view prefix)
				{
					const std::vector<std::string> trace = Test::SessionTrace(session->GetScene());
					for (const std::string_view path : { "InCreate", "InStart" })
					{
						std::vector<std::string> phases;
						const std::string name = std::format("{}{}:", prefix, path);
						for (const std::string& entry : trace)
						{
							if (entry.starts_with(name))
								phases.push_back(entry.substr(name.size()));
						}
						CAPTURE(name);
						CHECK(phases == withBody);
					}
				};

				// Tick 0 requests the load, which applies at its frame's end; ticks 1 and 2 run the probes' first fixed steps.
				Test::RunTicks(*session, 3);
				check("Load");
				// Tick 3 requests the reload; ticks 4 and 5 run the new probes' first fixed steps.
				Test::RunTicks(*session, 3);
				check("Reload");
				CHECK(session->GetScriptErrors().GetErrors().empty());
			}
		}

		// Decision 14's bound: a start flush runs at most MaxScriptStartRounds rounds, so an OnStart chain without end (each
		// OnStart creating the next scripted entity) is reported as a script error of each instance still waiting, which is
		// disabled instead of started, and the session goes on.
		TEST_CASE("PlaySession: a start flush stops a runaway OnStart chain at its round limit with a script error")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle chainScript(0x710079);
			REQUIRE(fixture.AddScript(chainScript, "Assets/Scripts/Chain.luau", R"(local T = {}
function T:OnStart()
	local next = Scene.CreateEntity("Chain")
	next:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/Chain.luau"), Fields = {}, ExecutionOrder = 0}}})
	Scene.CreateEntity("trace:started")
end
return Script.Define("Chain", T)
)"));
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Chain"), chainScript);
			Scope<PlaySession> session;
			{
				Test::ExpectLog expected(LogLevel::Error, "limit of 16 rounds");
				auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
				REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
				session = std::move(*created);
			}
			session->SetExtractionEnabled(false);
			// The session's setup flush started the scene's Chain and the 15 it created round after round; the 16th created
			// waits past the limit and is disabled.
			CHECK(Test::SessionTrace(session->GetScene()).size() == MaxScriptStartRounds);
			REQUIRE(session->GetScriptErrors().GetErrors().size() == 1);
			const ScriptError& error = session->GetScriptErrors().GetErrors().front();
			CHECK(error.Callback == "OnStart");
			CHECK(error.Script == "Assets/Scripts/Chain.luau");
			CHECK(error.Count == 1);
			// The disabled instance never starts, so the chain stays ended in the next ticks.
			Test::RunTicks(*session, 3);
			CHECK(Test::SessionTrace(session->GetScene()).size() == MaxScriptStartRounds);
			CHECK(session->GetScriptErrors().GetErrors().size() == 1);
		}

		// ADR 0038, friction F18: RigidBody:MoveKinematic writes the entity's pose, and the tick's physics pre-step (§5.7 step
		// 5, after OnFixedUpdate and the tasks, where test cases run) sweeps the body there with the velocities that reach it
		// in one fixed step. A Kinematic body's velocities are those of its last sweep: in the tick of a MoveKinematic, the
		// fixed step still reads the previous tick's, while the frame phase, an evaluation between ticks and the next tick's
		// fixed step read the new sweep's.
		TEST_CASE("PlaySession: a Kinematic body's velocities are those of its last physics step, so a MoveKinematic shows in them from its tick's physics step on")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle moverScript(0x710072);
			REQUIRE(fixture.AddScript(moverScript, "Assets/Scripts/Mover.luau", R"(
local T = {}
function T:OnStart()
	self.Fixed = 0
end
function T:OnFixedUpdate()
	self.Fixed += 1
	local body = self.Entity.RigidBody :: RigidBody
	if self.Fixed == 3 then
		self.BeforeMove = body:GetLinearVelocity()
		body:MoveKinematic(vector.create(0, 2, 0), Quat.FromEuler(0, 6, 0))
		self.AfterMove = body:GetLinearVelocity()
		self.AfterMoveAngular = body:GetAngularVelocity()
	elseif self.Fixed == 4 then
		self.NextTick = body:GetLinearVelocity()
		self.NextTickAngular = body:GetAngularVelocity()
	end
end
function T:OnUpdate()
	if self.Fixed == 3 then
		self.FrameAfterMove = (self.Entity.RigidBody :: RigidBody):GetLinearVelocity()
	end
end
return Script.Define("Mover", T)
)"));
			const Entity platform = Test::AddBoxBody(fixture.GetScene(), "Platform", glm::vec3(0.0f), glm::vec3(1.0f, 0.25f, 1.0f), BodyType::Kinematic);
			Test::AttachSessionScript(platform, moverScript);
			auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);
			const UUID id = platform.GetUUID();
			const auto read = [&session, id](std::string_view member)
			{
				const auto value = session->GetScripts()->Evaluate(std::format("return self.{}", member), VfsPath{}, id);
				REQUIRE_MESSAGE(value, (value ? "" : value.error().ToString()));
				const Json json = value->Value.Get();
				REQUIRE_MESSAGE((json.is_array() && json.size() == 3), json.dump());
				glm::vec3 velocity(0.0f);
				for (int axis = 0; axis < 3; ++axis)
				{
					const auto component = JsonReader(json).GetElement(static_cast<size_t>(axis)).and_then([](const JsonReader& element)
					{
						return element.ReadFloat();
					});
					REQUIRE_MESSAGE(component, json.dump());
					velocity[axis] = *component;
				}
				return velocity;
			};
			const auto near = [](const glm::vec3& actual, const glm::vec3& expected)
			{
				return glm::all(glm::lessThanEqual(glm::abs(actual - expected), glm::vec3(1.0e-3f * std::max(1.0f, glm::length(expected)))));
			};
			const float fixedHz = 1.0f / static_cast<float>(session->GetFixedDelta());
			const glm::vec3 jump(0.0f, 2.0f * fixedHz, 0.0f);
			const glm::vec3 turn(0.0f, glm::radians(6.0f) * fixedHz, 0.0f);

			// Two resting ticks, then the tick that moves the platform 2 m up and turns it 6 degrees.
			Test::RunTicks(*session, 3);
			CHECK(near(read("BeforeMove"), glm::vec3(0.0f)));
			// In its own tick's fixed step the body has not swept yet: it still reads its last (resting) step.
			CHECK(near(read("AfterMove"), glm::vec3(0.0f)));
			CHECK(near(read("AfterMoveAngular"), glm::vec3(0.0f)));
			// From the tick's physics step on, the sweep that carried it there in one step.
			CHECK(near(read("FrameAfterMove"), jump));
			CHECK(near(session->GetPhysics().GetLinearVelocity(id).value_or(glm::vec3(0.0f)), jump));
			CHECK(near(session->GetPhysics().GetAngularVelocity(id).value_or(glm::vec3(0.0f)), turn));
			// The next tick's fixed step still reads that sweep; its own step, which does not move the body, rests it again.
			session->Tick();
			CHECK(near(read("NextTick"), jump));
			CHECK(near(read("NextTickAngular"), turn));
			CHECK(near(session->GetPhysics().GetLinearVelocity(id).value_or(glm::vec3(1.0f)), glm::vec3(0.0f)));
			CHECK(near(session->GetPhysics().GetAngularVelocity(id).value_or(glm::vec3(1.0f)), glm::vec3(0.0f)));
			CHECK(near(Test::GetWorldPosition(*session, "/Platform"), glm::vec3(0.0f, 2.0f, 0.0f)));
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		TEST_CASE("PlaySession: Simulate never initializes scripts or accepts a recorder")
		{
			Test::ScriptTestFixture fixture;
			Test::AttachSessionScript(fixture.GetScene().CreateEntity("Missing"), AssetHandle(0x71ffff));
			auto specification = Test::ScriptSessionSpecification(fixture);
			specification.Mode = PlayMode::Simulate;
			auto result = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE(result);
			CHECK((*result)->GetScripts() == nullptr);
			CHECK((*result)->GetRecorder() == nullptr);
			(*result)->SetExtractionEnabled(false);
			(*result)->Tick();
			CHECK((*result)->GetTick() == 1);
		}
	}

}
