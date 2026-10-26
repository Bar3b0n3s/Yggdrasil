#include "TestsPCH.h"

#include "Engine/Automation/Methods/ScriptMethods.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <string>

namespace Engine {

	namespace {

		Json CallScriptMethod(Test::AutomationFixture& fixture, std::string_view method, const Json& params)
		{
			auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			return std::move(*result);
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ScriptMethods: host error cursors survive play session replacement")
		{
			Test::AutomationFixture fixture("ScriptHostHistory");
			Json previous = "0";
			for (int occurrence = 1; occurrence <= 2; ++occurrence)
			{
				CallScriptMethod(fixture, "play.start", Json{ { "paused", true } });
				{
					Test::ExpectLog expected(LogLevel::Error, "persistent host fault");
					CHECK(fixture.Request("script.eval", Json{ { "context", "play" }, { "code", "error('persistent host fault', 0)" } }).contains("error"));
				}
				CallScriptMethod(fixture, "play.stop", Json::object());
				const Json errors = CallScriptMethod(fixture, "script.errors", Json{ { "since", previous } });
				REQUIRE(errors["errors"].size() == 1);
				CHECK(errors["errors"][0]["count"] == Json(occurrence));
				CHECK(errors["nextCursor"] != previous);
				previous = errors["nextCursor"];
			}
		}

		TEST_CASE("ScriptMethods: edit evaluation returns a detached value and captured prints")
		{
			Test::AutomationFixture fixture("ScriptEvalValue");
			const Json result = CallScriptMethod(fixture, "script.eval",
				Json{ { "context", "edit" }, { "code", "print('first'); print('second'); return { answer = 42, ready = true }" } });
			CHECK(result["value"] == Json({ { "answer", 42 }, { "ready", true } }));
			CHECK(result["prints"] == Json::array({ "first", "second" }));
		}

		TEST_CASE("ScriptMethods: edit evaluation refuses host writes before changing the scene")
		{
			Test::AutomationFixture fixture("ScriptEvalReadOnly");
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Before" } });
			const Json before = CallScriptMethod(fixture, "entity.get", Json{ { "entity", "/Before" } });
			{
				Test::ExpectLog expected(LogLevel::Error, "read-only");
				const Json response = fixture.Request("script.eval", Json{ { "context", "edit" }, { "code", "Scene.FindByName('Before').Name = 'After'" } });
				CHECK(response["error"]["data"]["errorCode"] == Json("Script"));
			}
			const Json after = CallScriptMethod(fixture, "entity.get", Json{ { "entity", "/Before" } });
			CHECK(after["entity"] == before["entity"]);
			CHECK(after["entity"]["components"] == before["entity"]["components"]);
			CHECK(fixture.Call("entity.get", Json{ { "entity", "/Before" } }).has_value());
			CHECK_FALSE(fixture.Call("entity.get", Json{ { "entity", "/After" } }).has_value());
		}

		TEST_CASE("ScriptMethods: eval requires code and context and rejects unknown enum values")
		{
			Test::AutomationFixture fixture("ScriptEvalParams");
			for (const Json& params : { Json{ { "code", "return 1" } }, Json{ { "context", "edit" } },
					 Json{ { "code", "return 1" }, { "context", "native" } } })
			{
				const Json response = fixture.Request("script.eval", params);
				CHECK(response["error"]["code"] == Json(-32602));
			}
		}

		TEST_CASE("ScriptMethods: play evaluation requires a live scripting session")
		{
			Test::AutomationFixture fixture("ScriptEvalPlayState");
			const Json response = fixture.Request("script.eval", Json{ { "context", "play" }, { "code", "return 1" } });
			CHECK(response["error"]["data"]["errorCode"] == Json("InvalidState"));
		}

		TEST_CASE("ScriptMethods: eval binds self to the selected behaviour instance")
		{
			Test::AutomationFixture fixture("ScriptEvalSelf");
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/State.luau" }, { "source", "local State = {}; function State.OnCreate(self: any) self.Score = 7 end; return Script.Define('State', State)" } });
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Game" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/State.luau" } } }) } } } } } });
			CallScriptMethod(fixture, "play.start", Json{ { "mode", "play" }, { "lockstep", true } });
			const Json result = CallScriptMethod(fixture, "script.eval",
				Json{ { "context", "play" }, { "entity", "/Game" }, { "code", "return self.Score" } });
			CHECK(result["value"] == Json(7));
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		// Docs/Decisions/0031-m20-contract.md decision 18: script names the behaviour whose slot self binds to, in both
		// contexts; a name no slot of the entity has is NotFound at /script before anything runs, and script needs entity.
		// The entities with several slots are ScriptSlotTests' and test_script_slots.py's.
		TEST_CASE("ScriptMethods: eval binds self to the named behaviour's slot and locates a name no slot has")
		{
			Test::AutomationFixture fixture("ScriptEvalBehaviour");
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/State.luau" }, { "source", "local State = {}; function State.OnCreate(self: any) self.Score = 7 end; return Script.Define('State', State)" } });
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Game" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/State.luau" } } }) } } } } } });
			const auto checkRefusals = [&fixture](std::string_view context)
			{
				CAPTURE(context);
				const Json missing = fixture.Request("script.eval", Json{ { "context", context }, { "entity", "/Game" }, { "script", "Missing" }, { "code", "Scene.CreateEntity('Ran') return 1" } });
				CHECK(missing["error"]["data"]["errorCode"] == Json("NotFound"));
				CHECK(missing["error"]["data"]["issues"][0]["pointer"] == Json("/script"));
				// An entity that does not resolve stays located at /entity.
				const Json noEntity = fixture.Request("script.eval", Json{ { "context", context }, { "entity", "/Nobody" }, { "script", "State" }, { "code", "return 1" } });
				CHECK(noEntity["error"]["data"]["errorCode"] == Json("NotFound"));
				CHECK(noEntity["error"]["data"]["issues"][0]["pointer"] == Json("/entity"));
				for (const Json& params : { Json{ { "context", context }, { "script", "State" }, { "code", "return 1" } },
						 Json{ { "context", context }, { "entity", "/Game" }, { "script", "" }, { "code", "return 1" } } })
				{
					const Json refused = fixture.Request("script.eval", params);
					CHECK(refused["error"]["data"]["errorCode"] == Json("InvalidArgument"));
					CHECK(refused["error"]["data"]["issues"][0]["pointer"] == Json("/script"));
				}
			};
			// Edit: the read-only VM builds the slot's table from its fields without running OnCreate.
			CHECK(CallScriptMethod(fixture, "script.eval", Json{ { "context", "edit" }, { "entity", "/Game" }, { "script", "State" }, { "code", "return self ~= nil and self.Entity.Name" } })["value"] == Json("Game"));
			checkRefusals("edit");
			CallScriptMethod(fixture, "play.start", Json{ { "mode", "play" }, { "lockstep", true } });
			CHECK(CallScriptMethod(fixture, "script.eval", Json{ { "context", "play" }, { "entity", "/Game" }, { "script", "State" }, { "code", "return self.Score" } })["value"] == Json(7));
			checkRefusals("play");
			CHECK(CallScriptMethod(fixture, "scene.query", Json{ { "where", Json{ { "name", "Ran" } } }, { "target", "play" } })["total"] == Json(0));
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		TEST_CASE("ScriptMethods: errors expose locations and tracebacks and advance an exclusive cursor")
		{
			Test::AutomationFixture fixture("ScriptErrorCursor");
			const auto eventCursor = fixture.GetEditor().GetEngine().GetEventLog().GetNextSeq();
			const Json start = CallScriptMethod(fixture, "script.errors", Json{ { "since", "end" } });
			REQUIRE(start["errors"].empty());
			const std::string cursor = JsonReader(start["nextCursor"]).ReadString().value_or(std::string());
			{
				Test::ExpectLog expected(LogLevel::Error, "contract error");
				const Json response = fixture.Request("script.eval",
					Json{ { "context", "edit" }, { "code", "local function fail() error('contract error') end\nfail()" } });
				CHECK(response.contains("error"));
				CHECK(expected.GetMatchCount() == 1);
			}
			const Json errors = CallScriptMethod(fixture, "script.errors", Json{ { "since", cursor }, { "limit", 1 } });
			REQUIRE(errors["errors"].size() == 1);
			const Json& error = errors["errors"][0];
			CHECK(error["kind"] == Json("runtime"));
			CHECK(error["line"] != Json(0));
			CHECK_FALSE(error["traceback"].empty());
			CHECK(error["count"] == Json(1));
			const auto raised = fixture.GetEditor().GetEngine().GetEventLog().Read(eventCursor, std::array{ EngineEventType::ScriptErrorRaised });
			REQUIRE(raised.Events.size() == 1);
			CHECK(Json(raised.Events[0].Message) == error["message"]);
			const Json next = CallScriptMethod(fixture, "script.errors", Json{ { "since", errors["nextCursor"] } });
			CHECK(next["errors"].empty());
		}

		// The entity summary's path is "the entity's path from the scene root", which script.errors left empty. The stream
		// outlives the play session, so the path is read when the errors are: from the play scene while playing and from the
		// edit scene after play, and it is empty once the scene that reads address no longer holds the entity.
		TEST_CASE("ScriptMethods: errors give the entity's path in the scene that reads address while it holds the entity")
		{
			Test::AutomationFixture fixture("ScriptErrorEntityPath");
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Faulty.luau" }, { "source", "local Faulty = {}\n"
																												  "function Faulty.OnStart(self: any)\n"
																												  "\terror('board fault', 0)\n"
																												  "end\n"
																												  "return Script.Define('Faulty', Faulty)\n" } });
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Game" } });
			const Json board = CallScriptMethod(fixture, "entity.create", Json{ { "name", "Board" }, { "parent", "/Game" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Faulty.luau" } } }) } } } } } });
			const Json id = board["entity"]["id"];
			const auto readEntity = [&fixture]()
			{
				const Json errors = CallScriptMethod(fixture, "script.errors", Json::object());
				REQUIRE(errors["errors"].size() == 1);
				return errors["errors"][0]["entity"];
			};

			{
				// OnStart runs as the session starts.
				Test::ExpectLog expected(LogLevel::Error, "board fault");
				CallScriptMethod(fixture, "play.start", Json{ { "mode", "play" }, { "lockstep", true } });
				CHECK(expected.GetMatchCount() == 1);
			}
			CHECK(readEntity() == Json{ { "id", id }, { "name", "Board" }, { "path", "/Game/Board" } });
			// The entry names the faulting behaviour's slot (Docs/Decisions/0031-m20-contract.md decision 17).
			CHECK(CallScriptMethod(fixture, "script.errors", Json::object())["errors"][0]["slot"] == Json(0));
			// Destroyed in the play scene: the id and name stay, the path is empty.
			CallScriptMethod(fixture, "entity.destroy", Json{ { "entities", Json::array({ "/Game/Board" }) }, { "target", "play" } });
			CHECK(readEntity() == Json{ { "id", id }, { "name", "Board" }, { "path", "" } });
			// After play the edit scene, which still holds it, gives the path; destroyed there too, it is empty again.
			CallScriptMethod(fixture, "play.stop", Json::object());
			CHECK(readEntity()["path"] == Json("/Game/Board"));
			CallScriptMethod(fixture, "entity.destroy", Json{ { "entities", Json::array({ "/Game/Board" }) } });
			CHECK(readEntity() == Json{ { "id", id }, { "name", "Board" }, { "path", "" } });
		}

		TEST_CASE("ScriptMethods: errors preserve distinct embedded expectation pointers across RPC cursor pages")
		{
			Test::AutomationFixture fixture("ScriptEmbeddedErrorCursor");
			CallScriptMethod(fixture, "input.record", Json{ { "action", "start" } });
			const Json expectation{ { "tick", 0 }, { "luau", "return error('embedded expectation error', 0)" } };
			const std::string replayPath = "Assets/Tests/EmbeddedErrors.replay";
			CallScriptMethod(fixture, "input.record",
				Json{ { "action", "stop" }, { "path", replayPath }, { "expect", Json::array({ expectation, expectation }) } });
			const Json start = CallScriptMethod(fixture, "script.errors", Json{ { "since", "end" } });
			REQUIRE(start["errors"].empty());
			{
				Test::ExpectLog expected(LogLevel::Error, "embedded expectation error");
				const Json response = fixture.Request("input.replay", Json{ { "path", replayPath }, { "verify", true } });
				CHECK(response["error"]["data"]["errorCode"] == Json("Validation"));
			}
			const Json first = CallScriptMethod(fixture, "script.errors", Json{ { "since", start["nextCursor"] }, { "limit", 1 } });
			REQUIRE(first["errors"].size() == 1);
			const Json second = CallScriptMethod(fixture, "script.errors", Json{ { "since", first["nextCursor"] }, { "limit", 1 } });
			REQUIRE(second["errors"].size() == 1);
			CHECK(first["errors"][0]["jsonPointer"] == Json("/Expect/0/Luau"));
			CHECK(second["errors"][0]["jsonPointer"] == Json("/Expect/1/Luau"));
			CHECK(first["errors"][0]["message"] == Json("/Expect/0/Luau: embedded expectation error"));
			CHECK(second["errors"][0]["message"] == Json("/Expect/1/Luau: embedded expectation error"));
			CHECK(first["nextCursor"] != second["nextCursor"]);
			for (const Json& page : { first, second })
			{
				CHECK(page["errors"][0]["script"] == Json(replayPath));
				CHECK(page["errors"][0]["line"] == Json(1));
				CHECK(page["errors"][0]["count"] == Json(1));
			}
			const Json after = CallScriptMethod(fixture, "script.errors", Json{ { "since", second["nextCursor"] } });
			CHECK(after["errors"].empty());
		}

		TEST_CASE("ScriptMethods: errors rejects malformed cursors and limits with located errors")
		{
			Test::AutomationFixture fixture("ScriptErrorsInvalid");
			for (const Json& params : { Json{ { "since", "-1" } }, Json{ { "since", "18446744073709551616" } },
					 Json{ { "limit", 0 } }, Json{ { "limit", 1001 } } })
			{
				const Json response = fixture.Request("script.errors", params);
				CHECK(response["error"]["code"] == Json(-32602));
				CHECK_FALSE(response["error"]["data"]["issues"].empty());
			}
		}

		TEST_CASE("ScriptMethods: eval refuses dry runs and atomic batches before execution")
		{
			Test::AutomationFixture fixture("ScriptEvalFlags");
			const Json params{ { "context", "edit" }, { "code", "return 1" }, { "dryRun", true } };
			CHECK(fixture.Request("script.eval", params)["error"]["data"]["errorCode"] == Json("Unsupported"));
			const Json batch = fixture.Request("edit.batch", Json{ { "ops", Json::array({ Json{ { "method", "script.eval" }, { "params", Json{ { "context", "edit" }, { "code", "return 1" } } } } }) } });
			CHECK(batch.contains("error"));
		}

		TEST_CASE("ScriptMethods: errors keep bounded messages and report evictions to older cursors")
		{
			Test::AutomationFixture fixture("ScriptErrorBounds");
			const std::string marker = " [truncated: 20000 bytes]";
			{
				Test::ExpectLog expected(LogLevel::Error, marker);
				const Json response = fixture.Request("script.eval", Json{ { "context", "edit" }, { "code", "error(string.rep('x', 20000), 0)" } });
				REQUIRE(response.contains("error"));
				const Json& data = response["error"]["data"];
				CHECK(data["errorCode"] == Json("Script"));
				REQUIRE_MESSAGE(data.contains("scriptError"), data.dump().substr(0, 512));
				CHECK(data["scriptError"]["message"] == Json(std::string(MaxScriptLogMessageBytes - marker.size(), 'x') + marker));
				CHECK(expected.GetMatchCount() == 1);
			}
			const Json first = CallScriptMethod(fixture, "script.errors", Json::object());
			REQUIRE(first["errors"].size() == 1);
			CHECK(first["errors"][0]["message"] == Json(std::string(MaxScriptLogMessageBytes - marker.size(), 'x') + marker));
			CHECK(first["evicted"] == Json(false));
			// The editor's history keeps the newest distinct errors: one more than it retains evicts the evaluation's error.
			ScriptErrorStream& history = fixture.GetEditor().GetPlay().GetScriptErrors();
			for (size_t index = 0; index < MaxScriptErrorEntries; ++index)
				static_cast<void>(history.Add({ .Script = "Assets/Scripts/Spam.luau", .Line = 1, .Message = std::format("fault {}", index) }));
			CHECK(history.GetErrors().size() == MaxScriptErrorEntries);
			CHECK(history.GetEvictedCount() == 1);
			// Pages stay small enough to be answered inline.
			const Json oldest = CallScriptMethod(fixture, "script.errors", Json{ { "limit", 1 } });
			CHECK(oldest["evicted"] == Json(true));
			REQUIRE(oldest["errors"].size() == 1);
			CHECK(oldest["errors"][0]["message"] == Json("fault 0"));
			// A reader that already saw the evicted error missed nothing; its next page starts at the oldest retained one.
			const Json seen = CallScriptMethod(fixture, "script.errors", Json{ { "since", first["nextCursor"] }, { "limit", 1 } });
			CHECK(seen["evicted"] == Json(false));
			REQUIRE(seen["errors"].size() == 1);
			CHECK(seen["errors"][0]["message"] == Json("fault 0"));
			CHECK(seen["nextCursor"] == oldest["nextCursor"]);
			const Json rest = CallScriptMethod(fixture, "script.errors", Json{ { "since", std::to_string(history.GetCursor()) } });
			CHECK(rest["evicted"] == Json(false));
			CHECK(rest["errors"].empty());
			CHECK(CallScriptMethod(fixture, "script.errors", Json{ { "since", "end" } })["evicted"] == Json(false));
		}
	}

}
