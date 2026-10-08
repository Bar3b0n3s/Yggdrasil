#include "TestsPCH.h"

#include "EditorCore/Automation/ObserveMethods.h"

#include "Support/AutomationTestClient.h"

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ObserveMethods: docs.get lists the skills and returns a topic")
		{
			Test::EditorTestFixture fixture("DocsGet");
			Test::AutomationTestClient client(fixture.GetEditor());
			Result<Json> topics = client.Call("docs.get", Json::object());
			REQUIRE(topics.has_value());
			CHECK((*topics)["topics"].dump().contains("skills/build-and-test"));
			CHECK((*topics)["topics"].dump().contains("skills/add-automation-method"));
			Result<Json> skill = client.Call("docs.get", Json{ { "topic", "skills/add-automation-method" } });
			REQUIRE(skill.has_value());
			CHECK((*skill)["content"].dump().contains("RegisterMethods.cpp"));
			CHECK((*skill)["path"] == Json(".claude/skills/add-automation-method/SKILL.md"));
			CHECK(client.Call("docs.get", Json{ { "topic", "skills/missing" } }).error().GetCode() == ErrorCode::NotFound);
		}
	}

}
