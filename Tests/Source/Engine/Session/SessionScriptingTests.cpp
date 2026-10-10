#include "TestsPCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <format>
#include <string>
#include <vector>

namespace Engine {

	namespace Test {

		static void AttachSessionScript(Entity entity, AssetHandle script)
		{
			ScriptComponent component{};
			component.Script.SetHandle(script);
			entity.AddComponent<ScriptComponent>(component);
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
			CHECK_FALSE(session->GetScripts()->Evaluate("error('session cursor probe')", {}));
			const uint64_t cursor = session->GetScriptErrors().GetCursor();
			REQUIRE(cursor > 0);
			REQUIRE(session->GetScripts()->GetHost().RequestSceneLoad(scene, Json::object()));
			session->Tick();
			CHECK_FALSE(session->GetScripts()->Evaluate("error('session cursor probe')", {}));
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
