#include "TestsPCH.h"

#include "Engine/Automation/Methods/SceneMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <format>
#include <string>

// The scene reads both hosts serve (scene.tree, scene.query, scene.get; Docs/Decisions/0012-m7-decisions.md decision 21),
// in process through the editor's AutomationFixture (§15.2). The editor-only scene methods are tested in
// Tests/Source/EditorCore/Automation/SceneMethodsTests.cpp, the Runtime's answers in RuntimeAutomationServerTests.cpp.

namespace Engine {

	static Json ParseSceneMethodJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("SceneMethods: scene.tree prints the text outline and the JSON list with a depth limit")
		{
			Test::AutomationFixture setup("SceneTree");
			REQUIRE(setup.Call("edit.batch", ParseSceneMethodJson(R"({"label":"Tree","ops":[
				{"method":"entity.create","params":{"name":"Camera","components":{"Camera":{"Projection":"Orthographic","OrthographicSize":11}}}},
				{"method":"entity.create","params":{"name":"Game"}},
				{"method":"entity.create","params":{"name":"ScoreText","parent":{"$ref":"1.entity.id"}}},
				{"method":"entity.create","params":{"name":"Deep","parent":{"$ref":"2.entity.id"}}}]})"))
					.has_value());

			Result<Json> text = setup.Call("scene.tree", Json::object());
			REQUIRE(text.has_value());
			const std::string outline = JsonReader((*text)["text"]).ReadString().value_or(std::string());
			CHECK(outline.starts_with("Main.scene  rev "));
			CHECK(outline.contains("4 entities"));
			CHECK(outline.contains("Camera(Ortho 11)"));
			CHECK(outline.contains("ScoreText"));

			Result<Json> limited = setup.Call("scene.tree", Json{ { "depth", 2 }, { "format", "JSON" } });
			REQUIRE(limited.has_value());
			CHECK((*limited)["entities"].size() == 3);
			bool omitted = false;
			for (Json& entry : (*limited)["entities"])
				omitted = omitted || entry["childrenOmitted"] == Json(true);
			CHECK(omitted);

			Result<Json> subtree = setup.Call("scene.tree", Json{ { "root", "/Game" }, { "format", "json" } });
			REQUIRE(subtree.has_value());
			CHECK((*subtree)["entities"][0]["name"] == Json("Game"));
			CHECK((*subtree)["entities"][0]["depth"] == Json(0));
		}

		TEST_CASE("SceneMethods: scene.query filters and paginates in canonical order")
		{
			Test::AutomationFixture setup("SceneQuery");
			Json batch = ParseSceneMethodJson(R"({"label":"Cells","ops":[]})");
			for (int index = 0; index < 25; ++index)
			{
				batch["ops"].push_back(Json{ { "method", "entity.create" },
					{ "params", Json{ { "name", std::format("Cell{}", index) }, { "tags", Json::array({ index % 2 == 0 ? "Even" : "Odd" }) } } } });
			}
			REQUIRE(setup.Call("edit.batch", batch).has_value());

			Result<Json> first = setup.Call("scene.query", ParseSceneMethodJson(R"({"where":{"name":"Cell*","tag":"Even"},"limit":5,"select":["Transform"]})"));
			REQUIRE(first.has_value());
			CHECK((*first)["total"] == Json(13));
			CHECK((*first)["entities"].size() == 5);
			CHECK((*first)["entities"][0]["name"] == Json("Cell0"));
			CHECK((*first)["entities"][0]["components"].contains("Transform"));
			const std::string cursor = JsonReader((*first)["nextCursor"]).ReadString().value_or(std::string());
			REQUIRE_FALSE(cursor.empty());

			Result<Json> second = setup.Call("scene.query", Json{ { "where", Json{ { "name", "Cell*" }, { "tag", "Even" } } }, { "limit", 5 }, { "cursor", cursor } });
			REQUIRE(second.has_value());
			CHECK((*second)["entities"][0]["name"] == Json("Cell10"));
			CHECK(setup.Call("scene.query", ParseSceneMethodJson(R"({"where":{"component":"RigidBdy"}})")).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Call("scene.query", ParseSceneMethodJson(R"({"where":{},"limit":1001})")).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("SceneMethods: scene.get returns the canonical document")
		{
			Test::AutomationFixture setup("SceneGet");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Board" } }).has_value());
			Result<Json> scene = setup.Call("scene.get", Json::object());
			REQUIRE(scene.has_value());
			CHECK((*scene)["scene"]["Format"] == Json("Scene"));
			CHECK((*scene)["scene"]["Entities"][0]["Name"] == Json("Board"));
			CHECK(setup.Call("scene.get", Json{ { "target", "play" } }).error().GetCode() == ErrorCode::InvalidState);
		}
	}

}
