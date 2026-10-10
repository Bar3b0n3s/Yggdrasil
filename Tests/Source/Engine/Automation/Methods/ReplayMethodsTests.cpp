#include "TestsPCH.h"
#include "Engine/Automation/Methods/ReplayMethods.h"

#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Events.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

#include <chrono>

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
			CallReplayMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", "Assets/Tests/Failures.replay" }, { "expect", Json::array({ Json{ { "tick", 0 }, { "luau", "false" } }, Json{ { "tick", 0 }, { "luau", "local x = 1\nerror('replay fault')" } }, Json{ { "tick", 0 }, { "luau", "42" } } }) } });
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
	}

}
