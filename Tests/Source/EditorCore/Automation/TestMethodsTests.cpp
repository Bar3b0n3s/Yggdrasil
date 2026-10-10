#include "TestsPCH.h"

#include "EditorCore/Automation/TestMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		Json CallTestMethod(Test::AutomationFixture& fixture, std::string_view method, const Json& params)
		{
			auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		void WriteSuite(Test::AutomationFixture& fixture, std::string_view source)
		{
			CallTestMethod(fixture, "script.write", Json{ { "path", "Assets/Tests/Contract.test.luau" }, { "source", source } });
			CallTestMethod(fixture, "project.setSettings", Json{ { "patch", Json{ { "Testing", Json{ { "Suites", Json::array({ Json{ { "Script", "Assets/Tests/Contract.test.luau" }, { "Scene", "" }, { "Modes", Json::array({ "Editor" }) } } }) } } } } } });
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("TestMethods: list collects case names and locations without executing bodies" * doctest::skip(true))
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

		TEST_CASE("TestMethods: run filters suite and case names case-insensitively" * doctest::skip(true))
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

		TEST_CASE("TestMethods: failed expectations return structured locations and continue the case" * doctest::skip(true))
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

		TEST_CASE("TestMethods: case timeout reports its wait location and permits the next case" * doctest::skip(true))
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

		TEST_CASE("TestMethods: recording requires exactly one selected suite before writing any replay" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("TestRecordSelection");
			WriteSuite(fixture, "return Test.Suite('Only', function() Test.Case('pass', function() end) end)");
			const Json response = fixture.Request("test.run", Json{ { "filter", "does not match" }, { "record", "Assets/Tests/Replays/Missing.replay" } });
			CHECK(response["error"]["data"]["errorCode"] == Json("Validation"));
			CHECK_FALSE(fixture.Call("asset.info", Json{ { "asset", "Assets/Tests/Replays/Missing.replay" } }).has_value());
		}

		TEST_CASE("TestMethods: a recorded suite writes a replay that verifies without its test body" * doctest::skip(true))
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

		TEST_CASE("TestMethods: run rejects dry runs and invalid limits without starting a suite" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("TestRunParams");
			CHECK(fixture.Request("test.run", Json{ { "dryRun", true } })["error"]["data"]["errorCode"] == Json("Unsupported"));
			CHECK(fixture.Request("test.run", Json{ { "timeoutTicks", 0 } })["error"]["code"] == Json(-32602));
			CHECK(fixture.Request("test.run", Json{ { "record", "../outside.replay" } })["error"]["code"] == Json(-32602));
		}
	}

}
