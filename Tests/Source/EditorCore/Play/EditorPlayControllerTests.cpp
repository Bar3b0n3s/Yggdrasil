#include "TestsPCH.h"

#include "EditorCore/Play/EditorPlayController.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Window.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"
#include "Support/TestOptions.h"
#include "Support/WaitUntil.h"
#include "Support/WindowedChild.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The editor's play mode (Architecture Â§5.6, Â§12.4; Docs/Decisions/0012-m7-decisions.md decisions 3 and 11).

namespace Engine {

	static Json CallEditorHost(Test::AutomationFixture& fixture, std::string_view method, const Json& params = Json::object())
	{
		auto result = fixture.Call(method, params);
		REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
		return std::move(*result);
	}

	static UUID AttachStartupScript(EditorContext& editor, std::string_view callback, std::string_view body)
	{
		const auto path = VfsPath::Create("project", "Assets/Startup.luau");
		REQUIRE(path.has_value());
		REQUIRE(editor.GetScriptService() != nullptr);
		const auto source = std::format("local Startup = {{}}\nfunction Startup:{}()\n{}\nend\nreturn Script.Define(\"Startup\", Startup)\n", callback, body);
		const auto written = editor.GetScriptService()->Write(*path, source);
		REQUIRE_MESSAGE(written.has_value(), (written ? std::string() : written.error().ToString()));
		const auto loaded = editor.GetScriptService()->GetFields(written->Script);
		REQUIRE_MESSAGE(loaded.has_value(), (loaded ? std::string() : loaded.error().ToString()));
		REQUIRE((*loaded)->Kind == ScriptKind::Behaviour);
		const Entity entity = editor.GetScene().CreateEntity("Startup");
		ScriptComponent script;
		script.Script.SetHandle(written->Script);
		entity.AddComponent<ScriptComponent>(script);
		// Recording resolves the saved scene's asset handle before replacing the session.
		REQUIRE(editor.GetAssets().Refresh().has_value());
		return entity.GetUUID();
	}

