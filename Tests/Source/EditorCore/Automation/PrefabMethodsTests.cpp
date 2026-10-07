#include "TestsPCH.h"

#include "EditorCore/Automation/PrefabMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		Json ParsePrefabMethodJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE(json.has_value());
			return std::move(*json);
		}

		Json CallOrFail(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			Result<Json> result = setup.Call(method, params);
			INFO(std::string(method));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("PrefabMethods: create, instantiate, apply, revert and unpack are undoable steps" * doctest::skip(true))
		{
			Test::AutomationFixture setup("PrefabLifecycle");
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Cell", "components": {"Transform": {"Scale": [0.95, 0.95, 0.95]},
				"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			Json created = CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Cell", "path": "Assets/Prefabs/Cell.prefab",
				"replaceWithInstance": true})"));
			CHECK(created["prefab"]["type"] == Json("Prefab"));
			CHECK(created["prefab"]["path"] == Json("Assets/Prefabs/Cell.prefab"));
			CHECK(created["instance"]["path"] == Json("/Cell"));

			Json instance = CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Cell.prefab", "name": "Cell2",
				"transform": {"Translation": [2, 0, 0]}})"));
			CHECK(instance["entity"]["name"] == Json("Cell2"));

			// An override on the second instance, applied to the prefab: the first instance follows.
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Cell2", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			Json applied = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Cell2"})"));
			CHECK(applied["updatedInstances"] == Json(2));
			Json first = CallOrFail(setup, "entity.get", ParsePrefabMethodJson(R"({"entity": "/Cell", "components": ["MeshRenderer"]})"));
			CHECK(first["entity"]["components"]["MeshRenderer"]["CastShadows"] == Json(false));

			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Cell", "components": {"MeshRenderer": {"Visible": false}}})"));
			Json reverted = CallOrFail(setup, "prefab.revert", ParsePrefabMethodJson(R"({"instance": "/Cell"})"));
			CHECK(reverted["removedOverrides"] != Json(0));
			Json visible = CallOrFail(setup, "entity.get", ParsePrefabMethodJson(R"({"entity": "/Cell", "components": ["MeshRenderer"]})"));
			CHECK(visible["entity"]["components"]["MeshRenderer"]["Visible"] == Json(true));

			Json unpacked = CallOrFail(setup, "prefab.unpack", ParsePrefabMethodJson(R"({"instance": "/Cell2"})"));
			CHECK(unpacked["entity"]["name"] == Json("Cell2"));
			CHECK_FALSE(setup.GetEditor().GetScene().FindEntityByPath("/Cell2").HasComponent<PrefabLinkComponent>());

			// Every step is one undo entry: undoing all of them restores the scene with the original entity.
			REQUIRE(setup.Call("edit.undo", Json{ { "steps", 7 } }).has_value());
			CHECK(setup.GetEditor().GetScene().FindEntityByPath("/Cell").IsValid());
			CHECK_FALSE(setup.GetEditor().GetScene().FindEntityByPath("/Cell2").IsValid());
		}

		TEST_CASE("PrefabMethods: errors name the problem" * doctest::skip(true))
		{
			Test::AutomationFixture setup("PrefabErrors");
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Plain"})"));
			Json notInstance = setup.Request("prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Plain"})"));
			CHECK(notInstance["error"]["code"] == Json(-32602));
			Json missing = setup.Request("prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Missing.prefab"})"));
			CHECK(missing["error"]["code"] == Json(-32001));
			Json badPath = setup.Request("prefab.create", ParsePrefabMethodJson(R"({"entity": "/Plain", "path": "Assets/Plain.scene"})"));
			CHECK(badPath["error"]["code"] == Json(-32602));
			Json dry = CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Plain", "path": "Assets/Plain.prefab", "dryRun": true})"));
			CHECK(dry["dryRun"] == Json(true));
		}
	}

}
