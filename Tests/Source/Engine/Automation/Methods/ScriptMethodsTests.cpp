#include "TestsPCH.h"

#include "Engine/Automation/Methods/ScriptMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		Json CallScriptMethod(Test::AutomationFixture& fixture, std::string_view method, const Json& params)
		{
			auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ScriptMethods: edit evaluation returns a detached value and captured prints" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptEvalValue");
			const Json result = CallScriptMethod(fixture, "script.eval",
				Json{ { "context", "edit" }, { "code", "print('first'); print('second'); return { answer = 42, ready = true }" } });
			CHECK(result["value"] == Json({ { "answer", 42 }, { "ready", true } }));
			CHECK(result["prints"] == Json::array({ "first", "second" }));
		}

		TEST_CASE("ScriptMethods: edit evaluation refuses host writes before changing the scene" * doctest::skip(true))
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
			CHECK(after["components"] == before["components"]);
			CHECK(fixture.Call("entity.get", Json{ { "entity", "/Before" } }).has_value());
			CHECK_FALSE(fixture.Call("entity.get", Json{ { "entity", "/After" } }).has_value());
		}

		TEST_CASE("ScriptMethods: eval requires code and context and rejects unknown enum values" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptEvalParams");
			for (const Json& params : { Json{ { "code", "return 1" } }, Json{ { "context", "edit" } },
					 Json{ { "code", "return 1" }, { "context", "native" } } })
			{
				const Json response = fixture.Request("script.eval", params);
				CHECK(response["error"]["code"] == Json(-32602));
			}
		}

		TEST_CASE("ScriptMethods: play evaluation requires a live scripting session" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptEvalPlayState");
			const Json response = fixture.Request("script.eval", Json{ { "context", "play" }, { "code", "return 1" } });
			CHECK(response["error"]["data"]["errorCode"] == Json("InvalidState"));
		}

		TEST_CASE("ScriptMethods: eval binds self to the selected behaviour instance" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptEvalSelf");
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/State.luau" }, { "source", "local State = {}; function State.OnCreate(self: any) self.Score = 7 end; return Script.Define('State', State)" } });
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Game" }, { "components", Json{ { "Script", Json{ { "Script", "Assets/Scripts/State.luau" } } } } } });
			CallScriptMethod(fixture, "play.start", Json{ { "mode", "play" }, { "lockstep", true } });
			const Json result = CallScriptMethod(fixture, "script.eval",
				Json{ { "context", "play" }, { "entity", "/Game" }, { "code", "return self.Score" } });
			CHECK(result["value"] == Json(7));
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		TEST_CASE("ScriptMethods: errors expose locations and tracebacks and advance an exclusive cursor" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptErrorCursor");
			const Json start = CallScriptMethod(fixture, "script.errors", Json{ { "since", "end" } });
			REQUIRE(start["errors"].empty());
			const std::string cursor = JsonReader(start["nextCursor"]).ReadString().value_or(std::string());
			{
				Test::ExpectLog expected(LogLevel::Error, "contract error");
				const Json response = fixture.Request("script.eval",
					Json{ { "context", "edit" }, { "code", "local function fail() error('contract error') end\nfail()" } });
				CHECK(response.contains("error"));
			}
			const Json errors = CallScriptMethod(fixture, "script.errors", Json{ { "since", cursor }, { "limit", 1 } });
			REQUIRE(errors["errors"].size() == 1);
			const Json& error = errors["errors"][0];
			CHECK(error["kind"] == Json("runtime"));
			CHECK(error["line"] != Json(0));
			CHECK_FALSE(error["traceback"].empty());
			CHECK(error["count"] == Json(1));
			const Json next = CallScriptMethod(fixture, "script.errors", Json{ { "since", errors["nextCursor"] } });
			CHECK(next["errors"].empty());
		}

		TEST_CASE("ScriptMethods: errors preserve distinct embedded expectation pointers across RPC cursor pages" * doctest::skip(true))
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
			CHECK(first["errors"][0]["message"] == second["errors"][0]["message"]);
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

		TEST_CASE("ScriptMethods: errors rejects malformed cursors and limits with located errors" * doctest::skip(true))
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

		TEST_CASE("ScriptMethods: eval refuses dry runs and atomic batches before execution" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("ScriptEvalFlags");
			const Json params{ { "context", "edit" }, { "code", "return 1" }, { "dryRun", true } };
			CHECK(fixture.Request("script.eval", params)["error"]["data"]["errorCode"] == Json("Unsupported"));
			const Json batch = fixture.Request("edit.batch", Json{ { "ops", Json::array({ Json{ { "method", "script.eval" }, { "params", Json{ { "context", "edit" }, { "code", "return 1" } } } } }) } });
			CHECK(batch.contains("error"));
		}
	}

}
