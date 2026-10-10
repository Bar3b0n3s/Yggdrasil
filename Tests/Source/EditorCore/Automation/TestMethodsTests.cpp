#include "TestsPCH.h"

#include "EditorCore/Automation/TestMethods.h"
#include "EditorCore/Play/EditorPlayController.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
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
				REQUIRE(editor.GetPlay().IsTestRunActive());
			}
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