	static void CheckEditorWindowHost()
	{
		Test::TempDirectory directory("EditorWindowHost");
		auto engine = EngineContext::Create({ .WorkerCount = 0, .Window = WindowSpecification{ .Title = "Editor host test", .Width = 640, .Height = 360 }, .RegisterTypes = &RegisterEditorMethodTypes });
		REQUIRE_MESSAGE(engine.has_value(), (engine ? "" : engine.error().ToString()));
		auto editor = EditorContext::Create(**engine, { .IdGeneratorState = Test::EditorTestIdState });
		REQUIRE(editor);
		const auto created = ProjectManager::CreateProject({ .Directory = directory / "Project", .Name = "Host", .Template = ProjectTemplate::Empty, .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources/Templates/Projects" }, (*engine)->GetTypeRegistry());
		REQUIRE(created);
		auto project = ProjectManager::OpenProject(created->ProjectFile, {}, (*engine)->GetTypeRegistry());
		REQUIRE(project);
		REQUIRE((*editor)->OpenProject(std::move(*project)));
		(*editor)->SetScene((*editor)->CreateScene("Host"), std::nullopt);
		auto& play = (*editor)->GetPlay();
		Window* window = (*engine)->GetWindow();
		REQUIRE(window != nullptr);
		window->PollEvents();
		window->SetEventCallback([&play](Event& event)
		{
			play.OnInputEvent(event, true);
		});
		Test::AutomationTestClient client(**editor);
		const auto eval = [&client](std::string_view context, const std::string& source)
		{
			auto result = client.Call("script.eval", Json{ { "context", context }, { "code", source } });
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			REQUIRE_MESSAGE(result->contains("value"), result->dump());
			return (*result)["value"];
		};
		const auto checkEnvironment = [&eval, window](std::string_view context)
		{
			CHECK(eval(context, "Application.IsEditor()") == Json(true));
			CHECK(eval(context, "Application.IsHeadless()") == Json(GlfwLibrary::GetMode() == WindowMode::Headless));
			CHECK(eval(context, "Application.IsFocused()") == Json(window->IsFocused()));
			CHECK(eval(context, std::format("Application.GetWindowSize() == vector.create({}, {}, 0)", window->GetWidth(), window->GetHeight())) == Json(true));
			CHECK(eval(context, "Application.GetPlatform() ~= '' and Application.GetVersion() ~= ''") == Json(true));
		};
		checkEnvironment("edit");
		static_cast<void>(AttachStartupScript(**editor, "OnStart", R"(
local instance: any = self
instance.StartCount = (instance.StartCount or 0) + 1
instance.Start = {Headless = Application.IsHeadless(), Focused = Application.IsFocused(), Size = Application.GetWindowSize(), Platform = Application.GetPlatform()}
instance.BeforeCursor = Input.GetCursorMode()
Input.SetCursorMode("Locked")
)"));
		const bool startupFocused = window->IsFocused();
		REQUIRE(play.Start({ .Paused = true }));
		CHECK(window->GetCursorMode() == CursorMode::Locked);
		CHECK(eval("play", "Scene.FindByName('Startup'):GetScript().StartCount") == Json(1));
		CHECK(eval("play", "Scene.FindByName('Startup'):GetScript().Start.Headless") == Json(GlfwLibrary::GetMode() == WindowMode::Headless));
		CHECK(eval("play", "Scene.FindByName('Startup'):GetScript().Start.Focused") == Json(startupFocused));
		CHECK(eval("play", "Scene.FindByName('Startup'):GetScript().Start.Size == vector.create(640, 360, 0)") == Json(true));
		CHECK(eval("play", "Scene.FindByName('Startup'):GetScript().Start.Platform ~= ''") == Json(true));
		PlaySession* original = play.GetSession();
		CHECK_FALSE(play.StartRecording({}, true)); // The unsaved scene cannot be recorded; preserve its cursor owner.
		CHECK(play.GetSession() == original);
		CHECK(window->GetCursorMode() == CursorMode::Locked);
		REQUIRE(client.Call("script.write", Json{ { "path", "Assets/Tests/Cursor.test.luau" }, { "source", R"(
return Test.Suite("Cursor", function()
	Test.Case("temporary cursor ownership", function()
		Test.Expect(Input.GetCursorMode() == "Normal")
		Input.SetCursorMode("Hidden")
		Test.Expect(Input.GetCursorMode() == "Hidden")
	end)
end)
)" } }));
		REQUIRE(client.Call("project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Cursor.test.luau" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } }));
		auto tested = client.Call("test.run", Json::object());
		REQUIRE_MESSAGE(tested.has_value(), (tested ? "" : tested.error().ToString()));
		// Coverage can exceed the inline result limit; the RPC then returns the saved report's envelope.
		if (JsonReader(*tested).ReadMember<bool>("truncated").value_or(false))
		{
			const auto path = JsonReader(*tested).ReadMember<std::string>("path");
			REQUIRE_MESSAGE(path.has_value(), (path ? "" : path.error().ToString()));
			const auto text = FileSystem::ReadText(FileSystem::PathFromUtf8(*path));
			REQUIRE_MESSAGE(text.has_value(), (text ? "" : text.error().ToString()));
			tested = JsonReader::Parse(*text);
			REQUIRE_MESSAGE(tested.has_value(), (tested ? "" : tested.error().ToString()));
		}
		const auto passed = JsonReader(*tested).ReadMember<bool>("passed");
		REQUIRE_MESSAGE(passed.has_value(), tested->dump());
		CHECK(*passed);
		CHECK(play.GetSession() == original);
		CHECK(window->GetCursorMode() == CursorMode::Locked);
		REQUIRE(client.Call("scene.save", Json{ { "path", "Assets/Scenes/Host.scene" } }));
		// Saving writes the new scene; discover its metadata before requesting a replay header with a saved asset handle.
		const auto refreshed = client.Call("project.refreshAssets", Json::object());
		REQUIRE_MESSAGE(refreshed.has_value(), (refreshed ? "" : refreshed.error().ToString()));
		(*editor)->GetAssets().WaitIdle();
		const auto savedScene = (*editor)->GetAssets().Resolve("Assets/Scenes/Host.scene");
		REQUIRE_MESSAGE(savedScene.has_value(), "saved scene was not indexed after asset refresh");
		REQUIRE(savedScene->IsValid());
		PlayStartOptions replacement;
		replacement.Paused = true;
		const Status replaced = play.StartRecording(replacement, true);
		REQUIRE_MESSAGE(replaced.has_value(), (replaced ? "" : replaced.error().ToString()));
		const auto header = play.DescribeReplayHeader();
		REQUIRE_MESSAGE(header.has_value(), (header ? "" : header.error().ToString()));
		CHECK(header->Scene.Handle == *savedScene);
		CHECK(play.GetSession()->IsPaused());
		CHECK(eval("play", "Scene.FindByName('Startup'):GetScript().StartCount") == Json(1));
		CHECK(eval("play", "Scene.FindByName('Startup'):GetScript().BeforeCursor") == Json("Normal"));
		CHECK(window->GetCursorMode() == CursorMode::Locked);
		for (const CursorMode mode : std::array{ CursorMode::Normal, CursorMode::Hidden, CursorMode::Locked })
		{
			const std::string name(CursorModeToString(mode));
			CHECK(eval("play", std::format("Input.SetCursorMode('{}'); return Input.GetCursorMode()", name)) == Json(name));
			CHECK(window->GetCursorMode() == mode);
		}
		window->InjectEvent(WindowFocusEvent{ .Focused = false });
		window->PollEvents();
		CHECK(window->GetCursorMode() == CursorMode::Normal);
		checkEnvironment("play"); // An injected focus notification must not replace the window's actual focus state.
		window->SetSize(800, 450);
		REQUIRE(Test::WaitUntil([window]()
		{
			window->PollEvents();
			return window->GetWidth() == 800 && window->GetHeight() == 450;
		}));
		checkEnvironment("edit");
		checkEnvironment("play");
		if (GlfwLibrary::GetMode() == WindowMode::Windowed)
		{
			window->Minimize();
			REQUIRE(Test::WaitUntil([window]()
			{
				window->PollEvents();
				return window->IsMinimized() && !window->IsFocused();
			}));
			CHECK(eval("play", "Application.IsFocused()") == Json(false));
			window->Restore();
			REQUIRE(Test::WaitUntil([window]()
			{
				window->PollEvents();
				return !window->IsMinimized();
			}));
			checkEnvironment("play");
		}
		CHECK(eval("play", "Input.SetCursorMode('Hidden'); return Input.GetCursorMode()") == Json("Hidden"));
		REQUIRE(play.Stop());
		CHECK(window->GetCursorMode() == CursorMode::Normal);
		REQUIRE(play.Start({ .Paused = true }));
		CHECK(window->GetCursorMode() == CursorMode::Locked);
		CHECK(eval("play", "Application.Quit(0); return 42") == Json(42));
		CHECK(window->GetCursorMode() == CursorMode::Locked);
		play.OnSafePoint();
		CHECK_FALSE(play.IsPlaying());
		CHECK(window->GetCursorMode() == CursorMode::Normal);
		window->SetEventCallback({});
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorPlayController: headless window supplies startup environment and owns the play cursor")
		{
			CheckEditorWindowHost();
		}

		TEST_CASE("EditorPlayController: native window supplies live environment and owns the play cursor")
		{
			ENGINE_CHECK_WINDOWED_CHILD("EditorPlayController: native window host target");
		}

		TEST_CASE("EditorPlayController: native window host target" * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			CheckEditorWindowHost();
		}

		TEST_CASE("EditorPlayController: script saves reload live behaviour and required modules only at a safe point")
		{
			Test::AutomationFixture fixture("EditorLiveReload");
			auto& play = fixture.GetEditor().GetPlay();
			const std::string module = "Assets/Scripts/HostModule.luau";
			const std::string behaviour = "Assets/Scripts/Host.luau";
			const std::string original = R"(
local Module = require("./HostModule")
local Host = { Fields = { Count = Field.Number(0), Reloads = Field.Number(0) } }
function Host:OnFixedUpdate() self.Count += Module.Value end
function Host:OnHotReload() self.Reloads += 1 end
return Script.Define("Host", Host)
)";
			const auto write = [&fixture](const std::string& path, const std::string& source)
			{
				CallEditorHost(fixture, "script.write", Json{ { "path", path }, { "source", source } });
			};
			const auto state = [&fixture]()
			{
				return CallEditorHost(fixture, "script.eval", Json{ { "context", "play" }, { "code", "local s = Scene.FindByName('Host'):GetScript(); return {s.Count, s.Reloads}" } })["value"];
			};
			write(module, "return {Value = 1}");
			write(behaviour, original);
			CallEditorHost(fixture, "entity.create", Json{ { "name", "Host" }, { "components", Json{ { "Script", Json{ { "Script", behaviour } } } } } });
			CallEditorHost(fixture, "play.start", Json{ { "paused", true } });
			play.GetSession()->Tick();
			CHECK(state() == Json::array({ 1, 0 }));
			std::string changed = original;
			changed.replace(changed.find("+= Module.Value"), std::string("+= Module.Value").size(), "+= Module.Value * 2");
			write(behaviour, changed);
			fixture.GetEditor().GetAssets().WaitIdle();
			play.GetSession()->Tick();
			CHECK(state() == Json::array({ 2, 0 })); // Publication cannot reenter the VM.
			play.OnSafePoint();
			play.GetSession()->Tick();
			CHECK(state() == Json::array({ 4, 1 })); // Existing instance fields survive.
			write(module, "return {Value = 10}");
			// The real frame loop publishes main-thread job completions before entering the editor safe point.
			fixture.GetEditor().GetAssets().WaitIdle();
			CHECK(state() == Json::array({ 4, 1 })); // Dependency publication alone cannot reload the live VM.
			play.OnSafePoint();
			play.GetSession()->Tick();
			CHECK(state() == Json::array({ 24, 2 }));
			play.OnSafePoint();
			CHECK(state() == Json::array({ 24, 2 })); // Dependent notifications do not reload twice.

			// A valid new dependency and an invalid dependent belong to one chain. Keep every old function until repaired.
			const uint64_t errorCursor = play.GetScriptErrors().GetCursor();
			{
				Test::ExpectLog failedImport(LogLevel::Error, "ASSET_IMPORT_FAILED");
				write(behaviour, "local =");
				write(module, "return {Value = 100}");
				fixture.GetEditor().GetAssets().WaitIdle();
				play.OnSafePoint();
			}
			play.GetSession()->Tick();
			CHECK(state() == Json::array({ 44, 2 }));
			const auto errors = play.GetScriptErrors().Read(errorCursor);
			REQUIRE_FALSE(errors.empty());
			CHECK(errors.front().Kind == ScriptErrorKind::Compile);
			CHECK(errors.front().Script == behaviour);
			CHECK(errors.front().Line == 1);
			CHECK(errors.front().Column > 0);
			write(behaviour, changed);
			fixture.GetEditor().GetAssets().WaitIdle();
			CHECK(state() == Json::array({ 44, 2 }));
			play.OnSafePoint();
			play.GetSession()->Tick();
			CHECK(state() == Json::array({ 244, 3 }));
			CallEditorHost(fixture, "play.stop");
			CallEditorHost(fixture, "play.start", Json{ { "lockstep", true } });
			write(module, "return {Value = 1000}");
			play.OnSafePoint();
			CallEditorHost(fixture, "play.step", Json{ { "ticks", 1 } });
			CHECK(state() == Json::array({ 200, 0 }));
			CallEditorHost(fixture, "play.stop");
			CallEditorHost(fixture, "play.start", Json{ { "paused", true } });
			play.GetSession()->Tick();
			CHECK(state() == Json::array({ 2000, 0 })); // Stop publishes deferred work before startup, without a live reload.
		}

		TEST_CASE("EditorPlayController: startup and update quit unwind before normal stop restores editor state")
		{
			for (const std::string_view callback : std::array{ "OnStart", "OnFixedUpdate" })
			{
				INFO(callback);
				Test::EditorTestFixture fixture("EditorQuit");
				fixture.CreateAndOpenProject();
				fixture.CreateAndOpenScene();
				auto& editor = fixture.GetEditor();
				auto& play = editor.GetPlay();
				const UUID entity = AttachStartupScript(editor, callback, "Application.Quit(7)");
				editor.SetSelection({ entity });
				REQUIRE(play.Start({}));
				if (callback == "OnFixedUpdate")
					play.OnFixedStep();
				REQUIRE(play.GetSession() != nullptr);
				CHECK(play.GetSession()->GetQuitRequest() == std::optional<int32_t>(7));
				const auto cursor = fixture.GetEngine().GetEventLog().GetNextSeq();
				play.OnSafePoint();
				CHECK_FALSE(play.IsPlaying());
				CHECK(play.GetPlayStateName() == "Edit");
				REQUIRE(editor.GetSelection().size() == 1);
				CHECK(editor.GetSelection()[0] == entity);
				play.OnSafePoint();
				const auto states = fixture.GetEngine().GetEventLog().Read(cursor, std::array{ EngineEventType::PlayStateChanged });
				REQUIRE(states.Events.size() == 1);
				CHECK(states.Events[0].Name == "Edit");
			}
		}

		TEST_CASE("EditorPlayController: eval quit returns its RPC result before stop and deterministic drivers retain ownership")
		{
			Test::AutomationFixture fixture("EditorEvalQuit");
			auto& play = fixture.GetEditor().GetPlay();
			for (const bool lockstep : std::array{ false, true })
			{
				CallEditorHost(fixture, "play.start", Json{ { "paused", true }, { "lockstep", lockstep } });
				const Json result = CallEditorHost(fixture, "script.eval", Json{ { "context", "play" }, { "code", "Application.Quit(0); return 42" } });
				CHECK(result["value"] == Json(42));
				REQUIRE(play.GetSession() != nullptr);
				play.GetSession()->SetStepping(true);
				play.OnSafePoint();
				REQUIRE(play.GetSession() != nullptr);
				play.GetSession()->SetStepping(false);
				play.OnSafePoint();
				CHECK(play.IsPlaying() == lockstep);
				if (lockstep)
					CallEditorHost(fixture, "play.stop");
			}
		}

		TEST_CASE("EditorPlayController: type-error policy permits iteration and rejects a loadable behaviour when enabled")
		{
			Test::AutomationFixture fixture("EditorBehaviourTypeGate");
			auto& play = fixture.GetEditor().GetPlay();
			static_cast<void>(AttachStartupScript(fixture.GetEditor(), "OnStart", "local bad: number = \"wrong\""));
			CallEditorHost(fixture, "project.setSettings", Json{ { "patch", Json{ { "Scripting", Json{ { "BlockPlayOnTypeErrors", false } } } } } });
			REQUIRE(play.Start({ .Paused = true }));
			REQUIRE(play.Stop());
			CallEditorHost(fixture, "project.setSettings", Json{ { "patch", Json{ { "Scripting", Json{ { "BlockPlayOnTypeErrors", true } } } } } });
			const auto rejected = play.Start({});
			REQUIRE_FALSE(rejected);
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK(rejected.error().GetLocation().File == "Assets/Startup.luau");
			CHECK(rejected.error().GetLocation().Line == 3);
			CHECK_FALSE(play.IsPlaying());
		}

		TEST_CASE("EditorPlayController: type-error policy checks required modules before changing play ownership")
		{
			Test::AutomationFixture fixture("EditorTypeGate");
			auto& editor = fixture.GetEditor();
			auto& play = editor.GetPlay();
			const Json module{ { "path", "Assets/Scripts/BadType.luau" }, { "source", "--!strict\nlocal value: number = \"wrong\"\nreturn {Value = value}" } };
			CallEditorHost(fixture, "script.write", module);
			CallEditorHost(fixture, "script.write", Json{ { "path", "Assets/Scripts/TypedHost.luau" }, { "source", "local M = require('./BadType'); return Script.Define('TypedHost', {})" } });
			CallEditorHost(fixture, "entity.create", Json{ { "name", "TypedHost" }, { "components", Json{ { "Script", Json{ { "Script", "Assets/Scripts/TypedHost.luau" } } } } } });
			CallEditorHost(fixture, "project.setSettings", Json{ { "patch", Json{ { "Scripting", Json{ { "BlockPlayOnTypeErrors", false } } } } } });
			CallEditorHost(fixture, "play.start", Json{ { "paused", true } });
			PlaySession* original = play.GetSession();
			const uint64_t serial = original->GetSerial();
			CallEditorHost(fixture, "project.setSettings", Json{ { "patch", Json{ { "Scripting", Json{ { "BlockPlayOnTypeErrors", true } } } } } });
			const auto replaced = play.StartRecording({}, true);
			REQUIRE_FALSE(replaced);
			CHECK(replaced.error().GetLocation().File == "Assets/Scripts/BadType.luau");
			CHECK(replaced.error().GetLocation().Line == 2);
			REQUIRE(play.GetSession() == original);
			CHECK(original->GetSerial() == serial);
			CHECK(original->IsPaused());
			CallEditorHost(fixture, "play.stop");
			const auto eventsBefore = fixture.GetEditorFixture().GetEngine().GetEventLog().GetNextSeq();
			const auto rejected = play.Start({});
			REQUIRE_FALSE(rejected);
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK(rejected.error().GetLocation().Column > 0);
			CHECK_FALSE(play.IsPlaying());
			CHECK(fixture.GetEditorFixture().GetEngine().GetEventLog().Read(eventsBefore, std::array{ EngineEventType::PlayStateChanged }).Events.empty());
			const auto configuration = VfsPath::Create("project", ".luaurc");
			REQUIRE(configuration);
			const std::array invalid{ std::byte{ 0xff } };
			REQUIRE(editor.WriteProjectFile(*configuration, invalid));
			const auto unavailable = play.Start({});
			REQUIRE_FALSE(unavailable);
			CHECK(unavailable.error().GetCode() == ErrorCode::Validation);
			CHECK(unavailable.error().GetMessageText().find(".luaurc") != std::string::npos);
			CHECK_FALSE(play.IsPlaying());
		}

		TEST_CASE("EditorPlayController: default play options preserve startup script-error pauses on start and replacement")
		{
			for (const std::string_view callback : std::array{ "OnCreate", "OnStart" })
			{
				for (const bool replacement : std::array{ false, true })
				{
					INFO(callback);
					INFO(replacement);
					Test::EditorTestFixture fixture("StartupErrorPause");
					fixture.CreateAndOpenProject();
					fixture.CreateAndOpenScene();
					EditorContext& editor = fixture.GetEditor();
					EditorPlayController& play = editor.GetPlay();
					if (replacement)
						REQUIRE(play.Start({}).has_value());
					const UUID entity = AttachStartupScript(editor, callback, "error(\"startup pause regression\")");
					REQUIRE(editor.GetProject().GetSettings().Scripting.PauseOnError);
					Test::ExpectLog expected(LogLevel::Error, std::format("runtime: startup pause regression\nCallback: {}", callback));
					const auto eventCursor = fixture.GetEngine().GetEventLog().GetNextSeq();
					const Status started = replacement ? play.StartRecording({}, true) : play.Start({});
					REQUIRE_MESSAGE(started.has_value(), (started ? std::string() : started.error().ToString()));
					CHECK(expected.GetMatchCount() == 1);
					REQUIRE(play.GetSession() != nullptr);
					CHECK(play.GetSession()->IsPaused());
					CHECK(play.GetPlayStateName() == "Paused");
					const auto errors = play.GetScriptErrors().Read();
					REQUIRE(errors.size() == 1);
					CHECK(errors[0].Entity == entity);
					CHECK(errors[0].Callback == callback);
					CHECK(errors[0].Message.contains("startup pause regression"));
					CHECK(errors[0].Tick == 0);
					const auto raised = fixture.GetEngine().GetEventLog().Read(eventCursor, std::array{ EngineEventType::ScriptErrorRaised });
					REQUIRE(raised.Events.size() == 1);
					CHECK(raised.Events[0].Id == entity);
					CHECK(raised.Events[0].Path == errors[0].Script);
					CHECK(raised.Events[0].Message == errors[0].Message);
					CHECK(raised.Events[0].Tick == std::optional<uint64_t>(0));
					play.OnFixedStep();
					CHECK(play.GetSession()->GetTick() == 0);
					REQUIRE(play.Stop().has_value());
				}
			}
		}

		TEST_CASE("EditorPlayController: default play options preserve startup Debug.Break on start and replacement")
		{
			for (const std::string_view callback : std::array{ "OnCreate", "OnStart" })
			{
				for (const bool replacement : std::array{ false, true })
				{
					INFO(callback);
					INFO(replacement);
					Test::EditorTestFixture fixture("StartupDebugPause");
					fixture.CreateAndOpenProject();
					fixture.CreateAndOpenScene();
					EditorContext& editor = fixture.GetEditor();
					EditorPlayController& play = editor.GetPlay();
					if (replacement)
						REQUIRE(play.Start({}).has_value());
					static_cast<void>(AttachStartupScript(editor, callback, "Debug.Break()"));
					const Status started = replacement ? play.StartRecording({}, true) : play.Start({});
					REQUIRE_MESSAGE(started.has_value(), (started ? std::string() : started.error().ToString()));
					REQUIRE(play.GetSession() != nullptr);
					CHECK(play.GetSession()->IsPaused());
					CHECK(play.GetPlayStateName() == "Paused");
					CHECK(play.GetScriptErrors().GetErrors().empty());
					play.OnFixedStep();
					CHECK(play.GetSession()->GetTick() == 0);
					REQUIRE(play.Stop().has_value());
				}
			}
		}

		TEST_CASE("EditorPlayController: disabling PauseOnError keeps a recoverable startup error running")
		{
			for (const bool replacement : std::array{ false, true })
			{
				INFO(replacement);
				Test::EditorTestFixture fixture("StartupErrorWithoutPause");
				fixture.CreateAndOpenProject();
				fixture.CreateAndOpenScene();
				EditorContext& editor = fixture.GetEditor();
				EditorPlayController& play = editor.GetPlay();
				if (replacement)
					REQUIRE(play.Start({}).has_value());
				static_cast<void>(AttachStartupScript(editor, "OnStart", "error(\"startup pause disabled\")"));
				PlayStartOptions options;
				options.PauseOnError = false;
				Test::ExpectLog expected(LogLevel::Error, "runtime: startup pause disabled\nCallback: OnStart");
				const Status started = replacement ? play.StartRecording(options, true) : play.Start(options);
				REQUIRE_MESSAGE(started.has_value(), (started ? std::string() : started.error().ToString()));
				CHECK(expected.GetMatchCount() == 1);
				REQUIRE(play.GetSession() != nullptr);
				CHECK_FALSE(play.GetSession()->IsPaused());
				CHECK(play.GetScriptErrors().GetErrors().size() == 1);
				play.OnFixedStep();
				CHECK(play.GetSession()->GetTick() == 1);
				REQUIRE(play.Stop().has_value());
			}
		}

		TEST_CASE("EditorPlayController: failed recording replacement preserves the original session and mixer")
		{
			Test::EditorTestFixture fixture("RecordingReplacement", {}, nullptr, AudioEngineSpecification{});
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			REQUIRE(fixture.GetEditor().GetAssets().Refresh().has_value());
			auto& play = fixture.GetEditor().GetPlay();
			AudioEngine* audio = fixture.GetEngine().GetAudioEngine();
			REQUIRE(audio != nullptr);
			REQUIRE(audio->SetGroupVolume(AudioGroup::Music, 0.25f));
			REQUIRE(play.Start({ .Lockstep = true, .LockstepOwner = 41 }));
			PlaySession* original = play.GetSession();
			REQUIRE(original != nullptr);
			original->Tick();
			const uint64_t hash = original->ComputeStateHash();
			REQUIRE(audio->SetGroupVolume(AudioGroup::Music, 0.5f));
			PlayStartOptions invalid;
			invalid.Lockstep = true;
			invalid.LockstepOwner = 41;
			invalid.Parameters = VariantValue(Json::array());
			CHECK_FALSE(play.StartRecording(invalid, true));
			CHECK(play.GetSession() == original);
			CHECK(original->ComputeStateHash() == hash);
			CHECK(original->IsLockstep());
			CHECK(original->GetLockstepOwner() == 41);
			CHECK_FALSE(original->IsPaused());
			CHECK(audio->GetGroupVolume(AudioGroup::Music) == 0.5f);
			PlayStartOptions valid;
			valid.Lockstep = true;
			valid.LockstepOwner = 41;
			REQUIRE(play.StartRecording(valid, true));
			CHECK(play.GetSession() != original);
			CHECK(play.GetSession()->GetTick() == 0);
			CHECK_FALSE(play.GetSession()->IsPaused());
			REQUIRE(play.Stop());
			CHECK(audio->GetGroupVolume(AudioGroup::Music) == 0.25f);
		}

		TEST_CASE("EditorPlayController: before-play failure prevents the copy and a retry observes completed preparation")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			uint32_t calls = 0;
			editor.SetLifecycleCallbacks({ .BeforePlay = [&calls, &editor]() -> Status
			{
				if (++calls == 1)
					return MakeError(ErrorCode::Io, "cannot save before play");
				static_cast<void>(editor.GetScene().CreateEntity("Prepared"));
				return {};
			} });
			const auto failed = editor.GetPlay().Start({});
			REQUIRE_FALSE(failed);
			CHECK(failed.error().GetCode() == ErrorCode::Io);
			CHECK_FALSE(editor.GetPlay().IsPlaying());
			CHECK(calls == 1);
			REQUIRE(editor.GetPlay().Start({}));
			CHECK(calls == 2);
			CHECK(editor.GetPlay().GetSession()->GetScene().FindEntityByPath("/Prepared").IsValid());
			REQUIRE(editor.GetPlay().Stop());
			editor.SetLifecycleCallbacks({});
		}

		TEST_CASE("EditorPlayController: play then stop leaves the edit scene, its revision and its history untouched")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			static_cast<void>(editor.GetScene().CreateEntity("Ball"));
			const Result<std::string> before = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(before.has_value());
			const uint64_t revision = editor.GetRevision();
			const size_t historySize = editor.GetHistory().GetUndoCount();

			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true }).has_value());
			REQUIRE(play.IsPlaying());
			CHECK(play.GetPlayStateName() == "Play");
			for (int tick = 0; tick < 5; ++tick)
				play.GetSession()->Tick();
			CHECK(play.GetTick() == std::optional<uint64_t>(5));
			REQUIRE(play.Stop().has_value());
			CHECK_FALSE(play.IsPlaying());
			CHECK(play.GetPlayStateName() == "Edit");

