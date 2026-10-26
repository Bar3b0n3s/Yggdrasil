#include "TestsPCH.h"
#include "Engine/Automation/Methods/ReplayMethods.h"

#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Events.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayPlayer.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		Json CallReplayMethod(Test::AutomationFixture& fixture, std::string_view method, const Json& params = Json::object())
		{
			auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			return std::move(*result);
		}

		ReplayDocument ReadRecordedReplay(EditorContext& editor, std::string_view path)
		{
			const auto parsed = VfsPath::Create("project", path);
			REQUIRE(parsed.has_value());
			const auto source = editor.GetVfs().ReadText(*parsed);
			REQUIRE(source.has_value());
			ReplayLoadReport report;
			auto replay = ReplayFromText(*source, report, true);
			REQUIRE_MESSAGE(replay.has_value(), (replay ? "" : replay.error().ToString()));
			return std::move(*replay);
		}

		// Saves the open scene with a "Mover" entity whose script moves it one unit along X on every fixed step while A, B,
		// C or Space is held, so a recording that presses one of them has an outcome that depends on its input and passes
		// input.record stop's input check (M16 FX-4).
		void SaveInputScene(Test::AutomationFixture& fixture)
		{
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/KeyMover.luau" }, { "source", "local KeyMover = {}\n"
																													"function KeyMover.OnFixedUpdate(self: any, dt: number)\n"
																													"\tif Input.IsKeyDown(\"A\") or Input.IsKeyDown(\"B\") or Input.IsKeyDown(\"C\") or Input.IsKeyDown(\"Space\") then\n"
																													"\t\tself.Entity.Transform:Translate(vector.create(1, 0, 0))\n"
																													"\tend\n"
																													"end\n"
																													"return Script.Define(\"KeyMover\", KeyMover)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Mover" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/KeyMover.luau" } } }) } } } } } });
			CallReplayMethod(fixture, "scene.save");
		}

		bool ProjectFileExists(Test::AutomationFixture& fixture, std::string_view path)
		{
			const auto output = VfsPath::Create("project", path);
			REQUIRE(output.has_value());
			return fixture.GetEditor().GetVfs().Exists(*output);
		}

		// Gives the recorded replay at `path` the expectations `expect` through the project's file system and reimports it:
		// input.record's Stop verifies every expectation before it writes a recording (Docs/Decisions/0030-m19-contract.md
		// decision 4), so a replay whose expectations fail or stop its playback is written this way.
		void WriteReplayExpectations(Test::AutomationFixture& fixture, std::string_view path, std::vector<ReplayExpectation> expect)
		{
			ReplayDocument replay = ReadRecordedReplay(fixture.GetEditor(), path);
			replay.Expect = std::move(expect);
			const Result<std::string> text = ReplayToText(replay);
			REQUIRE_MESSAGE(text.has_value(), (text ? "" : text.error().ToString()));
			const auto file = VfsPath::Create("project", path);
			REQUIRE(file.has_value());
			REQUIRE(fixture.GetEditor().GetVfs().WriteFileAtomic(*file, AsBytes(*text)).has_value());
			CallReplayMethod(fixture, "asset.reimport", Json{ { "asset", path } });
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ReplayMethods: recording disconnect cancels after its owner has left lockstep")
		{
			Test::AutomationFixture fixture("RecordingOwner");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.pause");
			PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			REQUIRE(session->GetRecorder() != nullptr);
			CHECK_FALSE(session->IsLockstep());
			CHECK(session->GetRecorder()->IsRecording());
			fixture.GetClient().GetServer().DisconnectInProcess(fixture.GetClient().GetClient());
			CHECK_FALSE(session->GetRecorder()->IsRecording());
			CHECK(session->IsPaused());
		}
		TEST_CASE("ReplayMethods: recording starts at tick zero and captures every applied input source")
		{
			Test::AutomationFixture fixture("RecordAppliedInput");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "play.start", Json{ { "lockstep", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 2 } });
			const uint64_t oldSerial = fixture.GetEditor().GetPlay().GetSession()->GetSerial();
			CHECK(fixture.Request("input.record", Json{ { "action", "start" } })["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetSerial() == oldSerial);
			const Json started = CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true }, { "seed", 123 }, { "parameters", Json{ { "level", 2 } } } });
			CHECK(started["tick"] == Json(0));
			CHECK(started["recording"] == Json(true));
			PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			CHECK(session->GetSerial() != oldSerial);
			session->GetInput().QueueDeviceEvent(KeyEvent{ .KeyCode = Key::A, .Action = ButtonAction::Pressed });
			// The Test host uses this same validated queue; its Lua adapter is exercised by test.run's recording tests.
			REQUIRE(session->GetInput().Queue(0, { .Type = PlayInputEventType::KeyInput, .State = PlayInputEventState::Tap, .KeyCode = Key::B }).has_value());
			CallReplayMethod(fixture, "input.inject", Json{ { "events", Json::array({ Json{ { "type", "key" }, { "key", "C" }, { "state", "tap" } } }) } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 2 }, { "render", "none" } });
			const Json stopped = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Applied.replay" } });
			CHECK(stopped["events"] == Json(5));
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(false));
			const ReplayDocument replay = ReadRecordedReplay(fixture.GetEditor(), "Assets/Tests/Applied.replay");
			CHECK(replay.Header.Seed == 123);
			CHECK(replay.Header.Parameters.Get() == Json({ { "level", 2 } }));
			REQUIRE(replay.Events.size() == 5);
			CHECK(replay.Events[0].KeyName == "A");
			CHECK(replay.Events[1].KeyName == "B");
			CHECK(replay.Events[2].KeyName == "C");
			CHECK(replay.Events[3].Tick == 1);
			CHECK(replay.Events[3].State == "Up");
			CHECK(replay.Events[4].State == "Up");
		}

		TEST_CASE("ReplayMethods: replay verifies expectations and the strict final state hash")
		{
			Test::AutomationFixture fixture("RecordReplayHash");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "seed", 42 }, { "parameters", Json{ { "message", "hello" } } } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 4 }, { "input", Json::array({ Json{ { "type", "key" }, { "key", "Space" }, { "state", "tap" } } }) } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Verified.replay" }, { "expect", Json::array({ Json{ { "tick", 0 }, { "luau", "Scene.GetLoadParameters().message == 'hello'" } }, Json{ { "tick", 4 }, { "luau", "Time.GetTick() == 4" } } }) } });
			const Json result = CallReplayMethod(fixture, "input.replay", Json{ { "path", "Assets/Tests/Verified.replay" }, { "verify", true }, { "strictHash", true } });
			CHECK(result["passed"] == Json(true));
			CHECK(result["hashChecked"] == Json(true));
			CHECK(result["hashMatched"] == Json(true));
			CHECK(result["stateHash"] == saved["stateHash"]);
			REQUIRE(result["expect"].size() == 2);
			CHECK(result["expect"][0]["satisfied"] == Json(true));
			CHECK(result["expect"][1]["satisfied"] == Json(true));
			const PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			CHECK(session->IsPaused());
			CHECK_FALSE(session->IsStepping());
			CHECK_FALSE(session->IsLockstep());
		}

		TEST_CASE("ReplayMethods: cancellation releases time ownership and never writes an invalid recording")
		{
			Test::AutomationFixture fixture("RecordInvalidated");
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Target" } });
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "script.eval", Json{ { "context", "play" }, { "code", "Scene.FindByName('Target').Name = 'Changed'" } });
			CHECK(fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Invalid.replay" } })["error"]["data"]["errorCode"] == Json("InvalidState"));
			const auto output = VfsPath::Create("project", "Assets/Tests/Invalid.replay");
			REQUIRE(output.has_value());
			CHECK_FALSE(fixture.GetEditor().GetVfs().Exists(*output));
			fixture.GetClient().GetServer().DisconnectInProcess(fixture.GetClient().GetClient());
			const PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			CHECK(session->IsPaused());
			CHECK_FALSE(session->IsLockstep());
			REQUIRE(session->GetRecorder() != nullptr);
			CHECK_FALSE(session->GetRecorder()->IsRecording());
		}

		TEST_CASE("ReplayMethods: verification failures retain every expectation outcome and source location")
		{
			Test::AutomationFixture fixture("ReplayOutcomes");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Failures.replay" } });
			WriteReplayExpectations(fixture, "Assets/Tests/Failures.replay", { { .Tick = 0, .Luau = "false" }, { .Tick = 0, .Luau = "local x = 1\nerror('replay fault')" }, { .Tick = 0, .Luau = "42" } });
			Json response;
			{
				Test::ExpectLog expected(LogLevel::Error, "replay fault");
				response = fixture.Request("input.replay", Json{ { "path", "Assets/Tests/Failures.replay" }, { "verify", true }, { "strictHash", true } });
			}
			CHECK(response["error"]["data"]["errorCode"] == Json("Validation"));
			const Json& outcome = response["error"]["data"]["replay"];
			CHECK(outcome["finalTick"] == Json(0));
			CHECK(outcome["hashChecked"] == Json(true));
			REQUIRE(outcome["expect"].size() == 3);
			CHECK(outcome["expect"][0]["satisfied"] == Json(false));
			CHECK(outcome["expect"][1]["file"] == Json("Assets/Tests/Failures.replay"));
			CHECK(outcome["expect"][1]["line"] == Json(2));
			CHECK(outcome["expect"][1]["jsonPointer"] == Json("/Expect/1/Luau"));
			CHECK(outcome["expect"][2]["satisfied"] == Json(true));
			CHECK(outcome["expect"][2]["value"] == Json(42));
		}

		TEST_CASE("ReplayMethods: another active driver is rejected before replacing the session")
		{
			Test::EditorTestFixture fixture("ReplayDriverAdmission");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto specification = Test::MakeTestServerSpecification();
			auto now = std::chrono::steady_clock::time_point{};
			specification.WallClock = [&now]
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), std::move(specification));
			SUBCASE("lockstep driver")
			{
				REQUIRE(client.Call("play.start", Json{ { "lockstep", true } }).has_value());
			}
			SUBCASE("paused driver without lockstep ownership")
			{
				REQUIRE(client.Call("play.start", Json{ { "paused", true } }).has_value());
			}
			const auto pending = client.Submit("play.step", Json{ { "ticks", 1000 } });
			static_cast<void>(pending);
			client.GetServer().Pump();
			PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			const uint64_t serial = session->GetSerial();
			REQUIRE(session->IsStepping());
			// Requests on the stepping client wait until its operation finishes. A second client reaches admission while
			// the first driver is still active; the missing path proves rejection precedes loading or replacement.
			auto& server = client.GetServer();
			const ClientId contender = server.ConnectInProcess("contender");
			REQUIRE(contender != NoClient);
			server.SubmitInProcess(contender, RpcRequest{ .Id = Json(1), .IsNotification = false, .Method = "input.replay", .Params = Json{ { "path", "Assets/Tests/Missing.replay" } }, .TranscriptLine = std::nullopt });
			server.Pump();
			const auto responses = server.TakeInProcessResponses(contender);
			REQUIRE(responses.size() == 1);
			REQUIRE(responses.front().contains("error"));
			CHECK(responses.front()["error"]["data"]["errorCode"] == Json("InvalidState"));
			REQUIRE(fixture.GetEditor().GetPlay().GetSession() == session);
			CHECK(session->GetSerial() == serial);
			CHECK(session->IsStepping());
			CHECK(session->GetTick() < 1000);
			CHECK(server.TakeInProcessResponses(client.GetClient()).empty());
			server.DisconnectInProcess(client.GetClient());
			CHECK_FALSE(session->IsStepping());
			// Once the driver is gone, the same request reaches loading and reports the missing replay normally.
			server.SubmitInProcess(contender, RpcRequest{ .Id = Json(2), .IsNotification = false, .Method = "input.replay", .Params = Json{ { "path", "Assets/Tests/Missing.replay" } }, .TranscriptLine = std::nullopt });
			server.Pump();
			const auto after = server.TakeInProcessResponses(contender);
			REQUIRE(after.size() == 1);
			REQUIRE(after.front().contains("error"));
			CHECK(after.front()["error"]["data"]["errorCode"] == Json("NotFound"));
			CHECK(session->GetSerial() == serial);
			server.DisconnectInProcess(contender);
		}

		TEST_CASE("ReplayMethods: read-only recording start is transient and stop checks write permission")
		{
			Test::AutomationFixture fixture("ReplayReadOnly");
			EditorContext& editor = fixture.GetEditor();
			const auto projectFile = editor.GetProject().GetProjectFile();
			REQUIRE(editor.CloseProject().has_value());
			const auto cache = fixture.GetEditorFixture().GetDirectory().GetPath() / "PrivateCache";
			auto project = ProjectManager::OpenProject(projectFile, { .ReadOnly = true, .ReadOnlyCacheDirectory = cache }, editor.GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(editor.OpenProject(std::move(*project)).has_value());
			CallReplayMethod(fixture, "scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } });
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 1 } });
			CHECK(fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Denied.replay" } })["error"]["data"]["errorCode"] == Json("PermissionDenied"));
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(true));
			const auto output = VfsPath::Create("project", "Assets/Tests/Denied.replay");
			REQUIRE(output.has_value());
			CHECK_FALSE(editor.GetVfs().Exists(*output));
		}

		TEST_CASE("ReplayMethods: pending replay disconnect releases its session without another poll")
		{
			Test::EditorTestFixture fixture("ReplayDisconnect");
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
			// The recording presses keys, so its scene must react to them for input.record stop's input check to let it through.
			REQUIRE(client.Call("script.write", Json{ { "path", "Assets/Scripts/KeyMover.luau" }, { "source", "local KeyMover = {}\n"
																											  "function KeyMover.OnFixedUpdate(self: any, dt: number)\n"
																											  "\tif Input.IsKeyDown(\"A\") or Input.IsKeyDown(\"B\") then self.Entity.Transform:Translate(vector.create(1, 0, 0)) end\n"
																											  "end\n"
																											  "return Script.Define(\"KeyMover\", KeyMover)\n" } })
					.has_value());
			REQUIRE(client.Call("entity.create", Json{ { "name", "Mover" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/KeyMover.luau" } } }) } } } } } }).has_value());
			REQUIRE(client.Call("scene.save", Json::object()).has_value());
			REQUIRE(client.Call("input.record", Json{ { "action", "start" } }).has_value());
			REQUIRE(client.Call("input.inject", Json{ { "events", Json::array({ Json{ { "tick", 50 }, { "type", "key" }, { "key", "A" }, { "state", "down" } }, Json{ { "tick", 51 }, { "type", "key" }, { "key", "A" }, { "state", "up" } }, Json{ { "tick", 60 }, { "type", "key" }, { "key", "B" }, { "state", "tap" } } }) } }).has_value());
			REQUIRE(client.Call("play.step", Json{ { "ticks", 100 }, { "render", "none" } }).has_value());
			REQUIRE(client.Call("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Long.replay" } }).has_value());
			static_cast<void>(client.Submit("input.replay", Json{ { "path", "Assets/Tests/Long.replay" }, { "verify", true } }));
			client.GetServer().Pump();
			PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			REQUIRE(session->IsStepping());
			REQUIRE(session->GetTick() < 50);
			const uint64_t tick = session->GetTick();
			client.GetServer().DisconnectInProcess(client.GetClient());
			CHECK_FALSE(session->IsStepping());
			CHECK_FALSE(session->IsLockstep());
			CHECK_FALSE(session->IsAudioTimeOwned());
			CHECK(session->IsPaused());
			CHECK(session->GetTick() == tick);
			auto& server = client.GetServer();
			const ClientId resumed = server.ConnectInProcess("resumed");
			server.SubmitInProcess(resumed, RpcRequest{ .Id = Json(1), .Method = "input.inject", .Params = Json{ { "events", Json::array({ Json{ { "tick", 50 - tick }, { "type", "key" }, { "key", "C" }, { "state", "tap" } } }) } }, .TranscriptLine = std::nullopt });
			server.Pump();
			const auto responses = server.TakeInProcessResponses(resumed);
			REQUIRE(responses.size() == 1);
			REQUIRE_MESSAGE(responses.front().contains("result"), responses.front().dump());
			for (uint64_t applied = tick; applied < 65; ++applied)
			{
				session->Tick();
				REQUIRE(session->GetTick() == applied + 1);
				CHECK_FALSE(session->GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::A));
				CHECK_FALSE(session->GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::B));
				CHECK(session->GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::C) == (applied == 50));
			}
			server.DisconnectInProcess(resumed);
		}

		TEST_CASE("ReplayMethods: a recording starts from its parameters as the replay stores them, so the strict hash reproduces")
		{
			Test::AutomationFixture fixture("RecordParameterForm");
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Namer.luau" }, { "source", "local Namer = {}\nfunction Namer:OnCreate() self.Entity.Name = \"ratio=\" .. tostring(Scene.GetLoadParameters().ratio) end\nreturn Script.Define(\"Namer\", Namer)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Namer" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Namer.luau" } } }) } } } } } });
			CallReplayMethod(fixture, "scene.save");
			// 0.123456789 is not a float; the header stores the float form, which the recorded session must use too.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "parameters", Json{ { "ratio", 0.123456789 } } } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 2 }, { "render", "none" } });
			// The recorded session read the float form (Stop then leaves the play session to its verification).
			const Json recordedName = CallReplayMethod(fixture, "script.eval", Json{ { "context", "play" }, { "code", "return Scene.FindByName('ratio=0.12345679') ~= nil" } });
			CHECK(recordedName["value"] == Json(true));
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Ratio.replay" } });
			const Json replayed = CallReplayMethod(fixture, "input.replay", Json{ { "path", "Assets/Tests/Ratio.replay" }, { "verify", true }, { "strictHash", true } });
			CHECK(replayed["passed"] == Json(true));
			CHECK(replayed["hashMatched"] == Json(true));
			CHECK(replayed["stateHash"] == saved["stateHash"]);
			const ReplayDocument replay = ReadRecordedReplay(fixture.GetEditor(), "Assets/Tests/Ratio.replay");
			CHECK(replay.Header.Parameters.Get() == JsonReader::Parse(R"({"ratio":0.12345679})").value_or(Json()));

			// A float the replay header cannot store is refused before the recording starts, located at its member.
			const Json refused = fixture.Request("input.record", Json{ { "action", "start" }, { "restart", true }, { "parameters", Json{ { "big", 1e39 } } } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			CHECK(refused["error"]["data"]["errorCode"] == Json("InvalidArgument"));
			CHECK(refused.dump().find("/parameters/big") != std::string::npos);
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(false));
		}

		TEST_CASE("ReplayMethods: a play-scene edit during a recording invalidates it and Stop writes nothing")
		{
			Test::AutomationFixture fixture("RecordPlaySceneEdit");
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Ball" } });
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "entity.update", Json{ { "entity", "/Ball" }, { "target", "play" }, { "name", "Moved" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 3 }, { "render", "none" } });
			const Json stopped = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Edited.replay" } });
			REQUIRE_MESSAGE(stopped.contains("error"), stopped.dump());
			CHECK(stopped["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(stopped.dump().find("play scene was edited") != std::string::npos);
			const auto output = VfsPath::Create("project", "Assets/Tests/Edited.replay");
			REQUIRE(output.has_value());
			CHECK_FALSE(fixture.GetEditor().GetVfs().Exists(*output));
			// A dry run takes its edit back, so it never invalidates a recording.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "entity.update", Json{ { "entity", "/Ball" }, { "target", "play" }, { "name", "Preview" }, { "dryRun", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 1 }, { "render", "none" } });
			CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/DryRun.replay" } });
		}

		TEST_CASE("ReplayMethods: the recording owner restarts an active or invalidated recording")
		{
			Test::AutomationFixture fixture("RecordOwnerRestart");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 2 }, { "render", "none" } });
			// §13.6: while playing, start with restart:true restarts at tick 0, also over the owner's own active recording.
			const Json restarted = CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CHECK(restarted["tick"] == Json(0));
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(true));
			// An invalidated Stop ends the recording, so admission recovers without play.stop.
			CallReplayMethod(fixture, "script.eval", Json{ { "context", "play" }, { "code", "Scene.CreateEntity('External'); return true" } });
			CHECK(fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Invalid.replay" } })["error"]["data"]["errorCode"] == Json("InvalidState"));
			const Json state = CallReplayMethod(fixture, "play.state");
			CHECK(state["recording"] == Json(false));
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 1 }, { "render", "none" } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Again.replay" } });
			CHECK(saved["tick"] == Json(1));
		}

		TEST_CASE("ReplayMethods: a replay whose game quits before its final tick fails with its complete outcome")
		{
			Test::AutomationFixture fixture("ReplayQuitsEarly");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 6 }, { "render", "none" } });
			CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Quits.replay" } });
			WriteReplayExpectations(fixture, "Assets/Tests/Quits.replay", { { .Tick = 2, .Luau = "Application.Quit(3); return true" }, { .Tick = 5, .Luau = "true" } });
			const Json response = fixture.Request("input.replay", Json{ { "path", "Assets/Tests/Quits.replay" }, { "verify", true }, { "strictHash", true } });
			REQUIRE_MESSAGE(response.contains("error"), response.dump());
			CHECK(response["error"]["data"]["errorCode"] == Json("Validation"));
			REQUIRE_MESSAGE(response["error"]["data"].contains("replay"), response.dump());
			const Json& outcome = response["error"]["data"]["replay"];
			CHECK(outcome["passed"] == Json(false));
			CHECK(outcome["finalTick"] == Json(2));
			CHECK(outcome["hashMatched"] == Json(false));
			CHECK(JsonReader(outcome).ReadMember<std::string>("stopReason").value_or(std::string()).find("quit with exit code 3 at tick 2") != std::string::npos);
			REQUIRE(outcome["expect"].size() == 2);
			CHECK(outcome["expect"][0]["satisfied"] == Json(true));
			CHECK(outcome["expect"][1]["evaluated"] == Json(false));
			CHECK(outcome["expect"][1]["jsonPointer"] == Json("/Expect/1/Luau"));
			const PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			CHECK_FALSE(session->IsStepping());
			CHECK_FALSE(session->IsLockstep());
		}

		TEST_CASE("ReplayMethods: stop validates its destination and predicates before consuming the recording")
		{
			Test::AutomationFixture fixture("RecordStopPreflight");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CHECK(fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "../Outside.replay" } }).contains("error"));
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(true));
			CHECK(fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Good.replay" }, { "expect", Json::array({ Json{ { "tick", 1 }, { "luau", "true" } } }) } }).contains("error"));
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(true));
			CHECK(fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Good.replay" }, { "expect", Json::array({ Json{ { "tick", 0 }, { "luau", "return (" } } }) } }).contains("error"));
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(true));
			CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Good.replay" } });
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(false));
		}

		// M16 friction S2-01 (FX-4): Breakout's replay ended on a restart, so a run without any input reached the same final
		// state, and the replay proved nothing about the gameplay it recorded. Stop now replays a recording with input
		// events without them and refuses to write it when that run passes the same verification.
		TEST_CASE("ReplayMethods: input.record stop refuses a recording whose final state does not depend on its input")
		{
			Test::AutomationFixture fixture("RecordInputIndependent");
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 3 }, { "type", "key" }, { "key", "Space" }, { "state", "tap" } } }) } });
			const Json refused = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Idle.replay" }, { "expect", Json::array({ Json{ { "tick", 130 }, { "luau", "Time.GetTick() == 130" } } }) } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			const Json& data = refused["error"]["data"];
			CHECK(data["errorCode"] == Json("Validation"));
			const std::string detail = JsonReader(data).ReadMember<std::string>("detail").value_or("");
			CHECK(detail.find("Assets/Tests/Idle.replay") != std::string::npos);
			CHECK(detail.find("does not depend on its input") != std::string::npos);
			const std::string hint = JsonReader(data).ReadMember<std::string>("hint").value_or("");
			CHECK(hint.find("restart") != std::string::npos);
			// M16 friction S3-01: the input check's real rule, not a trailing restart as such.
			CHECK(hint.find(std::format("never shows in the world at a checkpoint, one every {} ticks, at its end or in an expectation", ReplayCheckpointInterval))
				!= std::string::npos);
			// The input check's own outcome: every checkpoint, the final hash and the expectation reproduced without input.
			const Json& check = data["inputCheck"];
			CHECK(check["passed"] == Json(true));
			CHECK(check["withoutInput"] == Json(true));
			CHECK(check["checkpointsChecked"] == Json(2));
			CHECK(check["finalTick"] == Json(130));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Idle.replay"));
			const Json state = CallReplayMethod(fixture, "play.state");
			CHECK(state["recording"] == Json(false));
			CHECK(state["lockstep"] == Json(false));
			CHECK(state["tick"] == Json(130));

			// A recording without input events is a pure determinism replay: it has nothing to depend on, so it is written once
			// its verification reproduced it (Docs/Decisions/0030-m19-contract.md decision 4), without an input check.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" } });
			const uint64_t serial = fixture.GetEditor().GetPlay().GetSession()->GetSerial();
			const Json pure = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Pure.replay" } });
			CHECK(pure["events"] == Json(0));
			CHECK(pure["checkpoints"] == Json(2));
			CHECK(pure["inputCheckTick"] == Json(0));
			CHECK(ProjectFileExists(fixture, "Assets/Tests/Pure.replay"));
			// No input check ran: the play session is the verification's, paused at the replay's last tick.
			REQUIRE(fixture.GetEditor().GetPlay().GetSession() != nullptr);
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetSerial() != serial);
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetTick() == 130);
			CHECK(fixture.GetEditor().GetPlay().GetSession()->IsPaused());

			// An expectation that only the input satisfies makes the outcome depend on it, although the world does not.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 6 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 3 }, { "type", "key" }, { "key", "Space" }, { "state", "tap" } } }) } });
			const Json observed = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Observed.replay" }, { "expect", Json::array({ Json{ { "tick", 4 }, { "luau", "Input.IsKeyDown(\"Space\")" } } }) } });
			CHECK(observed["events"] == Json(2));
			CHECK(observed["inputCheckTick"] == Json(6));
			CHECK(ProjectFileExists(fixture, "Assets/Tests/Observed.replay"));
		}

		// One rule decides input independence (Architecture §13.6): the input-free run passes the recording's verification,
		// whatever script errors that run raised. The Watcher faults only when Space was never held, so the input check's
		// run raises a script error that the recorded run never did, while the world, which the Watcher never changes,
		// reproduces every hash.
		TEST_CASE("ReplayMethods: input.record stop refuses an input-independent recording although its input-free run raised a script error")
		{
			Test::AutomationFixture fixture("RecordInputIndependentFault");
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Watcher.luau" }, { "source", "local Watcher = {}\n"
																												   "function Watcher.OnFixedUpdate(self: any, dt: number)\n"
																												   "\tif Input.IsKeyDown(\"Space\") then self.Pressed = true end\n"
																												   "\tif Time.GetTick() == 10 and not self.Pressed then error(\"idle fault\") end\n"
																												   "end\n"
																												   "return Script.Define(\"Watcher\", Watcher)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Watcher" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Watcher.luau" } } }) } } } } } });
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 70 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 3 }, { "type", "key" }, { "key", "Space" }, { "state", "tap" } } }) } });
			CHECK(CallReplayMethod(fixture, "script.errors")["errors"].empty());
			Json refused;
			{
				Test::ExpectLog expected(LogLevel::Error, "idle fault");
				refused = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Faulted.replay" } });
			}
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			const Json& data = refused["error"]["data"];
			CHECK(data["errorCode"] == Json("Validation"));
			CHECK(JsonReader(data).ReadMember<std::string>("detail").value_or("").find("does not depend on its input") != std::string::npos);
			CHECK(data["inputCheck"]["passed"] == Json(true));
			CHECK(data["inputCheck"]["checkpointsChecked"] == Json(1));
			CHECK(data["inputCheck"]["finalTick"] == Json(70));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Faulted.replay"));
			// The session is the input check's: its run raised the Watcher's error.
			CHECK(CallReplayMethod(fixture, "script.errors").dump().find("idle fault") != std::string::npos);
		}

		TEST_CASE("ReplayMethods: input.record stop writes an input-dependent recording with its state-hash checkpoints")
		{
			Test::AutomationFixture fixture("RecordCheckpoints");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			// A is held from tick 100 on: the world matches an input-free run at checkpoint 60 and differs at 120.
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 100 }, { "type", "key" }, { "key", "A" }, { "state", "down" } } }) } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Checkpoints.replay" } });
			CHECK(saved["tick"] == Json(130));
			CHECK(saved["events"] == Json(1));
			CHECK(saved["checkpoints"] == Json(2));
			CHECK(saved["inputCheckTick"] == Json(120));
			const ReplayDocument replay = ReadRecordedReplay(fixture.GetEditor(), "Assets/Tests/Checkpoints.replay");
			CHECK(replay.Version == ReplayFormatVersion);
			REQUIRE(replay.Checkpoints.size() == 2);
			CHECK(replay.Checkpoints[0].Tick == 60);
			CHECK(replay.Checkpoints[1].Tick == 120);
			// The input check plays on past the checkpoint that differed to the final tick (Docs/Decisions/0030-m19-contract.md
			// decision 3) and leaves its session paused there, with lockstep released, as input.replay does.
			const Json state = CallReplayMethod(fixture, "play.state");
			CHECK(state["tick"] == Json(130));
			CHECK(state["lockstep"] == Json(false));
			CHECK(state["state"] == Json("Paused"));

			const Json replayed = CallReplayMethod(fixture, "input.replay", Json{ { "path", "Assets/Tests/Checkpoints.replay" }, { "verify", true }, { "strictHash", true } });
			CHECK(replayed["passed"] == Json(true));
			CHECK(replayed["checkpointsChecked"] == Json(2));
			CHECK(replayed["checkpointFailed"] == Json(false));
			CHECK(replayed["stateHash"] == saved["stateHash"]);
			CHECK(replayed["withoutInput"] == Json(false));
		}

		TEST_CASE("ReplayMethods: input.replay stops at the first checkpoint that differs and names its tick and both hashes")
		{
			Test::AutomationFixture fixture("ReplayCheckpointMismatch");
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Tampered.replay" } });
			REQUIRE(saved["checkpoints"] == Json(2));
			ReplayDocument replay = ReadRecordedReplay(fixture.GetEditor(), "Assets/Tests/Tampered.replay");
			const std::string recorded = replay.Checkpoints[1].StateHash;
			replay.Checkpoints[1].StateHash = recorded == "0000000000000000" ? "0000000000000001" : "0000000000000000";
			const Result<std::string> text = ReplayToText(replay);
			REQUIRE(text.has_value());
			const auto file = VfsPath::Create("project", "Assets/Tests/Tampered.replay");
			REQUIRE(file.has_value());
			REQUIRE(fixture.GetEditor().GetVfs().WriteFileAtomic(*file, AsBytes(*text)).has_value());
			CallReplayMethod(fixture, "asset.reimport", Json{ { "asset", "Assets/Tests/Tampered.replay" } });

			const Json response = fixture.Request("input.replay", Json{ { "path", "Assets/Tests/Tampered.replay" }, { "verify", true }, { "strictHash", true } });
			REQUIRE_MESSAGE(response.contains("error"), response.dump());
			const Json& data = response["error"]["data"];
			CHECK(data["errorCode"] == Json("Validation"));
			const std::string detail = JsonReader(data).ReadMember<std::string>("detail").value_or("");
			CHECK(detail.find("checkpoint tick 120") != std::string::npos);
			CHECK(detail.find(recorded) != std::string::npos);
			CHECK(detail.find(replay.Checkpoints[1].StateHash) != std::string::npos);
			const Json& outcome = data["replay"];
			CHECK(outcome["passed"] == Json(false));
			CHECK(outcome["checkpointFailed"] == Json(true));
			CHECK(outcome["checkpointsChecked"] == Json(2));
			CHECK(outcome["finalTick"] == Json(120));
			CHECK(outcome["stateHash"] == Json(recorded));
			CHECK(outcome["expectedStateHash"] == Json(replay.Checkpoints[1].StateHash));
			CHECK(CallReplayMethod(fixture, "play.state")["tick"] == Json(120));
			// Without strictHash no hash is compared, checkpoints included.
			const Json loose = CallReplayMethod(fixture, "input.replay", Json{ { "path", "Assets/Tests/Tampered.replay" }, { "verify", true } });
			CHECK(loose["passed"] == Json(true));
			CHECK(loose["checkpointsChecked"] == Json(0));
			CHECK(loose["finalTick"] == Json(130));
		}

		// M17 friction F03: the look's components (PostProcess here) are serialized components, so they are in the state hash
		// and a look tuned after a recording fails its replay, which named only the two hashes. A checkpoint or final hash that
		// differs now carries a hint naming what changes the world and how to re-record, and a checkpoint's stop reason names
		// the causes; an expectation that fails while the hash matches gets no such hint.
		TEST_CASE("ReplayMethods: input.replay's checkpoint and final-hash failures hint that the world changed since the recording")
		{
			Test::AutomationFixture fixture("ReplayDivergenceHint");
			const std::string hint(GetReplayDivergenceHint());
			CHECK(hint.find("the look's included") != std::string::npos);
			// M17 review D-06/R-03: an engine hint names the method, which engine_client and Runtime callers can call, and the
			// skill section as game-building titles it, never the MCP tool.
			CHECK(hint.ends_with("; re-record it with test.run {record} (the game-building skill, \"Re-recording replays\")"));
			CHECK(hint.find("test_run") == std::string::npos);
			const auto setRadius = [&fixture](double radius)
			{
				CallReplayMethod(fixture, "entity.update", Json{ { "entity", "/Look" }, { "components", Json{ { "PostProcess", Json{ { "SsaoRadius", radius } } } } } });
				CallReplayMethod(fixture, "scene.save");
			};
			const auto record = [&fixture](std::string_view path, uint32_t ticks, const Json& expect)
			{
				CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
				CallReplayMethod(fixture, "play.step", Json{ { "ticks", ticks }, { "render", "none" } });
				CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", path }, { "expect", expect } });
				CallReplayMethod(fixture, "play.stop");
			};
			const Json verify = Json{ { "verify", true }, { "strictHash", true } };
			const auto replay = [&fixture, &verify](std::string_view path)
			{
				Json params = verify;
				params["path"] = path;
				return fixture.Request("input.replay", params);
			};
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Look" }, { "components", Json{ { "PostProcess", Json{ { "SsaoRadius", 0.5 } } } } } });
			CallReplayMethod(fixture, "scene.save");
			record("Assets/Tests/Look.replay", 70, Json::array());
			record("Assets/Tests/Short.replay", 30, Json::array());
			record("Assets/Tests/False.replay", 30, Json::array());
			WriteReplayExpectations(fixture, "Assets/Tests/False.replay", { { .Tick = 10, .Luau = "false" } });
			CHECK_FALSE(replay("Assets/Tests/Look.replay").contains("error"));
			CallReplayMethod(fixture, "play.stop");

			// Only the look changed: the replay stops at its first checkpoint.
			setRadius(1.5);
			const Json checkpoint = replay("Assets/Tests/Look.replay");
			REQUIRE_MESSAGE(checkpoint.contains("error"), checkpoint.dump());
			const Json& data = checkpoint["error"]["data"];
			CHECK(data["errorCode"] == Json("Validation"));
			CHECK(JsonReader(data).ReadMember<std::string>("detail").value_or("").starts_with("replay verification failed at checkpoint tick 60: expected state hash "));
			CHECK(JsonReader(data).ReadMember<std::string>("hint").value_or("") == hint);
			CHECK(data["replay"]["checkpointFailed"] == Json(true));
			const std::string stopReason = JsonReader(data["replay"]).ReadMember<std::string>("stopReason").value_or("");
			CHECK(stopReason.find("checkpoint tick 60") != std::string::npos);
			CHECK(stopReason.find(GetReplayDivergenceCauses()) != std::string::npos);
			CallReplayMethod(fixture, "play.stop");

			// A replay shorter than a checkpoint interval fails at its final hash, with the same hint and its message kept; its
			// start fingerprint names the look's change after it (Docs/Decisions/0030-m19-contract.md decision 2).
			const Json finalHash = replay("Assets/Tests/Short.replay");
			REQUIRE_MESSAGE(finalHash.contains("error"), finalHash.dump());
			const std::string finalDetail = JsonReader(finalHash["error"]["data"]).ReadMember<std::string>("detail").value_or("");
			CHECK(finalDetail.starts_with("replay verification failed at tick 30; changed since the recording: component PostProcess of /Look ("));
			CHECK(JsonReader(finalHash["error"]["data"]).ReadMember<std::string>("hint").value_or("") == hint);
			CHECK(finalHash["error"]["data"]["replay"]["hashMatched"] == Json(false));
			CHECK(finalHash["error"]["data"]["replay"]["checkpointFailed"] == Json(false));
			CallReplayMethod(fixture, "play.stop");

			// Back to the recorded look, only the false expectation fails: no hint.
			setRadius(0.5);
			const Json expectation = replay("Assets/Tests/False.replay");
			REQUIRE_MESSAGE(expectation.contains("error"), expectation.dump());
			CHECK(expectation["error"]["data"]["detail"] == Json("replay verification failed at tick 30"));
			CHECK(expectation["error"]["data"]["replay"]["hashMatched"] == Json(true));
			CHECK_FALSE(expectation["error"]["data"].contains("hint"));
		}

		TEST_CASE("ReplayMethods: input.replay withoutInput applies none of the replay's events")
		{
			Test::AutomationFixture fixture("ReplayWithoutInput");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 5 }, { "type", "key" }, { "key", "B" }, { "state", "down" } } }) } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Pressed.replay" } });
			CHECK(CallReplayMethod(fixture, "input.replay", Json{ { "path", "Assets/Tests/Pressed.replay" }, { "verify", true }, { "strictHash", true } })["passed"] == Json(true));
			const Json response = fixture.Request("input.replay", Json{ { "path", "Assets/Tests/Pressed.replay" }, { "verify", true }, { "strictHash", true }, { "withoutInput", true } });
			REQUIRE_MESSAGE(response.contains("error"), response.dump());
			const Json& outcome = response["error"]["data"]["replay"];
			CHECK(outcome["withoutInput"] == Json(true));
			CHECK(outcome["hashMatched"] == Json(false));
			CHECK(outcome["finalTick"] == Json(20));
			CHECK(outcome["expectedStateHash"] == saved["stateHash"]);
			// Without input the Mover never moved.
			const Json moved = CallReplayMethod(fixture, "script.eval", Json{ { "context", "play" }, { "code", "return Scene.FindByName('Mover').Transform.WorldPosition.x" } });
			CHECK(moved["value"] == Json(0));
		}

		// input.replay {withoutInput: true} is the input check, whose failure is the expected outcome for a replay with input
		// events. M17 friction F03's divergence hint and causes, which ask for a re-recording, were added to its checkpoint and
		// final-hash failures too; they belong only to a playback with the recording's input, so these failures carry neither
		// and say instead that the replay ran without its input events, as an early stop's does.
		TEST_CASE("ReplayMethods: input.replay withoutInput failures carry no divergence hint and say the replay ran without its input")
		{
			Test::AutomationFixture fixture("ReplayWithoutInputNoHint");
			SaveInputScene(fixture);
			const std::string causes(GetReplayDivergenceCauses());
			const std::string note = "the replay ran without its input events (withoutInput), which a replay that depends on its input is expected to fail";
			const auto record = [&fixture](std::string_view path, uint32_t ticks, std::string_view key)
			{
				CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
				CallReplayMethod(fixture, "play.step", Json{ { "ticks", ticks }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 3 }, { "type", "key" }, { "key", key }, { "state", "tap" } } }) } });
				const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", path } });
				CHECK(JsonReader(saved).ReadMember<uint32_t>("inputCheckTick").value_or(0) > 0);
				CallReplayMethod(fixture, "play.stop");
			};
			const auto replayWithoutInput = [&fixture](std::string_view path)
			{
				const Json response = fixture.Request("input.replay", Json{ { "path", path }, { "verify", true }, { "strictHash", true }, { "withoutInput", true } });
				REQUIRE_MESSAGE(response.contains("error"), response.dump());
				return response["error"]["data"];
			};
			record("Assets/Tests/Pressed.replay", 70, "B");
			record("Assets/Tests/Short.replay", 20, "B");

			// Without its input the Mover never moves, so the replay stops at its first checkpoint.
			const Json checkpoint = replayWithoutInput("Assets/Tests/Pressed.replay");
			CHECK(checkpoint["errorCode"] == Json("Validation"));
			const std::string detail = JsonReader(checkpoint).ReadMember<std::string>("detail").value_or("");
			CHECK(detail.starts_with("replay verification failed at checkpoint tick 60: expected state hash "));
			CHECK(detail.ends_with("; " + note));
			CHECK_FALSE(checkpoint.contains("hint"));
			CHECK(checkpoint["replay"]["checkpointFailed"] == Json(true));
			CHECK(checkpoint["replay"]["withoutInput"] == Json(true));
			const std::string stopReason = JsonReader(checkpoint["replay"]).ReadMember<std::string>("stopReason").value_or("");
			CHECK(stopReason.find("checkpoint tick 60") != std::string::npos);
			CHECK(stopReason.find("the replay ran without its input events") != std::string::npos);
			CHECK(checkpoint.dump().find(causes) == std::string::npos);
			CallReplayMethod(fixture, "play.stop");

			// A replay shorter than a checkpoint interval fails at its final hash.
			const Json finalHash = replayWithoutInput("Assets/Tests/Short.replay");
			CHECK(finalHash["detail"] == Json("replay verification failed at tick 20; " + note));
			CHECK_FALSE(finalHash.contains("hint"));
			CHECK(finalHash["replay"]["hashMatched"] == Json(false));
			CHECK(finalHash["replay"]["checkpointFailed"] == Json(false));
			CHECK(finalHash.dump().find(causes) == std::string::npos);
			CallReplayMethod(fixture, "play.stop");

			// Played with their input, both still verify.
			for (const std::string_view path : { "Assets/Tests/Pressed.replay", "Assets/Tests/Short.replay" })
			{
				CHECK(CallReplayMethod(fixture, "input.replay", Json{ { "path", path }, { "verify", true }, { "strictHash", true } })["passed"] == Json(true));
				CallReplayMethod(fixture, "play.stop");
			}

			// A game that quits only without the input (C was never held by tick 10) stops early, and says so too.
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Quitter.luau" }, { "source", "local Quitter = {}\n"
																												   "function Quitter.OnFixedUpdate(self: any, dt: number)\n"
																												   "\tif Input.IsKeyDown(\"C\") then self.Pressed = true end\n"
																												   "\tif Time.GetTick() == 10 and not self.Pressed then Application.Quit(3) end\n"
																												   "end\n"
																												   "return Script.Define(\"Quitter\", Quitter)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Quitter" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Quitter.luau" } } }) } } } } } });
			CallReplayMethod(fixture, "scene.save");
			record("Assets/Tests/Quits.replay", 20, "C");
			const Json stopped = replayWithoutInput("Assets/Tests/Quits.replay");
			const std::string stoppedDetail = JsonReader(stopped).ReadMember<std::string>("detail").value_or("");
			CAPTURE(stoppedDetail);
			// Application.Quit in the fixed step that Time.GetTick() numbers 10 lets that step complete: the replay stops at tick 11.
			CHECK(stoppedDetail.starts_with("replay failed: the game quit with exit code 3 at tick 11"));
			CHECK(stoppedDetail.ends_with("; " + note));
			CHECK_FALSE(stopped.contains("hint"));
			CHECK(stopped["replay"]["finalTick"] == Json(11));
		}

		// The recording guards of input.record stop keep their own messages. The input check without the recording's input,
		// below, names neither the divergence causes nor the without-input note of input.replay; the verification with its
		// input names no causes either (its case, "input.record stop refuses a recording that a replay with its input does
		// not reproduce", checks its error data).
		TEST_CASE("ReplayMethods: input.record stop's input check keeps its own refusal without the divergence hint")
		{
			Test::AutomationFixture fixture("RecordGuardMessages");
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 70 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 3 }, { "type", "key" }, { "key", "Space" }, { "state", "tap" } } }) } });
			const Json refused = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Idle.replay" } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			const Json& data = refused["error"]["data"];
			CHECK(data["errorCode"] == Json("Validation"));
			CHECK(data["detail"] == Json("refusing to write 'Assets/Tests/Idle.replay': its outcome does not depend on its input (the same scene, seed and parameters run without its 2 "
										 "input event(s) reproduce its final state hash at tick 70, its 1 checkpoint(s) and its 0 expectation(s))"));
			CHECK(data["hint"] == Json(std::format("end the recording while the played state is still in the scene (a recording is refused only when its input never shows in the world at a "
												   "checkpoint, one every {0} ticks, at its end or in an expectation, as in one shorter than {0} ticks that ends idle, input that changes "
												   "only script state, or play after the last checkpoint that a restart undoes), or add an expectation that only the input satisfies; "
												   "a replay without input events records a pure determinism run",
					  ReplayCheckpointInterval)));
			CHECK(data["inputCheck"]["passed"] == Json(true));
			CHECK(JsonReader(data["inputCheck"]).ReadMember<std::string>("stopReason").value_or("").empty());
			const std::string text = data.dump();
			CHECK(text.find(GetReplayDivergenceCauses()) == std::string::npos);
			CHECK(text.find("ran without its input events") == std::string::npos);
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Idle.replay"));
		}

		// M16 review R19: without verify nothing is checked, so input.replay {withoutInput: true} always passed and the input
		// check proved nothing. It is refused at /withoutInput, as strictHash without verify is, and changes no session.
		TEST_CASE("ReplayMethods: input.replay refuses withoutInput without verify")
		{
			Test::AutomationFixture fixture("ReplayWithoutInputNoVerify");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 5 }, { "type", "key" }, { "key", "B" }, { "state", "down" } } }) } });
			CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Pressed.replay" } });
			const Json before = CallReplayMethod(fixture, "play.state");

			for (const Json& params : { Json{ { "path", "Assets/Tests/Pressed.replay" }, { "withoutInput", true } },
					 Json{ { "path", "Assets/Tests/Pressed.replay" }, { "verify", false }, { "withoutInput", true } } })
			{
				CAPTURE(params.dump());
				const Json response = fixture.Request("input.replay", params);
				REQUIRE_MESSAGE(response.contains("error"), response.dump());
				const Json& data = response["error"]["data"];
				CHECK(data["errorCode"] == Json("InvalidArgument"));
				CHECK(data.dump().find("/withoutInput") != std::string::npos);
				CHECK(JsonReader(data).ReadMember<std::string>("detail").value_or("").find("withoutInput requires verify:true") != std::string::npos);
			}
			// The refusal restarted nothing: the recorded session stays where it was.
			CHECK(CallReplayMethod(fixture, "play.state")["tick"] == before["tick"]);
		}

		// Docs/Decisions/0027-m17-contract.md decision 3, A1 (M17 maps GTF-01, GDF-03, GAF-09): Stop played a recording with
		// input events only without its input, so it wrote a recording that its own input does not reproduce: one changed
		// by a script-table write from script.eval (no engine call, so nothing invalidates the recording), or one whose
		// expectation is false (the input check fails it too, which let it through). Stop now replays the candidate with
		// its input first, in a fresh ordinary playback, and refuses it naming where that playback diverged.
		TEST_CASE("ReplayMethods: input.record stop refuses a recording that a replay with its input does not reproduce")
		{
			Test::AutomationFixture fixture("RecordNotReproduced");
			// The Booster moves one unit along X per fixed step while A is held or while its own table says so.
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Booster.luau" }, { "source", "local Booster = {}\n"
																												   "function Booster.OnFixedUpdate(self: any, dt: number)\n"
																												   "\tif Input.IsKeyDown(\"A\") or self.Boost then\n"
																												   "\t\tself.Entity.Transform:Translate(vector.create(1, 0, 0))\n"
																												   "\tend\n"
																												   "end\n"
																												   "return Script.Define(\"Booster\", Booster)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Booster" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Booster.luau" } } }) } } } } } });
			CallReplayMethod(fixture, "scene.save");

			// Recorded, the Booster moves once for the tap and on every tick after the evaluation; replayed with its input,
			// it moves once, so the first checkpoint differs. Without its input it never moves, so the input check alone
			// fails and the recording used to be written.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 10 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 2 }, { "type", "key" }, { "key", "A" }, { "state", "tap" } } }) } });
			CallReplayMethod(fixture, "script.eval", Json{ { "context", "play" }, { "entity", "/Booster" }, { "code", "self.Boost = true; return true" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 120 }, { "render", "none" } });
			// The expectation at tick 100 lies after the checkpoint where the verification stops, so it is left unevaluated.
			const Json diverged = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Boosted.replay" }, { "expect", Json::array({ Json{ { "tick", 100 }, { "luau", "true" } } }) } });
			REQUIRE_MESSAGE(diverged.contains("error"), diverged.dump());
			const Json& data = diverged["error"]["data"];
			CHECK(data["errorCode"] == Json("Validation"));
			CHECK(data.dump().find("/path") != std::string::npos);
			const std::string detail = JsonReader(data).ReadMember<std::string>("detail").value_or("");
			CHECK(detail.find("refusing to write 'Assets/Tests/Boosted.replay'") != std::string::npos);
			CHECK(detail.find("replayed with its input, the recording does not reproduce itself") != std::string::npos);
			CHECK(detail.find("checkpoint tick 60") != std::string::npos);
			const std::string hint = JsonReader(data).ReadMember<std::string>("hint").value_or("");
			CHECK(hint.find("script.eval") != std::string::npos);
			CHECK(hint.find("neither recorded nor detected") != std::string::npos);
			// The playback's own outcome, as input.replay reports it: stopped at the checkpoint, with both hashes named.
			REQUIRE_MESSAGE(data.contains("verification"), data.dump());
			const Json& verification = data["verification"];
			CHECK(verification["passed"] == Json(false));
			CHECK(verification["withoutInput"] == Json(false));
			CHECK(verification["checkpointFailed"] == Json(true));
			CHECK(verification["finalTick"] == Json(60));
			const std::string observed = JsonReader(verification).ReadMember<std::string>("stateHash").value_or("");
			const std::string recorded = JsonReader(verification).ReadMember<std::string>("expectedStateHash").value_or("");
			CHECK_FALSE(observed.empty());
			CHECK(observed != recorded);
			CHECK(detail.find(observed) != std::string::npos);
			CHECK(detail.find(recorded) != std::string::npos);
			// The frozen reason, the observed hash first.
			CHECK(detail == std::format("refusing to write 'Assets/Tests/Boosted.replay': replayed with its input, the recording does not reproduce itself: the state hash at "
										"checkpoint tick 60 is {}, recorded {}; nothing was written",
					  observed, recorded));
			// M17 review C-01: the candidate was recorded moments before, so nothing in the error data blames changes since the
			// recording: the stop reason, and the unevaluated expectation's message that repeats it, keep the plain form.
			const std::string stopReason = JsonReader(verification).ReadMember<std::string>("stopReason").value_or("");
			CHECK(stopReason == std::format("the state hash at checkpoint tick 60 is {}, but the recording's is {}", observed, recorded));
			REQUIRE(verification["expect"].size() == 1);
			CHECK(verification["expect"][0]["evaluated"] == Json(false));
			CHECK(verification["expect"][0]["message"] == Json("the replay stopped at tick 60 before this expectation's tick 100: " + stopReason));
			CHECK(data.dump().find(GetReplayDivergenceCauses()) == std::string::npos);
			CHECK_FALSE(data.contains("inputCheck"));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Boosted.replay"));
			// The session is the verification's, paused where it stopped, with lockstep released and no recording.
			Json state = CallReplayMethod(fixture, "play.state");
			CHECK(state["recording"] == Json(false));
			CHECK(state["lockstep"] == Json(false));
			CHECK(state["tick"] == Json(60));

			// An expectation the recording's own input does not satisfy: replayed with its input, the expectation fails.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 2 }, { "type", "key" }, { "key", "A" }, { "state", "down" } } }) } });
			const Json unsatisfied = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Unsatisfied.replay" }, { "expect", Json::array({ Json{ { "tick", 10 }, { "luau", "Scene.FindByName('Booster').Transform.WorldPosition.x > 100" } } }) } });
			REQUIRE_MESSAGE(unsatisfied.contains("error"), unsatisfied.dump());
			const Json& refused = unsatisfied["error"]["data"];
			CHECK(refused["errorCode"] == Json("Validation"));
			const std::string reason = JsonReader(refused).ReadMember<std::string>("detail").value_or("");
			CHECK(reason.find("refusing to write 'Assets/Tests/Unsatisfied.replay'") != std::string::npos);
			CHECK(reason.find("expectation 0 at tick 10 (/expect/0/luau) is false") != std::string::npos);
			REQUIRE_MESSAGE(refused.contains("verification"), refused.dump());
			REQUIRE(refused["verification"]["expect"].size() == 1);
			CHECK(refused["verification"]["expect"][0]["evaluated"] == Json(true));
			CHECK(refused["verification"]["expect"][0]["satisfied"] == Json(false));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Unsatisfied.replay"));
			state = CallReplayMethod(fixture, "play.state");
			CHECK(state["recording"] == Json(false));
			CHECK(state["lockstep"] == Json(false));

			// The same input with an expectation it satisfies reproduces, passes the input check and is written.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 2 }, { "type", "key" }, { "key", "A" }, { "state", "down" } } }) } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Satisfied.replay" }, { "expect", Json::array({ Json{ { "tick", 10 }, { "luau", "Scene.FindByName('Booster').Transform.WorldPosition.x > 3" } } }) } });
			CHECK(saved["events"] == Json(1));
			CHECK(ProjectFileExists(fixture, "Assets/Tests/Satisfied.replay"));

			// A script error that the recording's own input raises: replayed with its input, the playback reproduces every
			// hash, but test.run, the games suite and --feature-test fail a replay that raises one, so Stop refuses it with
			// the script-error reason (nothing is written, and the input check never runs).
			CallReplayMethod(fixture, "play.stop");
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Faulty.luau" }, { "source", "local Faulty = {}\n"
																												  "function Faulty.OnFixedUpdate(self: any, dt: number)\n"
																												  "\tif Input.IsKeyDown(\"B\") then\n"
																												  "\t\terror(\"pressed too hard\")\n"
																												  "\tend\n"
																												  "end\n"
																												  "return Script.Define(\"Faulty\", Faulty)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Faulty" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Faulty.luau" } } }) } } } } } });
			CallReplayMethod(fixture, "scene.save");
			Test::ExpectLog raised(LogLevel::Error, "pressed too hard");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 2 }, { "type", "key" }, { "key", "A" }, { "state", "down" } }, Json{ { "tick", 3 }, { "type", "key" }, { "key", "B" }, { "state", "tap" } } }) } });
			const Json faulted = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Faulted.replay" } });
			REQUIRE_MESSAGE(faulted.contains("error"), faulted.dump());
			const Json& faultData = faulted["error"]["data"];
			CHECK(faultData["errorCode"] == Json("Validation"));
			const std::string fault = JsonReader(faultData).ReadMember<std::string>("detail").value_or("");
			CHECK(fault == "refusing to write 'Assets/Tests/Faulted.replay': replayed with its input, the recording does not reproduce itself: "
						   "the playback raised 1 script error(s); the first: Assets/Scripts/Faulty.luau:4: pressed too hard (tick 3); nothing was written");
			// Every hash and expectation reproduced: the script error alone refuses it.
			REQUIRE_MESSAGE(faultData.contains("verification"), faultData.dump());
			CHECK(faultData["verification"]["passed"] == Json(true));
			CHECK(faultData["verification"]["finalTick"] == Json(20));
			CHECK_FALSE(faultData.contains("inputCheck"));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Faulted.replay"));
			// Raised once while recording and once by the verification.
			CHECK(raised.GetMatchCount() == 2);
			state = CallReplayMethod(fixture, "play.state");
			CHECK(state["recording"] == Json(false));
			CHECK(state["lockstep"] == Json(false));
			CHECK(state["tick"] == Json(20));
		}

		// A1's other refusal reasons (Docs/Decisions/0027-m17-contract.md "Frozen interfaces / A1"): an expectation that raises
		// is named with its error's message, not as false; gameplay that stops the playback early is named with its stop
		// reason; and the script errors are every occurrence of the verification's own session from its start (OnStart's
		// included), the first named as it first appeared, although the stream keeps a repeated error at its latest tick.
		TEST_CASE("ReplayMethods: input.record stop names a raised expectation, an early stop and every script error of its verification")
		{
			Test::AutomationFixture fixture("RecordVerificationReasons");
			SaveInputScene(fixture);
			const Json held = Json::array({ Json{ { "tick", 2 }, { "type", "key" }, { "key", "A" }, { "state", "down" } } });
			const std::string_view hint = "a script.eval or play.waitFor that writes script state (a self field, a module table) during a recording is neither recorded nor "
										  "detected, an expectation must hold when the recording's own input is replayed, and a script that reads the host (Application "
										  "queries, pairs over entity keys) diverges; record again without them";
			const auto refusal = [](std::string_view path, std::string_view reason)
			{
				return std::format("refusing to write '{}': replayed with its input, the recording does not reproduce itself: {}; nothing was written", path, reason);
			};

			// An expectation that raises keeps its error's message (and code), where a false one says "is false".
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", held } });
			Json raised;
			{
				Test::ExpectLog boom(LogLevel::Error, "boom");
				raised = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Raised.replay" }, { "expect", Json::array({ Json{ { "tick", 10 }, { "luau", "error('boom')" } } }) } });
			}
			REQUIRE_MESSAGE(raised.contains("error"), raised.dump());
			const Json& raisedData = raised["error"]["data"];
			CHECK(raisedData["errorCode"] == Json("Validation"));
			REQUIRE_MESSAGE(raisedData.contains("verification"), raisedData.dump());
			REQUIRE(raisedData["verification"]["expect"].size() == 1);
			const Json& expectation = raisedData["verification"]["expect"][0];
			CHECK(expectation["evaluated"] == Json(true));
			CHECK(expectation["satisfied"] == Json(false));
			const std::string message = JsonReader(expectation).ReadMember<std::string>("message").value_or("");
			CHECK(message.find("boom") != std::string::npos);
			CHECK(JsonReader(raisedData).ReadMember<std::string>("detail").value_or("")
				== refusal("Assets/Tests/Raised.replay", std::format("expectation 0 at tick 10 (/expect/0/luau) failed: {}", message)));
			CHECK(JsonReader(raisedData).ReadMember<std::string>("hint").value_or("") == hint);
			CHECK_FALSE(raisedData.contains("inputCheck"));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Raised.replay"));

			// Gameplay that quits during the playback (here the expectation at tick 2 asks to) stops it early: its stop reason.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", held } });
			const Json quit = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Quit.replay" }, { "expect", Json::array({ Json{ { "tick", 2 }, { "luau", "Application.Quit(3); return true" } } }) } });
			REQUIRE_MESSAGE(quit.contains("error"), quit.dump());
			const Json& quitData = quit["error"]["data"];
			CHECK(quitData["errorCode"] == Json("Validation"));
			REQUIRE_MESSAGE(quitData.contains("verification"), quitData.dump());
			const Json& stopped = quitData["verification"];
			CHECK(stopped["passed"] == Json(false));
			CHECK(stopped["checkpointFailed"] == Json(false));
			CHECK(stopped["finalTick"] == Json(2));
			CHECK(stopped["expect"][0]["satisfied"] == Json(true));
			const std::string stopReason = JsonReader(stopped).ReadMember<std::string>("stopReason").value_or("");
			CHECK(stopReason.find("the game quit with exit code 3 at tick 2") != std::string::npos);
			CHECK(JsonReader(quitData).ReadMember<std::string>("detail").value_or("") == refusal("Assets/Tests/Quit.replay", std::format("playback stopped at tick 2: {}", stopReason)));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Quit.replay"));

			// Three instances raise the same error at ticks 3, 4 and 5 while B is held (each instance once: a faulting instance
			// is disabled). The stream keeps the one entry at its latest tick, 5; the reason counts every occurrence and names
			// the first as it appeared.
			CallReplayMethod(fixture, "play.stop");
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Faulty.luau" }, { "source", "local Faulty = { Fields = { At = Field.Number(0) } }\n"
																												  "function Faulty.OnFixedUpdate(self: any, dt: number)\n"
																												  "\tif Input.IsKeyDown(\"B\") and Time.GetTick() >= self.At then\n"
																												  "\t\terror(\"pressed too hard\")\n"
																												  "\tend\n"
																												  "end\n"
																												  "return Script.Define(\"Faulty\", Faulty)\n" } });
			for (const int at : { 3, 4, 5 })
				CallReplayMethod(fixture, "entity.create", Json{ { "name", std::format("Faulty{}", at) }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Faulty.luau" }, { "Fields", Json{ { "At", at } } } } }) } } } } } });
			CallReplayMethod(fixture, "scene.save");
			{
				Test::ExpectLog pressed(LogLevel::Error, "pressed too hard");
				CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
				CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 2 }, { "type", "key" }, { "key", "A" }, { "state", "down" } }, Json{ { "tick", 3 }, { "type", "key" }, { "key", "B" }, { "state", "down" } }, Json{ { "tick", 8 }, { "type", "key" }, { "key", "B" }, { "state", "up" } } }) } });
				const Json repeated = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Repeated.replay" } });
				REQUIRE_MESSAGE(repeated.contains("error"), repeated.dump());
				const Json& repeatedData = repeated["error"]["data"];
				CHECK(repeatedData["errorCode"] == Json("Validation"));
				CHECK(repeatedData["verification"]["passed"] == Json(true));
				CHECK(JsonReader(repeatedData).ReadMember<std::string>("detail").value_or("")
					== refusal("Assets/Tests/Repeated.replay", "the playback raised 3 script error(s); the first: Assets/Scripts/Faulty.luau:4: pressed too hard (tick 3)"));
				CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Repeated.replay"));
				// Three while recording and three by the verification.
				CHECK(pressed.GetMatchCount() == 6);
			}

			// An error of OnStart, raised when the verification's session starts, counts too.
			CallReplayMethod(fixture, "play.stop");
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Starter.luau" }, { "source", "local Starter = {}\n"
																												   "function Starter.OnStart(self: any)\n"
																												   "\terror(\"bad start\")\n"
																												   "end\n"
																												   "return Script.Define(\"Starter\", Starter)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Starter" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Starter.luau" } } }) } } } } } });
			CallReplayMethod(fixture, "scene.save");
			{
				Test::ExpectLog started(LogLevel::Error, "bad start");
				CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
				CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", held } });
				const Json faulted = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Started.replay" } });
				REQUIRE_MESSAGE(faulted.contains("error"), faulted.dump());
				const Json& faultedData = faulted["error"]["data"];
				CHECK(faultedData["errorCode"] == Json("Validation"));
				CHECK(JsonReader(faultedData).ReadMember<std::string>("detail").value_or("")
					== refusal("Assets/Tests/Started.replay", "the playback raised 1 script error(s); the first: Assets/Scripts/Starter.luau:3: bad start (tick 0)"));
				CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Started.replay"));
				CHECK(started.GetMatchCount() == 2);
			}
		}

		// A1's script-error reason names the verification's first occurrence (PlaySession::GetFirstScriptError). The stream
		// moves a repeated error behind the others with a new ID, so when two instances of one behaviour fault around another
		// behaviour's error within one tick, or within OnStart, its first entry is the other error, which the reason used to name.
		TEST_CASE("ReplayMethods: input.record stop names the first script error of its verification although a repeated error follows another in one tick")
		{
			Test::AutomationFixture fixture("RecordFirstScriptError");
			SaveInputScene(fixture);
			const auto refusal = [](std::string_view path, std::string_view reason)
			{
				return std::format("refusing to write '{}': replayed with its input, the recording does not reproduce itself: {}; nothing was written", path, reason);
			};
			const auto writeFault = [&fixture](std::string_view name, std::string_view callback, std::string_view condition, std::string_view message)
			{
				CallReplayMethod(fixture, "script.write", Json{ { "path", std::format("Assets/Scripts/{}.luau", name) }, { "source", std::format("local {0} = {{}}\n"
																																				 "function {0}.{1}(self: any{2})\n"
																																				 "\tif {3} then\n"
																																				 "\t\terror(\"{4}\")\n"
																																				 "\tend\n"
																																				 "end\n"
																																				 "return Script.Define(\"{0}\", {0})\n",
																																		 name, callback, callback == "OnStart" ? "" : ", dt: number", condition, message) } });
			};
			const auto createEntities = [&fixture](std::initializer_list<std::pair<std::string_view, std::string_view>> entities)
			{
				// Root entities keep their creation order, which is the canonical order their callbacks run in.
				for (const auto& [name, script] : entities)
					CallReplayMethod(fixture, "entity.create", Json{ { "name", name }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", std::format("Assets/Scripts/{}.luau", script) } } }) } } } } } });
				CallReplayMethod(fixture, "scene.save");
			};

			// In tick 3, while B is held, Faulty1, Other and Faulty2 fault in that order: the stream ends with Other's entry
			// and then Faulty's, moved behind it at tick 3 with a count of 2.
			writeFault("Faulty", "OnFixedUpdate", "Input.IsKeyDown(\"B\")", "pressed too hard");
			writeFault("Other", "OnFixedUpdate", "Input.IsKeyDown(\"B\")", "other fault");
			createEntities({ { "Faulty1", "Faulty" }, { "Other", "Other" }, { "Faulty2", "Faulty" } });
			{
				Test::ExpectLog pressed(LogLevel::Error, "pressed too hard");
				Test::ExpectLog other(LogLevel::Error, "other fault");
				CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
				CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 2 }, { "type", "key" }, { "key", "A" }, { "state", "down" } }, Json{ { "tick", 3 }, { "type", "key" }, { "key", "B" }, { "state", "tap" } } }) } });
				const Json interleaved = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Interleaved.replay" } });
				REQUIRE_MESSAGE(interleaved.contains("error"), interleaved.dump());
				const Json& data = interleaved["error"]["data"];
				CHECK(data["errorCode"] == Json("Validation"));
				CHECK(data["verification"]["passed"] == Json(true));
				CHECK(JsonReader(data).ReadMember<std::string>("detail").value_or("")
					== refusal("Assets/Tests/Interleaved.replay", "the playback raised 3 script error(s); the first: Assets/Scripts/Faulty.luau:4: pressed too hard (tick 3)"));
				CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Interleaved.replay"));
				// Twice while recording and twice by the verification; Other once in each.
				CHECK(pressed.GetMatchCount() == 4);
				CHECK(other.GetMatchCount() == 2);
			}

			// The same in OnStart, raised when the verification's session starts: Starter1, Second and Starter2, in that order.
			CallReplayMethod(fixture, "play.stop");
			writeFault("Starter", "OnStart", "self ~= nil", "bad start");
			writeFault("Second", "OnStart", "self ~= nil", "second start");
			createEntities({ { "Starter1", "Starter" }, { "Second", "Second" }, { "Starter2", "Starter" } });
			{
				Test::ExpectLog started(LogLevel::Error, "bad start");
				Test::ExpectLog second(LogLevel::Error, "second start");
				CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
				CallReplayMethod(fixture, "play.step", Json{ { "ticks", 20 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 2 }, { "type", "key" }, { "key", "A" }, { "state", "down" } } }) } });
				const Json faulted = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/StartedTwice.replay" } });
				REQUIRE_MESSAGE(faulted.contains("error"), faulted.dump());
				const Json& data = faulted["error"]["data"];
				CHECK(data["errorCode"] == Json("Validation"));
				CHECK(data["verification"]["passed"] == Json(true));
				CHECK(JsonReader(data).ReadMember<std::string>("detail").value_or("")
					== refusal("Assets/Tests/StartedTwice.replay", "the playback raised 3 script error(s); the first: Assets/Scripts/Starter.luau:4: bad start (tick 0)"));
				CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/StartedTwice.replay"));
				CHECK(started.GetMatchCount() == 4);
				CHECK(second.GetMatchCount() == 2);
			}
		}

		// Docs/Decisions/0030-m19-contract.md decision 2 (A-FPRINT; ADR 0027 friction F13): a replay records its start
		// fingerprint, so a playback whose hash differs names what changed since the recording: a serialized component (the
		// look's PostProcess), a material its MeshRenderer references, a script. The first change ends the error's message.
		TEST_CASE("ReplayMethods: input.replay names the component, script and material that changed since the recording")
		{
			Test::AutomationFixture fixture("ReplayFingerprintChanges");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Red.material" }, { "values", Json{ { "Roughness", 0.45 } } } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Look" }, { "components", Json{ { "PostProcess", Json{ { "SsaoRadius", 0.5 } } } } } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Box" }, { "components", Json{ { "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Cube" }, { "Materials", Json::array({ "Assets/Materials/Red.material" }) } } } } } });
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 70 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 3 }, { "type", "key" }, { "key", "A" }, { "state", "down" } } }) } });
			CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Look.replay" } });
			CallReplayMethod(fixture, "play.stop");
			const ReplayDocument recorded = ReadRecordedReplay(fixture.GetEditor(), "Assets/Tests/Look.replay");
			REQUIRE(recorded.Fingerprint.has_value());
			const Json verify = Json{ { "path", "Assets/Tests/Look.replay" }, { "verify", true }, { "strictHash", true } };
			const auto changeStarting = [](const Json& changes, std::string_view prefix)
			{
				return std::ranges::any_of(changes, [prefix](const Json& line)
				{
					return line.is_string() && JsonReader(line).ReadString().value_or("").starts_with(prefix);
				});
			};

			// The look and a material change: the replay fails at its first checkpoint, naming both.
			CallReplayMethod(fixture, "entity.update", Json{ { "entity", "/Look" }, { "components", Json{ { "PostProcess", Json{ { "SsaoRadius", 1.5 } } } } } });
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "asset.setProperties", Json{ { "asset", "Assets/Materials/Red.material" }, { "values", Json{ { "Roughness", 0.9 } } } });
			const Json changed = fixture.Request("input.replay", verify);
			REQUIRE_MESSAGE(changed.contains("error"), changed.dump());
			REQUIRE(changed["error"]["data"].contains("replay"));
			const Json& changes = changed["error"]["data"]["replay"]["changes"];
			CHECK(changeStarting(changes, "component PostProcess of /Look ("));
			CHECK(changeStarting(changes, "Material Assets/Materials/Red.material ("));
			CHECK_FALSE(changeStarting(changes, "Script "));
			const std::string detail = JsonReader(changed["error"]["data"]).ReadMember<std::string>("detail").value_or("");
			CHECK(detail.find("; changed since the recording: ") != std::string::npos);
			CallReplayMethod(fixture, "play.stop");

			// Back to the recorded look, a script change is named.
			CallReplayMethod(fixture, "entity.update", Json{ { "entity", "/Look" }, { "components", Json{ { "PostProcess", Json{ { "SsaoRadius", 0.5 } } } } } });
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/KeyMover.luau" }, { "source", "local KeyMover = {}\n"
																													"function KeyMover.OnFixedUpdate(self: any, dt: number)\n"
																													"\tif Input.IsKeyDown(\"A\") then\n"
																													"\t\tself.Entity.Transform:Translate(vector.create(2, 0, 0))\n"
																													"\tend\n"
																													"end\n"
																													"return Script.Define(\"KeyMover\", KeyMover)\n" } });
			const Json scripted = fixture.Request("input.replay", verify);
			REQUIRE_MESSAGE(scripted.contains("error"), scripted.dump());
			REQUIRE(scripted["error"]["data"].contains("replay"));
			const Json& scriptChanges = scripted["error"]["data"]["replay"]["changes"];
			CHECK(changeStarting(scriptChanges, "Script Assets/Scripts/KeyMover.luau ("));
			CHECK_FALSE(changeStarting(scriptChanges, "component PostProcess"));
			CallReplayMethod(fixture, "play.stop");

			// The input check names nothing: failing is its expected outcome.
			const Json withoutInput = fixture.Request("input.replay", Json{ { "path", "Assets/Tests/Look.replay" }, { "verify", true }, { "strictHash", true }, { "withoutInput", true } });
			REQUIRE_MESSAGE(withoutInput.contains("error"), withoutInput.dump());
			REQUIRE(withoutInput["error"]["data"].contains("replay"));
			CHECK(withoutInput["error"]["data"]["replay"]["changes"].empty());
		}

		// Decision 3 (A-F29): continuePastCheckpoint plays on past every checkpoint whose hash differs to the final tick and
		// compares the final hash too; the replay still fails, counting the failed checkpoints and naming the first. It
		// needs strictHash.
		TEST_CASE("ReplayMethods: input.replay continuePastCheckpoint plays on to the final tick and counts every failed checkpoint")
		{
			Test::AutomationFixture fixture("ReplayContinuePastCheckpoint");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			// A is held from tick 50: checkpoints 60 and 120 and the final hash differ from an input-free run.
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 50 }, { "type", "key" }, { "key", "A" }, { "state", "down" } } }) } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Held.replay" } });
			REQUIRE(saved["checkpoints"] == Json(2));

			const Json response = fixture.Request("input.replay", Json{ { "path", "Assets/Tests/Held.replay" }, { "verify", true }, { "strictHash", true }, { "withoutInput", true }, { "continuePastCheckpoint", true } });
			REQUIRE_MESSAGE(response.contains("error"), response.dump());
			REQUIRE_MESSAGE(response["error"]["data"].contains("replay"), response.dump());
			const Json& outcome = response["error"]["data"]["replay"];
			CHECK(outcome["passed"] == Json(false));
			CHECK(outcome["checkpointFailed"] == Json(true));
			CHECK(outcome["checkpointsChecked"] == Json(2));
			CHECK(outcome["checkpointsFailed"] == Json(2));
			CHECK(outcome["firstFailedCheckpointTick"] == Json(60));
			CHECK(outcome["finalTick"] == Json(130));
			CHECK(outcome["hashMatched"] == Json(false));
			CHECK(outcome["stopReason"] == Json(""));
			CHECK(CallReplayMethod(fixture, "play.state")["tick"] == Json(130));

			const Json noStrict = fixture.Request("input.replay", Json{ { "path", "Assets/Tests/Held.replay" }, { "verify", true }, { "continuePastCheckpoint", true } });
			REQUIRE_MESSAGE(noStrict.contains("error"), noStrict.dump());
			CHECK(noStrict["error"]["data"]["errorCode"] == Json("InvalidArgument"));
			REQUIRE(noStrict["error"]["data"].contains("location"));
			CHECK(noStrict["error"]["data"]["location"]["pointer"] == Json("/continuePastCheckpoint"));
		}

		// Decision 3 (A-F29; ADR 0026 friction F29): a recording whose input shows at its checkpoints but whose end state a
		// reset restores passes the input check and is written, with one warning that its final hash alone does not depend on
		// its input (MakeFinalHashIndependentWarning); the check then ends at the final tick.
		TEST_CASE("ReplayMethods: input.record stop warns when only its final state hash does not depend on its input")
		{
			Test::AutomationFixture fixture("RecordFinalHashWarning");
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Resetter.luau" }, { "source", "local Resetter = {}\n"
																													"function Resetter.OnFixedUpdate(self: any, dt: number)\n"
																													"\tif Input.IsKeyDown(\"A\") then self.Entity.Transform:Translate(vector.create(1, 0, 0)) end\n"
																													"\tif Time.GetTick() == 125 then self.Entity.Transform.Translation = vector.create(0, 0, 0) end\n"
																													"end\n"
																													"return Script.Define(\"Resetter\", Resetter)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Mover" }, { "components", Json{ { "Script", Json{ { "Script", "Assets/Scripts/Resetter.luau" } } } } } });
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 10 }, { "type", "key" }, { "key", "A" }, { "state", "down" } }, Json{ { "tick", 70 }, { "type", "key" }, { "key", "A" }, { "state", "up" } } }) } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Reset.replay" } });
			CHECK(ProjectFileExists(fixture, "Assets/Tests/Reset.replay"));
			CHECK(saved["inputCheckTick"] == Json(60));
			REQUIRE(saved["warnings"].size() == 1);
			CHECK(saved["warnings"][0] == Json(MakeFinalHashIndependentWarning("Assets/Tests/Reset.replay", 60, JsonReader(saved).ReadMember<std::string>("stateHash").value_or(""))));
			// The check played on to the final tick.
			CHECK(CallReplayMethod(fixture, "play.state")["tick"] == Json(130));

			// A recording whose final state shows its input gets no warning.
			CallReplayMethod(fixture, "play.stop");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 100 }, { "type", "key" }, { "key", "B" }, { "state", "down" } } }) } });
			const Json plain = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Plain.replay" } });
			CHECK(plain["warnings"].empty());
		}

		// Decision 4 (AL-EVFREE): a recording without input events is verified with its (empty) input too, so a false
		// expectation refuses it and nothing is written; a satisfied one is written, and the play session is then the
		// verification's, at its final tick.
		TEST_CASE("ReplayMethods: input.record stop verifies a recording without input events before writing it")
		{
			Test::AutomationFixture fixture("RecordEventFreeVerified");
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 70 }, { "render", "none" } });
			const Json refused = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/False.replay" }, { "expect", Json::array({ Json{ { "tick", 30 }, { "luau", "false" } } }) } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			CHECK(refused["error"]["data"]["errorCode"] == Json("Validation"));
			CHECK(refused["error"]["data"].contains("verification"));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/False.replay"));
			CHECK(CallReplayMethod(fixture, "play.state")["recording"] == Json(false));

			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" }, { "restart", true } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 70 }, { "render", "none" } });
			const uint64_t recordedSerial = fixture.GetEditor().GetPlay().GetSession()->GetSerial();
			const Json written = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Pure.replay" }, { "expect", Json::array({ Json{ { "tick", 70 }, { "luau", "Time.GetTick() == 70" } } }) } });
			CHECK(written["events"] == Json(0));
			CHECK(written["inputCheckTick"] == Json(0));
			CHECK(ProjectFileExists(fixture, "Assets/Tests/Pure.replay"));
			REQUIRE(fixture.GetEditor().GetPlay().GetSession() != nullptr);
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetSerial() != recordedSerial);
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetTick() == 70);
		}

		// Decision 5 (AL-TEARDOWN): once the verification's playback reached its final tick, its scripts are stopped and an
		// error their OnDestroy raises refuses the recording like any other script error, named "at teardown"; nothing is
		// written, and the session stays the verification's with its scripts stopped.
		TEST_CASE("ReplayMethods: input.record stop counts the errors its verification's scripts raise at teardown")
		{
			// Declared before the fixture: every session that ran the Leaver raises it when it ends, the fixture's last one too.
			Test::ExpectLog teardown(LogLevel::Error, "teardown fault");
			Test::AutomationFixture fixture("RecordTeardownError");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Leaver.luau" }, { "source", "local Leaver = {}\n"
																												  "function Leaver.OnDestroy(self: any)\n"
																												  "\terror(\"teardown fault\")\n"
																												  "end\n"
																												  "return Script.Define(\"Leaver\", Leaver)\n" } });
			CallReplayMethod(fixture, "entity.create", Json{ { "name", "Leaver" }, { "components", Json{ { "Script", Json{ { "Script", "Assets/Scripts/Leaver.luau" } } } } } });
			CallReplayMethod(fixture, "scene.save");
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 70 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 3 }, { "type", "key" }, { "key", "A" }, { "state", "down" } } }) } });
			const Json refused = fixture.Request("input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Teardown.replay" } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			const Json& data = refused["error"]["data"];
			CHECK(data["errorCode"] == Json("Validation"));
			const std::string detail = JsonReader(data).ReadMember<std::string>("detail").value_or("");
			CHECK(detail.find("teardown fault") != std::string::npos);
			CHECK(detail.find("at teardown") != std::string::npos);
			REQUIRE(data.contains("verification"));
			CHECK(data["verification"]["passed"] == Json(true));
			CHECK_FALSE(ProjectFileExists(fixture, "Assets/Tests/Teardown.replay"));
			const PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			CHECK(session->GetTick() == 70);
			CHECK(fixture.Request("script.eval", Json{ { "code", "return 1" }, { "context", "play" } }).contains("error"));
		}

		// Decision 14 (A-RREAD): input.readReplay returns a replay's header as its file writes it, its events in play.step's
		// input form with absolute ticks, a page at a time, its expectations and checkpoints; play.step with those events
		// from tick 0 of the header's session reproduces the recording's final hash.
		TEST_CASE("ReplayMethods: input.readReplay reads a replay's header, events, expectations and checkpoints back")
		{
			Test::AutomationFixture fixture("ReadReplay");
			SaveInputScene(fixture);
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" }, { "input", Json::array({ Json{ { "tick", 100 }, { "type", "key" }, { "key", "A" }, { "state", "down" } }, Json{ { "tick", 110 }, { "type", "key" }, { "key", "A" }, { "state", "up" } } }) } });
			const Json saved = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Read.replay" }, { "expect", Json::array({ Json{ { "tick", 130 }, { "luau", "Time.GetTick() == 130" } } }) } });
			CallReplayMethod(fixture, "play.stop");

			const Json read = CallReplayMethod(fixture, "input.readReplay", Json{ { "path", "Assets/Tests/Read.replay" } });
			CHECK(read["path"] == Json("Assets/Tests/Read.replay"));
			CHECK(read["formatVersion"] == Json(ReplayFormatVersion));
			REQUIRE(read["header"].contains("Scene"));
			CHECK(read["header"]["Scene"]["Path"] == Json("Assets/Scenes/Main.scene"));
			CHECK(read["header"].contains("Seed"));
			CHECK(read["finalTick"] == Json(130));
			CHECK(read["finalStateHash"] == saved["stateHash"]);
			CHECK(read["eventCount"] == Json(2));
			REQUIRE(read["events"].size() == 2);
			const Json& pressed = read["events"][0];
			CHECK(pressed.size() == 4);
			CHECK(pressed["tick"] == Json(100));
			CHECK(pressed["type"] == Json("Key"));
			CHECK(pressed["key"] == Json("A"));
			CHECK(pressed["state"] == Json("Down"));
			CHECK(read["nextCursor"] == Json(""));
			REQUIRE(read["expect"].size() == 1);
			CHECK(read["expect"][0]["tick"] == Json(130));
			CHECK(read["expect"][0]["luau"] == Json("Time.GetTick() == 130"));
			CHECK(read["checkpoints"].size() == 2);
			CHECK(read["hasFingerprint"] == Json(true));

			// One event per page.
			const Json first = CallReplayMethod(fixture, "input.readReplay", Json{ { "path", "Assets/Tests/Read.replay" }, { "limit", 1 } });
			REQUIRE(first["events"].size() == 1);
			const std::string cursor = JsonReader(first).ReadMember<std::string>("nextCursor").value_or("");
			REQUIRE_FALSE(cursor.empty());
			const Json second = CallReplayMethod(fixture, "input.readReplay", Json{ { "path", "Assets/Tests/Read.replay" }, { "limit", 1 }, { "cursor", cursor } });
			REQUIRE(second["events"].size() == 1);
			CHECK(second["events"][0] == read["events"][1]);
			CHECK(second["nextCursor"] == Json(""));

			// The events replay through play.step from tick 0: a re-recording reproduces the final hash.
			CallReplayMethod(fixture, "input.record", Json{ { "action", "start" } });
			CallReplayMethod(fixture, "play.step", Json{ { "ticks", 130 }, { "render", "none" }, { "input", read["events"] } });
			const Json again = CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Again.replay" }, { "expect", read["expect"] } });
			CHECK(again["stateHash"] == saved["stateHash"]);
		}
	}

}
