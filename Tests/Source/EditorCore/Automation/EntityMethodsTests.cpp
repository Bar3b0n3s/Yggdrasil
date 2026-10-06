#include "TestsPCH.h"

#include "EditorCore/Automation/EntityMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"

namespace Engine {

	static Json ParseEntityMethodJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	static std::string ReadEntityId(Json& result)
	{
		return JsonReader(result["entity"]["id"]).ReadString().value_or(std::string());
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EntityMethods: entity.create adds an entity with components as one undo step" * doctest::skip(true))
		{
			Test::AutomationFixture setup("EntityCreate");
			Result<Json> created = setup.Call("entity.create", ParseEntityMethodJson(R"({"name":"Camera","tags":["Main"],"active":false,
				"components":{"Transform":{"Translation":[4.5,9.5,20]},"Camera":{"Projection":"orthographic","OrthographicSize":11,"Primary":true}}})"));
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK((*created)["entity"]["name"] == Json("Camera"));
			CHECK((*created)["entity"]["path"] == Json("/Camera"));
			CHECK(ReadEntityId(*created).size() == 16);
			CHECK((*created)["undoIndex"] == Json(setup.GetEditor().GetHistory().GetCurrentSequence()));
			CHECK(setup.GetEditor().GetHistory().GetUndoLabel().starts_with("[agent] "));

			Result<Json> fetched = setup.Call("entity.get", Json{ { "entity", ReadEntityId(*created) } });
			REQUIRE(fetched.has_value());
			CHECK((*fetched)["entity"]["active"] == Json(false));
			CHECK((*fetched)["entity"]["tags"] == ParseEntityMethodJson(R"(["Main"])"));
			CHECK((*fetched)["entity"]["components"]["Camera"]["Projection"] == Json("Orthographic"));
			CHECK((*fetched)["entity"]["components"]["Transform"]["Translation"] == ParseEntityMethodJson("[4.5,9.5,20]"));

			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EntityMethods: an unknown component field is InvalidParams with a did-you-mean hint" * doctest::skip(true))
		{
			Test::AutomationFixture setup("EntityUnknownField");
			Json response = setup.Request("entity.create", ParseEntityMethodJson(R"({"name":"Ball","components":{"RigidBody":{"Mas":2}}})"));
			CHECK(response["error"]["code"] == Json(-32602));
			CHECK(response["error"]["data"]["issues"][0]["pointer"] == Json("/components/RigidBody/Mas"));
			CHECK(response["error"]["data"]["issues"][0]["hint"].dump().contains("Mass"));
			Json unknownComponent = setup.Request("entity.create", ParseEntityMethodJson(R"({"name":"Ball","components":{"RigidBdy":{}}})"));
			CHECK(unknownComponent["error"]["data"]["issues"][0]["hint"].dump().contains("RigidBody"));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EntityMethods: entity.get selects components and children" * doctest::skip(true))
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

		TEST_CASE("EntityMethods: entity.update changes only what is given and adds missing components" * doctest::skip(true))
		{
			Test::AutomationFixture setup("EntityUpdate");
			REQUIRE(setup.Call("entity.create", ParseEntityMethodJson(R"({"name":"Ball","tags":["Player"],"active":false})")).has_value());
			Result<Json> updated = setup.Call("entity.update",
				ParseEntityMethodJson(R"({"entity":"/Ball","components":{"RigidBody":{"Mass":2},"SphereCollider":{"Radius":0.25}}})"));
			REQUIRE_MESSAGE(updated.has_value(), updated.error().ToString());
			CHECK((*updated)["entity"]["name"] == Json("Ball"));
			CHECK((*updated)["entity"]["active"] == Json(false)); // not given: unchanged
			CHECK((*updated)["entity"]["tags"] == ParseEntityMethodJson(R"(["Player"])"));
			CHECK((*updated)["entity"]["components"]["RigidBody"]["Mass"] == Json(2));

			Result<Json> removed = setup.Call("entity.update", ParseEntityMethodJson(R"({"entity":"/Ball","name":"Orb","removeComponents":["SphereCollider"]})"));
			REQUIRE(removed.has_value());
			CHECK((*removed)["entity"]["name"] == Json("Orb"));
			CHECK_FALSE((*removed)["entity"]["components"].contains("SphereCollider"));
			CHECK(setup.Call("entity.update", ParseEntityMethodJson(R"({"entity":"/Orb","removeComponents":["Transform"]})")).error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EntityMethods: entity.destroy, entity.duplicate and entity.reparent are one command each" * doctest::skip(true))
		{
			Test::AutomationFixture setup("EntityStructure");
			REQUIRE(setup.Call("edit.batch", ParseEntityMethodJson(R"({"label":"Tree","ops":[
				{"method":"entity.create","params":{"name":"A"}},
				{"method":"entity.create","params":{"name":"B","parent":{"$ref":"0.entity.id"}}},
				{"method":"entity.create","params":{"name":"C"}}]})"))
					.has_value());

			Result<Json> duplicated = setup.Call("entity.duplicate", Json{ { "entities", Json::array({ "/A" }) } });
			REQUIRE(duplicated.has_value());
			CHECK((*duplicated)["entities"][0]["name"] == Json("A"));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 5);

			Result<Json> reparented = setup.Call("entity.reparent", Json{ { "entity", "/C" }, { "parent", "/A[0]" }, { "index", 0 } });
			REQUIRE(reparented.has_value());
			CHECK((*reparented)["entity"]["path"] == Json("/A[0]/C"));
			CHECK(setup.Call("entity.reparent", Json{ { "entity", "/A[0]" }, { "parent", "/A[0]/B" } }).error().GetCode() == ErrorCode::InvalidArgument);

			Result<Json> destroyed = setup.Call("entity.destroy", Json{ { "entities", Json::array({ "/A[0]", "/A[0]/B" }) } });
			REQUIRE(destroyed.has_value());
			CHECK((*destroyed)["destroyed"].size() == 3);
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 2);
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 5);
		}

		TEST_CASE("EntityMethods: dry runs report the would-be result and change nothing" * doctest::skip(true))
		{
			Test::AutomationFixture setup("EntityDryRun");
			const uint64_t revision = setup.GetEditor().GetScene().GetRevision();
			Json response = setup.Request("entity.create", Json{ { "name", "Ghost" }, { "dryRun", true } });
			REQUIRE(response.contains("result"));
			CHECK(response["result"]["dryRun"] == Json(true));
			CHECK(response["result"]["entity"]["name"] == Json("Ghost"));
			CHECK(response["result"]["undoIndex"] == Json(0));
			CHECK(response["result"]["_meta"]["revision"] == Json(revision));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
			CHECK(setup.GetEditor().GetScene().GetRevision() == revision);
		}
	}

}
