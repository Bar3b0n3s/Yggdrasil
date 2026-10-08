#include "TestsPCH.h"

#include "Engine/Automation/Methods/EntityMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <string>

// The entity reads both hosts serve (entity.get, entity.bounds; Docs/Decisions/0012-m7-decisions.md decision 21), in
// process through the editor's AutomationFixture (§15.2). The editor's entity mutations are tested in
// Tests/Source/EditorCore/Automation/EntityMethodsTests.cpp, the Runtime's answers in RuntimeAutomationServerTests.cpp.

namespace Engine {

	static Json ParseEntityMethodJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("EntityMethods: entity.get selects components and children")
		{
			Test::AutomationFixture setup("EntityGet");
			REQUIRE(setup.Call("edit.batch", ParseEntityMethodJson(R"({"label":"Tree","ops":[
				{"method":"entity.create","params":{"name":"Game","components":{"Camera":{}}}},
				{"method":"entity.create","params":{"name":"Child","parent":{"$ref":"0.entity.id"}}}]})"))
					.has_value());
			Result<Json> selected = setup.Call("entity.get", ParseEntityMethodJson(R"({"entity":"/Game","components":["Camera"],"children":true})"));
			REQUIRE(selected.has_value());
			CHECK((*selected)["entity"]["components"].size() == 1);
			CHECK((*selected)["entity"]["children"][0]["name"] == Json("Child"));
			Result<Json> byPrefix = setup.Call("entity.get", Json{ { "entity", JsonReader((*selected)["entity"]["id"]).ReadString().value_or("").substr(0, 6) } });
			REQUIRE(byPrefix.has_value());
			CHECK((*byPrefix)["entity"]["name"] == Json("Game"));
			CHECK(setup.Call("entity.get", Json{ { "entity", "/Nobody" } }).error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Call("entity.get", Json{ { "entity", "/Game" }, { "components", 3 } }).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("EntityMethods: entity.bounds reports world AABBs of meshes and their descendants")
		{
			Test::AutomationFixture setup("EntityBounds");
			REQUIRE(setup.Call("entity.create", ParseEntityMethodJson(R"({"name": "Track", "components": {"Transform": {"Translation": [10, 0, 0]}}})")).has_value());
			REQUIRE(setup.Call("entity.create", ParseEntityMethodJson(R"({"name": "Piece", "parent": "/Track", "components": {
				"Transform": {"Translation": [0, 1, 0], "Scale": [2, 1, 4]}, "MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"))
					.has_value());
			Result<Json> bounds = setup.Call("entity.bounds", ParseEntityMethodJson(R"({"entities": ["/Track", "/Track/Piece"]})"));
			REQUIRE_MESSAGE(bounds.has_value(), bounds.error().ToString());
			REQUIRE((*bounds)["bounds"].size() == 2);
			// The root has no mesh of its own: its bounds are its child's (includeDescendants defaults to true).
			CHECK((*bounds)["bounds"][0]["hasBounds"] == Json(true));
			CHECK((*bounds)["bounds"][0]["min"] == ParseEntityMethodJson("[9, 0.5, -2]"));
			CHECK((*bounds)["bounds"][0]["max"] == ParseEntityMethodJson("[11, 1.5, 2]"));
			CHECK((*bounds)["bounds"][1]["center"] == ParseEntityMethodJson("[10, 1, 0]"));
			CHECK((*bounds)["bounds"][1]["size"] == ParseEntityMethodJson("[2, 1, 4]"));
			Result<Json> own = setup.Call("entity.bounds", ParseEntityMethodJson(R"({"entities": ["/Track"], "includeDescendants": false})"));
			REQUIRE(own.has_value());
			CHECK((*own)["bounds"][0]["hasBounds"] == Json(false));
			CHECK((*own)["bounds"][0]["min"] == Json::array());
			Json missing = setup.Request("entity.bounds", ParseEntityMethodJson(R"({"entities": ["/Nothing"]})"));
			CHECK(missing["error"]["code"] == Json(-32001));
		}
	}

}
