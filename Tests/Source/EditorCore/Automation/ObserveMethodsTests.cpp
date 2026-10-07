#include "TestsPCH.h"

#include "EditorCore/Automation/ObserveMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Support/AutomationTestClient.h"

#include <charconv>

namespace Engine {

	// The sequence number of a log or event cursor (decimal digits only).
	static uint64_t ParseLogCursor(const std::string& cursor)
	{
		uint64_t value = 0;
		const std::from_chars_result parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), value);
		REQUIRE_MESSAGE(parsed.ec == std::errc(), cursor);
		REQUIRE_MESSAGE(parsed.ptr == cursor.data() + cursor.size(), cursor);
		return value;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ObserveMethods: log.read returns entries by cursor with filters")
		{
			Test::AutomationFixture setup("LogRead", false);
			Result<Json> start = setup.Call("log.read", Json{ { "limit", 1 } });
			REQUIRE(start.has_value());
			const std::string cursor = std::to_string(Log::GetRingBuffer().GetNextSeq());
			ENGINE_CORE_WARN("log.read marker {}", 1);
			ENGINE_CORE_INFO("log.read marker {}", 2);

			Result<Json> warnings = setup.Call("log.read", Json{ { "cursor", cursor }, { "minLevel", "warn" }, { "contains", "log.read marker" } });
			REQUIRE(warnings.has_value());
			REQUIRE((*warnings)["entries"].size() == 1);
			CHECK((*warnings)["entries"][0]["message"] == Json("log.read marker 1"));
			CHECK((*warnings)["entries"][0]["level"] == Json("Warn"));
			CHECK((*warnings)["entries"][0]["logger"] == Json("Engine"));
			const std::string next = JsonReader((*warnings)["nextCursor"]).ReadString().value_or(std::string());
			CHECK(ParseLogCursor(next) > ParseLogCursor(cursor));

			Result<Json> app = setup.Call("log.read", Json{ { "cursor", cursor }, { "loggers", Json::array({ "app" }) } });
			REQUIRE(app.has_value());
			CHECK((*app)["entries"].size() == 0);
		}

		TEST_CASE("ObserveMethods: the end cursor reads nothing and returns where the next entry will be")
		{
			Test::AutomationFixture setup("LogEnd", false);
			Result<Json> now = setup.Call("log.read", Json{ { "cursor", "end" } });
			REQUIRE(now.has_value());
			CHECK((*now)["entries"].empty());
			ENGINE_CORE_WARN("log.read end marker");
			Result<Json> after = setup.Call("log.read", Json{ { "cursor", (*now)["nextCursor"] }, { "contains", "end marker" } });
			REQUIRE(after.has_value());
			CHECK((*after)["entries"].size() == 1);

			Result<Json> events = setup.Call("events.read", Json{ { "cursor", "end" } });
			REQUIRE(events.has_value());
			CHECK((*events)["events"].empty());
			CHECK(setup.Call("log.read", Json{ { "cursor", "later" } }).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("ObserveMethods: events.read returns engine events by cursor and type")
		{
			Test::AutomationFixture setup("EventsRead");
			const std::string cursor = std::to_string(setup.GetEditorFixture().GetEngine().GetEventLog().GetNextSeq());
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" } }).has_value());
			Result<Json> events = setup.Call("events.read", Json{ { "cursor", cursor }, { "types", Json::array({ "sceneopened" }) } });
			REQUIRE(events.has_value());
			REQUIRE((*events)["events"].size() == 1);
			CHECK((*events)["events"][0]["type"] == Json("SceneOpened"));
			CHECK((*events)["events"][0]["path"].dump().contains("Second.scene"));
		}

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
