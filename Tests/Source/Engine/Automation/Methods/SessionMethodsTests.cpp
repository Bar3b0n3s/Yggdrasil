#include "TestsPCH.h"

#include "Engine/Automation/Methods/SessionMethods.h"

#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <format>
#include <string>
#include <string_view>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("SessionMethods: session.hello reports the client id and the capabilities")
		{
			Test::EditorTestFixture fixture("SessionHello");
			Test::AutomationTestClient client(fixture.GetEditor());
			Result<Json> hello = client.Call("session.hello",
				Json{ { "token", "ignored-in-process" }, { "protocolVersion", "1.0" }, { "client", Json{ { "name", "engine-tests" }, { "version", "1" } } } });
			REQUIRE_MESSAGE(hello.has_value(), hello.error().ToString());
			CHECK((*hello)["protocolVersion"] == Json("1.0"));
			CHECK((*hello)["engineVersion"] == Json(std::string(EngineVersionString)));
			CHECK((*hello)["clientId"] == Json(client.GetClient()));
			CHECK((*hello)["capabilities"].dump().contains("dryRun"));
			CHECK((*hello)["capabilities"].dump().contains("testHooks"));
			CHECK((*hello)["project"]["open"] == Json(false));
		}

		TEST_CASE("SessionMethods: session.info reports versions, project, renderer and clients")
		{
			Test::AutomationFixture setup("SessionInfo");
			Result<Json> info = setup.Call("session.info", Json::object());
			REQUIRE(info.has_value());
			CHECK((*info)["pid"] == Json(Process::GetCurrentId()));
			CHECK((*info)["project"]["open"] == Json(true));
			CHECK((*info)["project"]["name"] == Json("TestProject"));
			CHECK((*info)["playState"] == Json("Edit"));
			CHECK((*info)["renderer"] == Json("none"));
			CHECK((*info)["lockstepOwner"] == Json(""));
			CHECK((*info)["readOnly"] == Json(false));
			CHECK((*info)["headless"] == Json(true));
			REQUIRE((*info)["clients"].size() == 1);
			CHECK((*info)["clients"][0]["name"] == Json("test"));
			CHECK((*info)["clients"][0]["inProcess"] == Json(true));
			CHECK((*info)["stopped"] == Json(false));
			CHECK((*info)["stopError"].is_null());
		}

		TEST_CASE("SessionMethods: session.info reports a fatal stop as play.state does")
		{
			// §11.1: a second soft memory breach stops the session; session.info and play.state report the same error.
			Test::AutomationFixture setup("SessionInfoFatalStop");
			const std::string leak = "local Leak = {}\nfunction Leak:OnFixedUpdate()\n\tlocal retained = string.rep(\"x\", 2 * 1024 * 1024)\n\twhile true do retained = retained end\nend\nreturn Script.Define(\"Leak\", Leak)\n";
			for (const std::string_view name : { "First", "Second" })
			{
				const std::string path = std::format("Assets/Scripts/Leak{}.luau", name);
				REQUIRE(setup.Call("script.write", Json{ { "path", path }, { "source", leak } }).has_value());
				REQUIRE(setup.Call("entity.create", Json{ { "name", name }, { "components", Json{ { "Script", Json{ { "Script", path } } } } } }).has_value());
			}
			REQUIRE(setup.Call("project.setSettings", Json{ { "patch", Json{ { "Scripting", Json{ { "MemoryLimitMB", 1 }, { "PauseOnError", false } } } } } }).has_value());
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Result<Json> running = setup.Call("session.info", Json::object());
			REQUIRE(running.has_value());
			CHECK((*running)["playState"] == Json("Play"));
			CHECK((*running)["stopped"] == Json(false));
			CHECK((*running)["stopError"].is_null());
			{
				Test::ExpectLog breach(LogLevel::Error, "memory limit");
				const Result<Json> stepped = setup.Call("play.step", Json{ { "ticks", 1 }, { "render", "none" } });
				REQUIRE_MESSAGE(stepped.has_value(), stepped.error().ToString());
			}
			const Result<Json> info = setup.Call("session.info", Json::object());
			const Result<Json> state = setup.Call("play.state", Json::object());
			REQUIRE(info.has_value());
			REQUIRE(state.has_value());
			CHECK((*info)["playState"] == Json("Paused"));
			CHECK((*info)["stopped"] == Json(true));
			CHECK((*info)["stopError"]["kind"] == Json("memory"));
			CHECK((*info)["stopError"]["script"] == Json("Assets/Scripts/LeakSecond.luau"));
			CHECK((*info)["stopError"] == (*state)["stopError"]);
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
			const Result<Json> edit = setup.Call("session.info", Json::object());
			REQUIRE(edit.has_value());
			CHECK((*edit)["stopped"] == Json(false));
			CHECK((*edit)["stopError"].is_null());
		}

		TEST_CASE("SessionMethods: session.shutdown with a dirty scene requires save or force")
		{
			Test::AutomationFixture setup("SessionShutdown");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Unsaved" } }).has_value());
			const Result<Json> refused = setup.Call("session.shutdown", Json::object());
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			CHECK_FALSE(setup.GetEditor().GetShutdownRequest().has_value());

			Result<Json> saved = setup.Call("session.shutdown", Json{ { "save", true } });
			REQUIRE(saved.has_value());
			CHECK((*saved)["saved"] == Json(true));
			CHECK((*saved)["savedFiles"][0] == Json("Assets/Scenes/Main.scene"));
			CHECK(setup.GetEditor().GetShutdownRequest() == 0);
			CHECK_FALSE(setup.GetEditor().IsSceneDirty());
		}

		TEST_CASE("SessionMethods: session.shutdown with force discards unsaved changes")
		{
			Test::AutomationFixture setup("SessionForce");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Unsaved" } }).has_value());
			Result<Json> forced = setup.Call("session.shutdown", Json{ { "force", true } });
			REQUIRE(forced.has_value());
			CHECK((*forced)["saved"] == Json(false));
			CHECK(setup.GetEditor().GetShutdownRequest() == 0);
		}
	}

}
