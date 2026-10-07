#include "TestsPCH.h"

#include "EditorCore/Automation/SessionMethods.h"

#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Platform/Process.h"
#include "Support/AutomationTestClient.h"

namespace Engine {

	TEST_SUITE("EditorCore")
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
