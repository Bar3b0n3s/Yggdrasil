#include "TestsPCH.h"

#include "Engine/Automation/Methods/EntityMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
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

	// Gives the edit scene's entity at `path` the preserved components a scene file's "Native.Spinner" and an unknown
	// "FutureComponent" load into (Scene/Components/UnknownComponentsComponent.h; Docs/Decisions/0030-m19-contract.md
	// decision 27), as an editor without the native module holds them.
	static void AddPreservedEntityComponents(Test::AutomationFixture& setup, std::string_view path)
	{
		Entity entity = setup.GetEditor().GetScene().FindEntityByPath(path);
		REQUIRE(entity.IsValid());
		UnknownComponentsComponent preserved;
		preserved.Components.push_back({ .Name = "Native.Spinner", .Version = 1, .Data = VariantValue(Json{ { "Speed", 90 } }) });
		preserved.Components.push_back({ .Name = "FutureComponent", .Version = 1, .Data = VariantValue(Json{ { "Size", 2 } }) });
		entity.AddComponent<UnknownComponentsComponent>(std::move(preserved));
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

		// The native test game's friction NT-07: entity.get reports an entity's native components ("Native.<Name>") as the
		// scene holds them, in "all" (the default) and by name, unchecked against a loaded module; an unknown component stays
		// unreported.
		TEST_CASE("EntityMethods: entity.get reports an entity's native components as the scene holds them")
		{
			Test::AutomationFixture setup("EntityGetNative");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Wheel" } }).has_value());
			AddPreservedEntityComponents(setup, "/Wheel");
			const Json spinner = Json{ { "Speed", 90 } };

			Result<Json> all = setup.Call("entity.get", Json{ { "entity", "/Wheel" } });
			REQUIRE_MESSAGE(all.has_value(), all.error().ToString());
			const Json& components = (*all)["entity"]["components"];
			CHECK(components.contains("Transform"));
			REQUIRE(components.contains("Native.Spinner"));
			CHECK(components["Native.Spinner"] == spinner);
			CHECK_FALSE(components.contains("FutureComponent"));
			Result<Json> named = setup.Call("entity.get", Json{ { "entity", "/Wheel" }, { "components", "all" } });
			REQUIRE(named.has_value());
			CHECK((*named)["entity"]["components"] == components);

			Result<Json> selected = setup.Call("entity.get", Json{ { "entity", "/Wheel" }, { "components", Json::array({ "Native.Spinner" }) } });
			REQUIRE_MESSAGE(selected.has_value(), selected.error().ToString());
			CHECK((*selected)["entity"]["components"] == Json{ { "Native.Spinner", spinner } });
			// A native name the entity does not hold reports nothing, as a built-in component it lacks does.
			Result<Json> absent = setup.Call("entity.get", Json{ { "entity", "/Wheel" }, { "components", Json::array({ "Native.Missing", "Camera" }) } });
			REQUIRE_MESSAGE(absent.has_value(), absent.error().ToString());
			CHECK((*absent)["entity"]["components"].empty());
			// "Native." alone names no native component and is an unknown component.
			const Result<Json> prefixOnly = setup.Call("entity.get", Json{ { "entity", "/Wheel" }, { "components", Json::array({ "Native." }) } });
			REQUIRE_FALSE(prefixOnly.has_value());
			CHECK(prefixOnly.error().GetCode() == ErrorCode::NotFound);
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