			const Result<std::string> after = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(after.has_value());
			CHECK(*after == *before);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == historySize);
		}

		TEST_CASE("EditorPlayController: a second start and a stop in Edit mode are InvalidState")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			const Status notPlaying = play.Stop();
			REQUIRE_FALSE(notPlaying.has_value());
			CHECK(notPlaying.error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			const Status again = play.Start(PlayStartOptions{});
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorPlayController: a disconnect of the lockstep owner releases lockstep and pauses play")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true, .LockstepOwner = 3 }).has_value());
			play.OnClientDisconnected(2); // not the owner
			CHECK(play.GetSession()->IsLockstep());
			play.OnClientDisconnected(3);
			CHECK_FALSE(play.GetSession()->IsLockstep());
			CHECK(play.GetSession()->IsPaused());
			CHECK(play.GetPlayStateName() == "Paused");
		}

		TEST_CASE("EditorPlayController: the editor's loop runs at the project's FixedHz while playing")
		{
			// EditorApp applies GetFrameLoopConfig at its safe point (FrameLoop::SetLoopConfig), so a 30 Hz project plays at
			// 30 ticks per wall-clock second and its frame deltas are 1/30 s (Docs/Decisions/0012-m7-decisions.md decision 3).
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Result<Json> patch = JsonReader::Parse(R"({"Simulation":{"FixedHz":30,"MaxStepsPerFrame":3}})");
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Set Project Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());

			EditorPlayController& play = editor.GetPlay();
			CHECK_FALSE(play.GetFrameLoopConfig().has_value()); // Edit mode: the editor's own loop
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			const std::optional<FrameLoopConfig> config = play.GetFrameLoopConfig();
			REQUIRE(config.has_value());
			CHECK(config->FixedHz == 30);
			CHECK(config->MaxStepsPerFrame == 3);
			CHECK(config->MaxFrameDelta == FrameLoopConfig{}.MaxFrameDelta);
			CHECK(play.GetSession()->GetFixedDelta() == doctest::Approx(1.0 / 30.0));
			REQUIRE(play.Stop().has_value());
			CHECK_FALSE(play.GetFrameLoopConfig().has_value());
		}

		TEST_CASE("EditorPlayController: the frame hooks drive a running session; paused and lockstep sessions wait")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			const FrameTime frame{ .DeltaTime = 1.0 / 60.0, .UnscaledDeltaTime = 1.0 / 60.0, .Alpha = 1.0, .FrameIndex = 0 };
			// No effect in Edit mode.
			play.OnFixedStep();
			play.OnUpdate(frame);
			CHECK(play.GetFrameTimeScale() == 1.0);
			CHECK_FALSE(play.IsFrameThrottleSuspended());
			CHECK(play.GetPlayStateName() == "Edit");
			CHECK_FALSE(play.GetTick().has_value());

			REQUIRE(play.Start(PlayStartOptions{ .Mode = PlayMode::Simulate, .TimeScale = 0.5 }).has_value());
			CHECK(play.GetPlayStateName() == "Simulate");
			CHECK(play.GetFrameTimeScale() == 0.5);
			play.OnFixedStep();
			play.OnFixedStep();
			play.OnUpdate(frame);
			CHECK(play.GetTick() == std::optional<uint64_t>(2));
			play.GetSession()->SetPaused(true);
			CHECK(play.GetPlayStateName() == "Paused");
			play.OnFixedStep();
			CHECK(play.GetTick() == std::optional<uint64_t>(2));
			play.GetSession()->SetStepping(true);
			CHECK(play.IsFrameThrottleSuspended());
			REQUIRE(play.Stop().has_value());

			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true, .LockstepOwner = 4 }).has_value());
			play.OnFixedStep();
			play.OnUpdate(frame);
			CHECK(play.GetTick() == std::optional<uint64_t>(0));
			CHECK(play.GetSession()->GetLockstepOwner() == 4);
		}

		TEST_CASE("EditorPlayController: Start checks its options and the edit state, and changes nothing when it fails")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			const Status noScene = play.Start(PlayStartOptions{});
			REQUIRE_FALSE(noScene.has_value());
			CHECK(noScene.error().GetCode() == ErrorCode::InvalidState);

			fixture.CreateAndOpenScene();
			const Status tooFast = play.Start(PlayStartOptions{ .TimeScale = PlaySession::MaxTimeScale * 2.0 });
			REQUIRE_FALSE(tooFast.has_value());
			CHECK(tooFast.error().GetCode() == ErrorCode::InvalidArgument);
			const Status outside = play.Start(PlayStartOptions{ .ScenePath = "../Elsewhere.scene" });
			REQUIRE_FALSE(outside.has_value());
			CHECK(outside.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(play.IsPlaying());
			CHECK(play.GetPlayStateName() == "Edit");
		}

		TEST_CASE("EditorPlayController: the session seed is the project's Seed xor the scene's, unless one is given")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const Result<Json> patch = JsonReader::Parse(R"({"Simulation":{"Seed":1337}})");
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Set Project Settings");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			editor.GetScene().SetSeed(42);

			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			CHECK(play.GetSession()->GetSeed() == PlaySession::ComputeSessionSeed(1337, 42));
			REQUIRE(play.Stop().has_value());
			REQUIRE(play.Start(PlayStartOptions{ .Seed = 7 }).has_value());
			CHECK(play.GetSession()->GetSeed() == 7);
		}

		TEST_CASE("EditorPlayController: a lockstep session defers asset reloads until it stops")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			CHECK_FALSE(editor.GetAssets().AreReloadsDeferred()); // ordinary play reloads (see the next test case)
			REQUIRE(play.Stop().has_value());

			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true, .LockstepOwner = 3 }).has_value());
			CHECK(editor.GetAssets().AreReloadsDeferred());
			// A released lockstep keeps the deferral until the session ends.
			play.OnClientDisconnected(3);
			CHECK_FALSE(play.GetSession()->IsLockstep());
			CHECK(editor.GetAssets().AreReloadsDeferred());
			REQUIRE(play.Stop().has_value());
			CHECK_FALSE(editor.GetAssets().AreReloadsDeferred());
		}

		TEST_CASE("EditorPlayController: a reload during ordinary play marks the session modified, whatever started it")
		{
			// Â§7.5 race rule 4: the editor's own write of an asset (asset.setProperties, asset.import) reloads it in the running
			// game as an external change does, and both mark the session modified; a lockstep session defers both.
			Test::EditorTestFixture fixture("PlayReloadModified");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			EditorAssetManager& assets = editor.GetAssets();
			const Result<VfsPath> material = VfsPath::Parse("project://Assets/Materials/Red.material");
			REQUIRE(material.has_value());
			const auto materialText = [](double roughness)
			{
				return std::format(R"({{"Format": "Material", "Version": 1, "Roughness": {}}})", roughness);
			};
			const auto writeFromEditor = [&editor, &material](const std::string& text)
			{
				REQUIRE(editor.WriteProjectFile(*material, std::as_bytes(std::span(text.data(), text.size()))).has_value());
			};
			REQUIRE(editor.GetVfs().CreateDirectories(material->GetParent()).has_value());
			writeFromEditor(materialText(0.1));
			REQUIRE(assets.Refresh().has_value());
			const AssetHandle handle = assets.Resolve("Assets/Materials/Red.material").value_or(AssetHandle());
			REQUIRE(assets.Load(handle).has_value());
			const uint64_t version = assets.GetVersion(handle);

			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			CHECK_FALSE(play.GetSession()->IsModified());
			// The editor's write reimports the material on a job; WaitIdle publishes it.
			writeFromEditor(materialText(0.2));
			assets.WaitIdle();
			REQUIRE(assets.GetVersion(handle) == version + 1);
			CHECK(play.GetSession()->IsModified());
			REQUIRE(play.Stop().has_value());

			// An external change, found by a refresh.
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			const std::string external = materialText(0.3);
			REQUIRE(FileSystem::WriteFileAtomic(editor.GetProject().GetRoot() / "Assets/Materials/Red.material",
				std::as_bytes(std::span(external.data(), external.size())))
					.has_value());
			REQUIRE(assets.Refresh().has_value());
			assets.WaitIdle();
			REQUIRE(assets.GetVersion(handle) == version + 2);
			CHECK(play.GetSession()->IsModified());
			REQUIRE(play.Stop().has_value());

			// A lockstep session defers the reload until it stops, so it is never marked.
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true }).has_value());
			writeFromEditor(materialText(0.4));
			assets.WaitIdle();
			CHECK(assets.GetVersion(handle) == version + 2);
			CHECK_FALSE(play.GetSession()->IsModified());
			REQUIRE(play.Stop().has_value());
			assets.WaitIdle();
			CHECK(assets.GetVersion(handle) == version + 3);
		}

		TEST_CASE("EditorPlayController: every session gets a serial of its own")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorPlayController& play = fixture.GetEditor().GetPlay();
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			const uint64_t first = play.GetSession()->GetSerial();
			CHECK(first != 0);
			REQUIRE(play.Stop().has_value());
			// A start that fails uses up no serial; the next session's differs from every earlier one.
			CHECK_FALSE(play.Start(PlayStartOptions{ .TimeScale = -1.0 }).has_value());
			REQUIRE(play.Start(PlayStartOptions{}).has_value());
			CHECK(play.GetSession()->GetSerial() == first + 1);
		}

		TEST_CASE("EditorPlayController: Stop restores the selection by UUID")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const UUID ball = editor.GetScene().CreateEntity("Ball").GetUUID();
			editor.SetSelection({ ball });

			EditorPlayController& play = editor.GetPlay();
			REQUIRE(play.Start(PlayStartOptions{ .Lockstep = true }).has_value());
			// The play scene copies the ids, so the selection names the same entities in it; a runtime spawn can join it.
			CHECK(play.GetSession()->GetScene().FindEntityByID(ball).IsValid());
			Result<Entity> spawned = play.GetSession()->CreateEntity("Spawned");
			REQUIRE(spawned.has_value());
			editor.SetSelection({ ball, spawned->GetUUID() });
			CHECK(editor.GetSelection().size() == 2);

			REQUIRE(play.Stop().has_value());
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection().front() == ball);
		}

		TEST_CASE("EditorPlayController: closing the project stops its session")
		{
			Test::EditorTestFixture fixture;
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }).has_value());
			REQUIRE(editor.CloseProject());
			CHECK_FALSE(editor.GetPlay().IsPlaying());
			CHECK_FALSE(editor.GetAssets().AreReloadsDeferred());
		}
	}

}
