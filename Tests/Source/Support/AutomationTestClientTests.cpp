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

		TEST_CASE("AutomationTestClient: an error's code, detail and every issue reach the Result")
		{
			// Two problems in one request (an out-of-range width and a misspelled member, whose issue has a hint): the response
			// carries both as issues, and the Result carries them as the response does.
			Test::AutomationFixture setup("TestClientIssues", false);
			const Json params = Json{ { "view", "scene" }, { "width", 0 }, { "heigth", 360 } };
			const Json response = setup.Request("viewport.screenshot", params);
			REQUIRE(response.contains("error"));
			const Json& data = response["error"]["data"];
			REQUIRE(data["issues"].is_array());
			REQUIRE(data["issues"].size() == 2);

			const Result<Json> refused = setup.Call("viewport.screenshot", params);
			REQUIRE_FALSE(refused.has_value());
			const Error& error = refused.error();
			CHECK(error.GetCode() == ErrorCode::InvalidArgument);
			CHECK(data["errorCode"] == Json("InvalidArgument"));
			CHECK_FALSE(error.GetMessageText().empty());
			CHECK(Json(error.GetMessageText()) == data["detail"]);
			REQUIRE(error.GetIssues().size() == 2);
			bool hinted = false;
			for (size_t index = 0; index < 2; ++index)
			{
				const ErrorIssue& issue = error.GetIssues()[index];
				const Json& expected = data["issues"][index];
				INFO(expected.dump());
				CHECK(Json(issue.JsonPointer) == expected["pointer"]);
				CHECK(Json(issue.Message) == expected["message"]);
				CHECK(Json(issue.Hint) == (expected.contains("hint") ? expected["hint"] : Json("")));
				hinted = hinted || !issue.Hint.empty();
			}
			CHECK(hinted);
		}

		TEST_CASE("AutomationFixture: opens a project and a scene and connects a client")
		{
			Test::AutomationFixture setup("TestFixture");
			CHECK(setup.GetEditor().HasScene());
			CHECK(setup.Call("scene.tree", Json::object()).has_value());
		}
	}

}
