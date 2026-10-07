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
		TEST_CASE("EntityMethods: entity.create adds an entity with components as one undo step")
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

			// §13.5 lists name as required: there is no default name.
			Json nameless = setup.Request("entity.create", Json::object());
			CHECK(nameless["error"]["code"] == Json(-32602));
			CHECK(nameless["error"]["data"]["issues"][0]["pointer"] == Json("/name"));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EntityMethods: an unknown component field is InvalidParams with a did-you-mean hint")
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

		TEST_CASE("EntityMethods: entity.update changes only what is given and adds missing components")
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

		TEST_CASE("EntityMethods: virtual component fields are set through their setters in entity.create and entity.update")
		{
			Test::AutomationFixture setup("EntityVirtualFields");
			REQUIRE(setup.Call("entity.create", ParseEntityMethodJson(R"({"name":"Board","components":{"Transform":{"Translation":[1,0,0]}}})"))
					.has_value());
			// WorldPosition is computed from the parent: the child's local translation is what remains.
			Result<Json> created = setup.Call("entity.create",
				ParseEntityMethodJson(R"({"name":"Piece","parent":"/Board","components":{"Transform":{"WorldPosition":[3,0,0]}}})"));
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			Result<Json> fetched = setup.Call("entity.get", Json{ { "entity", "/Board/Piece" } });
			REQUIRE(fetched.has_value());
			CHECK((*fetched)["entity"]["components"]["Transform"]["Translation"] == ParseEntityMethodJson("[2,0,0]"));

			// A virtual field and a stored field of the same component in one entity.update. EulerAngles are degrees applied
			// Z, then X, then Y (q = qY * qX * qZ, TransformSystem::QuaternionFromEulerDegrees), and Rotation serializes as
			// [x, y, z, w]: (90, 90, 0) is (0.5, 0.5, -0.5, 0.5). Radians, another axis order or a sign error give another
			// quaternion (qX * qY would have z = +0.5).
			Result<Json> updated = setup.Call("entity.update",
				ParseEntityMethodJson(R"({"entity":"/Board/Piece","components":{"Transform":{"Scale":[2,2,2],"EulerAngles":[90,90,0]}}})"));
			REQUIRE_MESSAGE(updated.has_value(), updated.error().ToString());
			const Json& transform = (*updated)["entity"]["components"]["Transform"];
			CHECK(transform["Scale"] == ParseEntityMethodJson("[2,2,2]"));
			const std::array<double, 4> expected = { 0.5, 0.5, -0.5, 0.5 };
			REQUIRE(transform["Rotation"].size() == expected.size());
			for (size_t index = 0; index < expected.size(); ++index)
			{
				CAPTURE(index);
				CHECK(JsonReader(transform["Rotation"][index]).ReadDouble().value_or(99.0) == doctest::Approx(expected[index]).epsilon(1e-5));
			}
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == 3);

			Json misspelled = setup.Request("entity.update", ParseEntityMethodJson(R"({"entity":"/Board/Piece","components":{"Transform":{"EulerAngle":[0,0,0]}}})"));
			CHECK(misspelled["error"]["code"] == Json(-32602));
			CHECK(misspelled["error"]["data"]["issues"][0]["pointer"] == Json("/components/Transform/EulerAngle"));
			Json readOnly = setup.Request("entity.update", ParseEntityMethodJson(R"({"entity":"/Board/Piece","components":{"Transform":{"WorldScale":[1,1,1]}}})"));
			CHECK(readOnly["error"]["code"] == Json(-32602));
			CHECK(readOnly["error"]["data"]["issues"][0]["pointer"] == Json("/components/Transform/WorldScale"));
			CHECK(readOnly["error"]["data"]["issues"][0]["message"].dump().contains("read-only"));
			Json outOfRange = setup.Request("entity.update", ParseEntityMethodJson(R"({"entity":"/Board/Piece","components":{"Transform":{"EulerAngles":[0,0]}}})"));
			CHECK(outOfRange["error"]["code"] == Json(-32602));
			CHECK(outOfRange["error"]["data"]["issues"][0]["pointer"].dump().contains("/components/Transform/EulerAngles"));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == 3);
		}

		TEST_CASE("EntityMethods: entity.destroy, entity.duplicate and entity.reparent are one command each")
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

		TEST_CASE("EntityMethods: dry runs report the would-be result and change nothing")
		{
			Test::AutomationFixture setup("EntityDryRun");
			// "_meta".revision is the editor's revision (EditorContext::GetRevision, ADR 0008 decision 28).
			const uint64_t revision = setup.GetEditor().GetRevision();
			Json response = setup.Request("entity.create", Json{ { "name", "Ghost" }, { "dryRun", true } });
			REQUIRE(response.contains("result"));
			CHECK(response["result"]["dryRun"] == Json(true));
			CHECK(response["result"]["entity"]["name"] == Json("Ghost"));
			CHECK(response["result"]["undoIndex"] == Json(0));
			CHECK(response["result"]["_meta"]["revision"] == Json(revision));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
			CHECK(setup.GetEditor().GetRevision() == revision);
		}

		TEST_CASE("EntityMethods: entity.bounds reports world AABBs of meshes and their descendants" * doctest::skip(true))
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
