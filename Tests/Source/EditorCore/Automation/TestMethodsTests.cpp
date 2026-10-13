#include "TestsPCH.h"
#include "EditorCore/Automation/TestMethods.h"

#include "EditorCore/Automation/ScreenshotMethods.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <utility>

namespace Engine {

	namespace {

		template<typename TClient>
		Json CallTestMethod(TClient& fixture, std::string_view method, const Json& params)
		{
			auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			// Coverage can exceed the protocol's inline limit. Inspect the complete report that a real client reads.
			if (JsonReader(*result).ReadMember<bool>("truncated").value_or(false))
			{
				const auto path = JsonReader(*result).ReadMember<std::string>("path");
				REQUIRE(path);
				const auto text = FileSystem::ReadText(FileSystem::PathFromUtf8(*path));
				REQUIRE_MESSAGE(text, (text ? "" : text.error().ToString()));
				result = JsonReader::Parse(*text);
				REQUIRE_MESSAGE(result, (result ? "" : result.error().ToString()));
			}
			return std::move(*result);
		}

		// The result of a test.run response that arrived without waiting (Submit), read from its offloaded report when it
		// was too large to send inline, as CallTestMethod does.
		Json ReadRunResult(const Json& response)
		{
			REQUIRE_MESSAGE(response.contains("result"), response.dump());
			const Json& result = response["result"];
			if (!JsonReader(result).ReadMember<bool>("truncated").value_or(false))
				return result;
			const auto path = JsonReader(result).ReadMember<std::string>("path");
			REQUIRE(path);
			const auto text = FileSystem::ReadText(FileSystem::PathFromUtf8(*path));
			REQUIRE_MESSAGE(text, (text ? "" : text.error().ToString()));
			auto parsed = JsonReader::Parse(*text);
			REQUIRE_MESSAGE(parsed, (parsed ? "" : parsed.error().ToString()));
			return std::move(*parsed);
		}

