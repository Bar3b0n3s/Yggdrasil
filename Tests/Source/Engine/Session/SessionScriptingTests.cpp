#include "TestsPCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
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

		// ADR 0038, friction F19: an entity's body is created at the first physics pre-step after the entity was created
		// (§5.7 step 5), and its OnStart runs at the first start flush after it. One created before a tick's pre-step (in
		// OnFixedUpdate or a task, as a test case does) has its body by its OnStart; one created after it (in a physics
		// callback, in the frame phase, or between ticks by script.eval) starts and runs its first OnFixedUpdate at the next
		// tick before that tick's pre-step creates its body, so its body methods raise there (Rolling Ball's S3 probe).
		TEST_CASE("PlaySession: an entity created after a tick's physics pre-step gets its body only at the next tick's pre-step, after its OnStart and first OnFixedUpdate")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle spawnerScript(0x710070), probeScript(0x710071);
			REQUIRE(fixture.AddScript(spawnerScript, "Assets/Scripts/Spawner.luau", R"(
local T = {}
function T.Spawn(name: string, x: number)
	local entity = Scene.CreateEntity(name)
	entity.Transform.Translation = vector.create(x, 0, 0)
	entity:AddComponent("BoxCollider", {HalfExtents = vector.create(1, 0.25, 1)})
	entity:AddComponent("RigidBody", {Type = "Kinematic"})
	entity:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/Probe.luau"), Fields = {}, ExecutionOrder = 0}}})
end
function T:OnStart()
	self.Fixed = 0
	self.Frames = 0
	Task.Spawn(function()
		Task.WaitTicks(1)
		T.Spawn("InTask", 20)
	end)
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed == 1 then
		T.Spawn("InFixedUpdate", 30)
	end
end
function T:OnTriggerEnter(other)
	if not self.Entered then
		self.Entered = true
		T.Spawn("InTriggerEnter", 40)
	end
end
function T:OnUpdate()
	self.Frames += 1
	if self.Frames == 1 then
		T.Spawn("InUpdate", 50)
	end
end
return Script.Define("Spawner", T)
)"));
			REQUIRE(fixture.AddScript(probeScript, "Assets/Scripts/Probe.luau", R"(
local T = {}
local function record(self, phase: string, ok: boolean)
	Scene.CreateEntity("trace:" .. self.Entity.Name .. ":" .. phase .. ":" .. (if ok then "body" else "none"))
end
function T:OnStart()
	self.Fixed = 0
	record(self, "start", Physics.GetBodyBounds(self.Entity) ~= nil)
end
function T:OnFixedUpdate()
	self.Fixed += 1
	if self.Fixed <= 2 then
		local transform = self.Entity.Transform
		local moved = pcall(function()
			self.Entity.RigidBody:MoveKinematic(transform.WorldPosition, transform.WorldRotation)
		end)
		record(self, "fixed", moved)
	end
end
return Script.Define("Probe", T)
)"));
			Scene& editScene = fixture.GetScene();
			const Entity zone = Test::AddBoxBody(editScene, "Zone", glm::vec3(0.0f), glm::vec3(5.0f), BodyType::Static);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			const Entity spawner = Test::AddSphereBody(editScene, "Spawner", glm::vec3(0.0f), 0.5f, BodyType::Dynamic);
			Test::AttachSessionScript(spawner, spawnerScript);
			auto created = PlaySession::CreateFromScene(Test::ScriptSessionSpecification(fixture), fixture.GetScene());
			REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
			auto session = std::move(*created);
			session->SetExtractionEnabled(false);

			// Tick 0: InFixedUpdate in OnFixedUpdate, InTriggerEnter in PostStep's dispatch, InUpdate in the frame phase.
			session->Tick();
			// Between ticks, as script.eval {context: Play} runs in a lockstep session.
			const auto evaluated = session->GetScripts()->Evaluate(R"(
local entity = Scene.CreateEntity("InEval")
entity.Transform.Translation = vector.create(60, 0, 0)
entity:AddComponent("BoxCollider", {HalfExtents = vector.create(1, 0.25, 1)})
entity:AddComponent("RigidBody", {Type = "Kinematic"})
entity:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/Probe.luau"), Fields = {}, ExecutionOrder = 0}}})
)",
				VfsPath{});
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
			// Created before the tick's pre-step: the body exists by OnStart.
			const std::vector<std::string> beforePreStep = { "start:body", "fixed:body", "fixed:body" };
			CHECK(traceOf("InFixedUpdate") == beforePreStep);
			CHECK(traceOf("InTask") == beforePreStep);
			// Created after it: OnStart and the first OnFixedUpdate run before the next tick's pre-step creates the body.
			const std::vector<std::string> afterPreStep = { "start:none", "fixed:none", "fixed:body" };
			CHECK(traceOf("InTriggerEnter") == afterPreStep);
			CHECK(traceOf("InUpdate") == afterPreStep);
			CHECK(traceOf("InEval") == afterPreStep);
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
