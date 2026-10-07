#include "TestsPCH.h"

#include "Support/AutomationTestClient.h"

#include "Support/TestData.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("AutomationTestClient: the test server specification is in-process with test hooks")
		{
			const AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			CHECK_FALSE(specification.Listen);
			CHECK(specification.TestHooks);
			CHECK(specification.RendererName == "none");
			CHECK(specification.DocsRoot == Test::GetRepositoryRoot());
		}

		TEST_CASE("AutomationTestClient: requests get their own responses and errors become Results")
		{
			Test::EditorTestFixture fixture("TestClient");
			Test::AutomationTestClient client(fixture.GetEditor());
			Json response = client.Request("session.info", Json::object());
			CHECK(response["jsonrpc"] == Json("2.0"));
			CHECK(response.contains("result"));
			const Result<Json> refused = client.Call("scene.tree", Json::object());
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("AutomationFixture: opens a project and a scene and connects a client")
		{
			Test::AutomationFixture setup("TestFixture");
			CHECK(setup.GetEditor().HasScene());
			CHECK(setup.Call("scene.tree", Json::object()).has_value());
		}
	}

}