		template<typename TClient>
		void WriteSuite(TClient& fixture, std::string_view source)
		{
			CallTestMethod(fixture, "script.write", Json{ { "path", "Assets/Tests/Contract.test.luau" }, { "source", source } });
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Contract.test.luau" }, { "Scene", "" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } });
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("TestMethods: disconnect restores the suspended session and edit selection without another poll")
		{
			Test::EditorTestFixture fixture("TestRunLease");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), std::move(specification));
			WriteSuite(client, "return Test.Suite('Lease', function() Test.Case('long', function() Test.WaitTicks(1000) end) end)");
			CallTestMethod(client, "entity.create", Json{ { "name", "Selected" } });
			CallTestMethod(client, "play.start", Json::object());
			auto& editor = fixture.GetEditor();
			PlaySession* original = editor.GetPlay().GetSession();
			REQUIRE(original != nullptr);
			const UUID selected = editor.GetScene().FindEntityByPath("/Selected").GetUUID();
			REQUIRE(editor.SetSelection({ selected }, SceneTarget::Edit));
			const auto before = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(before.has_value());
			const uint64_t revision = editor.GetRevision();
			const auto history = editor.GetHistory().GetUndoCount();
			const uint64_t tick = original->GetTick();
			static_cast<void>(client.Submit("test.run", Json::object()));
			client.GetServer().Pump();
			REQUIRE(editor.GetPlay().IsTestRunActive());
			CHECK(editor.GetPlay().IsLiveInputSuppressed());
			CHECK(original->IsPaused());
			// Each client is serialized behind its pending operation. A second client exercises admission while the
			// first still owns the run; sending on the first would wait until its run had finished.
			AutomationServer& server = client.GetServer();
			const ClientId observer = server.ConnectInProcess("observer");
			REQUIRE(observer != NoClient);
			const std::array<std::pair<std::string_view, Json>, 3> requests = { {
				{ "play.stop", Json::object() },
				{ "entity.create", Json{ { "name", "Rejected" } } },
				{ "input.inject", Json{ { "events", Json::array() } } },
			} };
			for (const auto& [method, params] : requests)
			{
				server.SubmitInProcess(observer, RpcRequest{ .Id = Json(1), .IsNotification = false, .Method = std::string(method), .Params = params, .TranscriptLine = std::nullopt });
				server.Pump();
				const auto responses = server.TakeInProcessResponses(observer);
				REQUIRE(responses.size() == 1);
				CHECK(responses.front()["error"]["data"]["errorCode"] == Json("InvalidState"));
				CHECK(responses.front()["error"]["message"].dump().contains("a test run owns the editor"));
				REQUIRE(editor.GetPlay().IsTestRunActive());
			}
			// The observation subset UI actions share (EditorContext::CheckTestRunAdmission) stays available.
			server.SubmitInProcess(observer, RpcRequest{ .Id = Json(2), .IsNotification = false, .Method = "scene.tree", .Params = Json::object(), .TranscriptLine = std::nullopt });
			server.Pump();
			const auto reads = server.TakeInProcessResponses(observer);
			REQUIRE(reads.size() == 1);
			CHECK(reads.front().contains("result"));
			server.DisconnectInProcess(observer);
			editor.GetPlay().OnFixedStep();
			CHECK(original->GetTick() == tick);
			client.GetServer().DisconnectInProcess(client.GetClient());
			CHECK_FALSE(editor.GetPlay().IsTestRunActive());
			CHECK_FALSE(editor.GetPlay().IsLiveInputSuppressed());
			CHECK(editor.GetPlay().GetSession() == original);
			CHECK_FALSE(original->IsPaused());
			CHECK_FALSE(original->IsLockstep());
			CHECK(original->GetTick() == tick);
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
			REQUIRE(editor.GetSelection().size() == 1);
			CHECK(editor.GetSelection()[0] == selected);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetUndoCount() == history);
			const auto after = SceneSerializer::SaveToString(editor.GetScene());
			REQUIRE(after.has_value());
			CHECK(*after == *before);
		}

		TEST_CASE("TestMethods: completed runs restore the original lockstep owner and paused state")
		{
			Test::AutomationFixture fixture("TestRunRestore");
			WriteSuite(fixture, "return Test.Suite('Restore', function() Test.Case('short', function() Test.WaitTicks(2) end) end)");
			CallTestMethod(fixture, "play.start", Json{ { "lockstep", true }, { "paused", true } });
			PlaySession* original = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(original != nullptr);
			const ClientId owner = original->GetLockstepOwner();
			const auto result = CallTestMethod(fixture, "test.run", Json::object());
			CHECK(result["passed"] == Json(true));
			CHECK(fixture.GetEditor().GetPlay().GetSession() == original);
			CHECK(original->IsPaused());
			CHECK(original->IsLockstep());
			CHECK(original->GetLockstepOwner() == owner);
			CHECK(original->GetTick() == 0);
			CHECK_FALSE(fixture.GetEditor().GetPlay().IsTestRunActive());
		}

		TEST_CASE("TestMethods: failed discovery leaves the active session and ownership intact")
		{
			Test::AutomationFixture fixture("TestPreflightFailure");
			CallTestMethod(fixture, "play.start", Json{ { "lockstep", true } });
			PlaySession* original = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(original != nullptr);
			const ClientId owner = original->GetLockstepOwner();
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Missing.test.luau" } } }) } } } } } });
			CHECK(fixture.Request("test.run", Json::object()).contains("error"));
			CHECK(fixture.GetEditor().GetPlay().GetSession() == original);
			CHECK(original->IsLockstep());
			CHECK(original->GetLockstepOwner() == owner);
			CHECK(original->GetTick() == 0);
			CHECK_FALSE(fixture.GetEditor().GetPlay().IsTestRunActive());
		}
		TEST_CASE("TestMethods: list collects case names and locations without executing bodies")
		{
			Test::AutomationFixture fixture("TestList");
			WriteSuite(fixture, "return Test.Suite('Inventory', function()\n"
								"Test.Case('first', function() error('must not execute while listing') end)\n"
								"Test.Case('second', function() Test.Expect(true) end)\nend)");
			const Json listed = CallTestMethod(fixture, "test.list", Json::object());
			REQUIRE(listed["suites"].size() == 1);
			const Json& suite = listed["suites"][0];
			CHECK(suite["name"] == Json("Inventory"));
			REQUIRE(suite["cases"].size() == 2);
			CHECK(suite["cases"][0]["name"] == Json("first"));
			CHECK(suite["cases"][0]["file"] == Json("Assets/Tests/Contract.test.luau"));
			CHECK(suite["cases"][0]["line"] == Json(2));
			CHECK(suite["cases"][1]["name"] == Json("second"));
			CHECK(listed["replays"].empty());
			CHECK(CallTestMethod(fixture, "script.errors", Json::object())["errors"].empty());
		}

		TEST_CASE("TestMethods: run filters suite and case names case-insensitively")
		{
			Test::AutomationFixture fixture("TestRunFilter");
			WriteSuite(fixture, "return Test.Suite('Contract', function()\n"
								"Test.Case('selected', function() Test.Expect(true) end)\n"
								"Test.Case('excluded', function() Test.Fail('not selected') end)\nend)");
			const Json result = CallTestMethod(fixture, "test.run", Json{ { "filter", "cOnTrAcT/SELECTED" } });
			CHECK(result["passed"] == Json(true));
			REQUIRE(result["cases"].size() == 1);
			CHECK(result["cases"][0]["suite"] == Json("Contract"));
			CHECK(result["cases"][0]["case"] == Json("selected"));
			CHECK(result["cases"][0]["status"] == Json("passed"));
			CHECK(result.contains("coverage"));
			CHECK_FALSE(result["jsonPath"].empty());
			CHECK_FALSE(result["junitPath"].empty());
		}

		// ADR 0022 decisions 3 and 5 (stream A): a coverage run of the editor host observes loads through the editor's asset
		// manager (IFeatureTestHost::GetAssetManager), so the suite script's own load counts for gate 9, and it reports the
		// six evaluated per-mode gates.
		TEST_CASE("TestMethods: a coverage run counts the formats the editor's asset manager loads")
		{
			Test::AutomationFixture fixture("TestRunFormats");
			WriteSuite(fixture, "return Test.Suite('Contract', function()\nTest.Case('passes', function() Test.Expect(true) end)\nend)");
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "CoverageGates", true } } } } } });
			const Json result = CallTestMethod(fixture, "test.run", Json::object());
			REQUIRE(result["coverage"]["gates"].size() == 6);
			const Json& counters = result["coverage"]["counters"];
			const auto script = std::ranges::find_if(counters, [](const Json& counter)
			{
				return counter["id"] == Json("format:Script/.luau");
			});
			REQUIRE(script != counters.end());
			CHECK((*script)["count"] != Json(0));
			// The script API counters carry the gate 2 vocabulary of CoverageGates.h (ADR 0022 decision 2).
			const auto expect = std::ranges::find_if(counters, [](const Json& counter)
			{
				return counter["id"] == Json("api:Test.Expect");
			});
			REQUIRE(expect != counters.end());
			CHECK((*expect)["kind"] == Json("function"));
			CHECK((*expect)["count"] != Json(0));
			// Most of the registry is uncovered by one suite, so the gates evaluate and fail the run.
			CHECK(std::ranges::all_of(result["coverage"]["gates"], [](const Json& gate)
			{
				return gate["evaluated"] == Json(true);
			}));
			CHECK(result["passed"] == Json(false));
		}

		TEST_CASE("TestMethods: failed expectations return structured locations and continue the case")
		{
			Test::AutomationFixture fixture("TestRunLocations");
			WriteSuite(fixture, "return Test.Suite('Failures', function()\n"
								"Test.Case('both failures', function()\n"
								"Test.Expect(false, 'first failure')\n"
								"Test.Expect(false, 'second failure')\nend)\nend)");
			const Json result = CallTestMethod(fixture, "test.run", Json::object());
			CHECK(result["passed"] == Json(false));
			REQUIRE(result["cases"].size() == 1);
			const Json& row = result["cases"][0];
			CHECK(row["status"] == Json("failed"));
			CHECK(row["message"] == Json("first failure"));
			CHECK(row["file"] == Json("Assets/Tests/Contract.test.luau"));
			CHECK(row["line"] == Json(3));
			REQUIRE(row["failures"].size() == 2);
			CHECK(row["failures"][1]["line"] == Json(4));
		}

		TEST_CASE("TestMethods: case timeout reports its wait location and permits the next case")
		{
			Test::AutomationFixture fixture("TestRunTimeout");
			WriteSuite(fixture, "return Test.Suite('Timeouts', function()\n"
								"Test.Case('times out', function()\nTest.WaitTicks(100)\nend, { TimeoutTicks = 2 })\n"
								"Test.Case('continues', function() Test.Expect(true) end)\nend)");
			const Json result = CallTestMethod(fixture, "test.run", Json::object());
			CHECK(result["passed"] == Json(false));
			REQUIRE(result["cases"].size() == 2);
			CHECK(result["cases"][0]["status"] == Json("timeout"));
			CHECK(result["cases"][0]["line"] == Json(3));
			CHECK(result["cases"][1]["status"] == Json("passed"));
		}

		TEST_CASE("TestMethods: recording requires exactly one selected suite before writing any replay")
		{
			Test::AutomationFixture fixture("TestRecordSelection");
			WriteSuite(fixture, "return Test.Suite('Only', function() Test.Case('pass', function() end) end)");
			const Json response = fixture.Request("test.run", Json{ { "filter", "does not match" }, { "record", "Assets/Tests/Replays/Missing.replay" } });
			CHECK(response["error"]["data"]["errorCode"] == Json("Validation"));
			CHECK_FALSE(fixture.Call("asset.info", Json{ { "asset", "Assets/Tests/Replays/Missing.replay" } }).has_value());
		}

		TEST_CASE("TestMethods: a recorded suite writes a replay that verifies without its test body")
		{
			Test::AutomationFixture fixture("TestRecordReplay");
			WriteSuite(fixture, "return Test.Suite('Record', function()\n"
								"Test.Case('wait', function() Test.WaitTicks(3) end)\nend)");
			// A replay has an authored scene identity; bind this recording suite to the fixture's saved scene.
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Contract.test.luau" }, { "Scene", "Assets/Scenes/Main.scene" } } }) } } } } } });
			const std::string path = "Assets/Tests/Replays/Recorded.replay";
			const Json result = CallTestMethod(fixture, "test.run", Json{ { "filter", "Record" }, { "record", path } });
			CHECK(result["passed"] == Json(true));
			CHECK(result["recordingPath"] == Json(path));
			const Json replay = CallTestMethod(fixture, "input.replay", Json{ { "path", path }, { "verify", true }, { "strictHash", true } });
			REQUIRE(result["suites"].size() == 1);
			CHECK(replay["stateHash"] == result["suites"][0]["finalStateHash"]);
			// The header names the run mode that recorded it, never the build configuration (§9.1, ADR 0022 "Integration"),
			// so a Debug and a Release editor record the same bytes.
			const auto text = FileSystem::ReadText(fixture.GetEditorFixture().GetProjectRoot() / "Assets/Tests/Replays/Recorded.replay");
			REQUIRE_MESSAGE(text.has_value(), (text ? "" : text.error().ToString()));
			const auto document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			CHECK((*document)["Config"] == Json("Editor"));
		}

		TEST_CASE("TestMethods: the editor host's suite sessions own simulation audio time")
		{
			// A ScriptedClock suite runs without lockstep, so only PlaySessionSpecification::OwnsAudioTime gives its session the
			// engine's simulation time; Test.CaptureAudio and the runner's own check both require it (ADR 0015 decision 5).
			Test::AutomationFixture fixture("TestRunAudioTime", true, AudioEngineSpecification{ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic });
			CallTestMethod(fixture, "script.write", Json{ { "path", "Assets/Tests/Audio.test.luau" }, { "source", "return Test.Suite('Audio', function() Test.Case('capture', function() local levels = Test.CaptureAudio(2); Test.Expect(levels.Peak == 0) end) end)" } });
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Audio.test.luau" }, { "Scene", "" }, { "Clock", Json::array({ 5.0 / 60.0 }) }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } });
			AudioEngine* audio = fixture.GetEditor().GetEngine().GetAudioEngine();
			REQUIRE(audio != nullptr);
			const uint64_t pulled = audio->GetStats().PulledFrames;
			const Json result = CallTestMethod(fixture, "test.run", Json::object());
			CHECK(result["passed"] == Json(true));
			REQUIRE(result["cases"].size() == 1);
			CHECK(result["cases"][0]["status"] == Json("passed"));
			CHECK(audio->GetStats().PulledFrames > pulled);
			CHECK_FALSE(audio->IsSimulationTimeOwned());
		}

		TEST_CASE("TestMethods: a script test screenshot renders with the project's rendering quality")
		{
			Test::EditorTestFixture fixture("TestRunScreenshotQuality");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			std::optional<RenderSnapshot> captured;
			auto specification = Test::MakeTestServerSpecification();
			specification.RendererName = "vulkan";
			specification.Screenshots.View = [&captured](const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request) -> Result<Image>
			{
				captured = snapshot;
				Image image;
				image.Width = request.Width;
				image.Height = request.Height;
				image.Pixels.resize(static_cast<size_t>(request.Width) * request.Height * 4, std::byte{ 0x80 });
				return image;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), std::move(specification));
			CallTestMethod(client, "project.setSettings", Json{ { "patch", Json{ { "Rendering", Json{ { "ShadowMapSize", 512 }, { "SsaoHalfResolution", true } } } } } });
			WriteSuite(client, "return Test.Suite('Shot', function() Test.Case('capture', function() Test.Screenshot('Shot') end) end)");
			const Json result = CallTestMethod(client, "test.run", Json::object());
			CHECK(result["passed"] == Json(true));
			REQUIRE(captured.has_value());
			CHECK(captured->Quality.ShadowMapSize == 512);
			CHECK(captured->Quality.SsaoHalfResolution);
		}

		// ADR 0022 decision 15 and "Integration": Test.ReloadScript takes the play-mode reload's type gate
		// (EditorPlayController::CheckReloadTypes) between its reimport and the reload, so a script whose new source has type
		// errors keeps its running code, publishes a `type` error and fails the call in the case with a located Script error.
		TEST_CASE("TestMethods: Test.ReloadScript refuses new source with type errors and keeps the running code")
		{
			Test::EditorTestFixture setup("TestReloadTypeGate");
			setup.CreateAndOpenProject();
			setup.CreateAndOpenScene();
			EditorContext& editor = setup.GetEditor();
			// One runner step per poll, so the run is observable between its ticks.
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient fixture(editor, std::move(specification));
			const std::string path = "Assets/Scripts/Typed.luau";
			const std::string clean = "--!strict\nlocal Typed = {}\nfunction Typed:Value(): number\n\treturn 1\nend\nreturn Script.Define(\"Typed\", Typed)\n";
			std::string broken = clean;
			broken.replace(broken.find("return 1"), std::string_view("return 1").size(), "return \"two\"");
			CallTestMethod(fixture, "script.write", Json{ { "path", path }, { "source", clean } });
			CallTestMethod(fixture, "entity.create", Json{ { "name", "Typed" }, { "components", Json{ { "Script", Json{ { "Script", path } } } } } });
			CallTestMethod(fixture, "scene.save", Json::object());
			CallTestMethod(fixture, "script.write", Json{ { "path", "Assets/Tests/Reload.test.luau" }, { "source", "return Test.Suite(\"Reload\", function()\n"
																												   "\tTest.Case(\"refused\", function()\n"
																												   "\t\tTest.WaitTicks(400)\n"
																												   "\t\tlocal entity = Scene.FindByName(\"Typed\")\n"
																												   "\t\tlocal script = Assets.Load(\"Assets/Scripts/Typed.luau\")\n"
																												   "\t\tassert(entity and script, \"the scripted entity and its script exist\")\n"
																												   "\t\tlocal typed = entity:GetScript()\n"
																												   "\t\tlocal ok, message = pcall(Test.ReloadScript, script)\n"
																												   "\t\tTest.Expect(not ok, \"the reload is refused\")\n"
																												   "\t\tTest.Expect(string.find(tostring(message), \"type errors block the reload\", 1, true) ~= nil, tostring(message))\n"
																												   "\t\tTest.ExpectEqual(typed:Value(), 1, \"the running code is kept\")\n"
																												   "\t\tTest.ReloadScript(script)\n"
																												   "\tend)\n"
																												   "end)\n" } });
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Reload.test.luau" }, { "Scene", "Assets/Scenes/Main.scene" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } });
			editor.GetAssets().WaitIdle();
			const uint64_t cursor = editor.GetPlay().GetScriptErrors().GetCursor();
			Test::ExpectLog refused(LogLevel::Error, "type errors block the reload, so the running code is kept");
			Test::AutomationTestClient& client = fixture;
			static_cast<void>(client.Submit("test.run", Json::object()));
			// While the case waits, its script's source changes on disk; Test.ReloadScript's reimport then sees it.
			REQUIRE(Test::WaitUntil([&client, &editor]()
			{
				client.GetServer().Pump();
				const PlaySession* session = editor.GetPlay().GetSession();
				return editor.GetPlay().IsTestRunActive() && session != nullptr && session->GetScripts() != nullptr && session->GetTick() > 0;
			}));
			const std::filesystem::path file = setup.GetProjectRoot() / "Assets" / "Scripts" / "Typed.luau";
			REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(broken.data(), broken.size())), { .KeepBackup = false }).has_value());
			std::optional<Json> run;
			REQUIRE(Test::WaitUntil([&client, &run]()
			{
				client.GetServer().Pump();
				for (Json& response : client.GetServer().TakeInProcessResponses(client.GetClient()))
					run = std::move(response);
				return run.has_value();
			}));
			const Json result = ReadRunResult(*run);
			CHECK(result["passed"] == Json(false));
			REQUIRE(result["cases"].size() == 1);
			const Json& row = result["cases"][0];
			INFO(row.dump());
			// The pcall checks passed (no failed expectation); the unprotected call failed the case with the gate's located error.
			CHECK(row["status"] == Json("error"));
			CHECK(JsonReader(row).ReadMember<std::string>("message").value_or("").contains("Assets/Scripts/Typed.luau:4:9: type errors block the reload"));
			REQUIRE(row["failures"].size() == 1);
			CHECK(row["failures"][0]["message"] == row["message"]);
			const auto errors = editor.GetPlay().GetScriptErrors().Read(cursor);
			const auto typeError = std::ranges::find_if(errors, [](const ScriptError& error)
			{
				return error.Kind == ScriptErrorKind::Type;
			});
			REQUIRE(typeError != errors.end());
			CHECK(typeError->Callback == "Reload");
			CHECK(typeError->Script == path);
			CHECK(typeError->Line == 4);
			CHECK_FALSE(editor.GetPlay().IsTestRunActive());
		}

		// ADR 0022 decision 12: the editor host gives a suite's session the project's scripting settings with the suite's
		// Overrides applied (ApplyTestSuiteOverrides), so its live VM enforces the overridden budget and memory limit.
		TEST_CASE("TestMethods: a suite's overrides set the budget and memory limit of its session")
		{
			Test::EditorTestFixture setup("TestRunOverrides");
			setup.CreateAndOpenProject();
			setup.CreateAndOpenScene();
			EditorContext& editor = setup.GetEditor();
			// One runner step per poll, so the run is observable between its ticks.
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = Test::MakeTestServerSpecification();
			specification.WallClock = [&now]()
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient fixture(editor, std::move(specification));
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Scripting", Json{ { "CallbackBudgetMs", 700 }, { "MemoryLimitMB", 96 } } } } } });
			CallTestMethod(fixture, "script.write", Json{ { "path", "Assets/Tests/Limits.test.luau" }, { "source", "return Test.Suite(\"Limits\", function() Test.Case(\"waits\", function() Test.WaitTicks(400) end) end)" } });
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Limits.test.luau" }, { "Scene", "" }, { "Modes", Json::array({ "Editor" }) }, { "Overrides", Json{ { "CallbackBudgetMs", 40 }, { "MemoryLimitMB", 24 } } } } }) } } } } } });
			editor.GetAssets().WaitIdle();
			Test::AutomationTestClient& client = fixture;
			static_cast<void>(client.Submit("test.run", Json::object()));
			std::optional<ScriptingSettings> applied;
			std::optional<uint64_t> softLimit;
			std::optional<Json> run;
			REQUIRE(Test::WaitUntil([&client, &editor, &applied, &softLimit, &run]()
			{
				client.GetServer().Pump();
				if (const PlaySession* session = editor.GetPlay().GetSession(); session != nullptr && session->GetScripts() != nullptr && editor.GetPlay().IsTestRunActive() && !applied)
				{
					applied = session->GetScripts()->GetSettings();
					softLimit = session->GetScripts()->GetMemoryState().SoftLimitBytes;
				}
				for (Json& response : client.GetServer().TakeInProcessResponses(client.GetClient()))
					run = std::move(response);
				return run.has_value();
			}));
			CHECK(ReadRunResult(*run)["passed"] == Json(true));
			REQUIRE(applied.has_value());
			CHECK(applied->CallbackBudgetMs == 40u);
			CHECK(applied->MemoryLimitMB == 24u);
			CHECK(softLimit == std::optional<uint64_t>(24ull * 1024 * 1024));
			// The project's own values are untouched.
			CHECK(editor.GetProject().GetSettings().Scripting.CallbackBudgetMs == 700u);
			CHECK(editor.GetProject().GetSettings().Scripting.MemoryLimitMB == 96u);
		}

		TEST_CASE("TestMethods: run rejects dry runs and invalid limits without starting a suite")
		{
			Test::AutomationFixture fixture("TestRunParams");
			CHECK(fixture.Request("test.run", Json{ { "dryRun", true } })["error"]["data"]["errorCode"] == Json("Unsupported"));
			CHECK(fixture.Request("test.run", Json{ { "timeoutTicks", 0 } })["error"]["code"] == Json(-32602));
			CHECK(fixture.Request("test.run", Json{ { "record", "../outside.replay" } })["error"]["code"] == Json(-32602));
		}
	}

}
