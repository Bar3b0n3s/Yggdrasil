#include "TestsPCH.h"

#include "EditorCore/Automation/EntityMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"

#include <utility>

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
		TEST_CASE("EntityMethods: script assignment rejects modules and suites without changing the entity or history")
		{
			Test::AutomationFixture setup("EntityScriptKind", true, std::nullopt, true);
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Target" } }).has_value());
			for (const std::string_view kind : { "Module", "Test" })
			{
				const std::string path = std::format("Assets/Scripts/{}.luau", kind);
				const auto created = setup.Call("script.create", Json{ { "path", path }, { "template", kind } });
				REQUIRE_MESSAGE(created, (created ? "" : created.error().ToString()));
				const uint64_t sequence = setup.GetEditor().GetHistory().GetCurrentSequence();
				const Json components{ { "Script", Json{ { "Script", path } } } };
				const Json response = setup.Request("entity.update", Json{ { "entity", "/Target" }, { "name", "Changed" }, { "components", components } });
				CHECK(response["error"]["data"]["errorCode"] == Json("Validation"));
				CHECK(response["error"]["data"].dump().contains("SCRIPT_NOT_A_BEHAVIOUR"));
				CHECK(response["error"]["data"]["issues"][0]["pointer"] == Json("/components/Script/Script"));
				CHECK(setup.GetEditor().GetHistory().GetCurrentSequence() == sequence);
				const auto target = setup.Call("entity.get", Json{ { "entity", "/Target" } });
				REQUIRE(target.has_value());
				CHECK_FALSE((*target)["entity"]["components"].contains("Script"));
				CHECK_FALSE(setup.Call("entity.create", Json{ { "name", "Rejected" }, { "components", components } }).has_value());
				CHECK(setup.GetEditor().GetScene().GetEntityCount() == 1);
			}
		}

		TEST_CASE("EntityMethods: duplicate remaps nested script entity fields and keeps string and external references")
		{
			Test::AutomationFixture setup("EntityScriptDuplicate");
			const std::string path = "Assets/Scripts/References.luau";
			REQUIRE(setup.Call("script.write", Json{ { "path", path }, { "source", R"(
local References = { Fields = {
	Target = Field.Entity(), Targets = Field.Array(Field.Array(Field.Entity())),
	External = Field.Entity(), Label = Field.String(), Amount = Field.Number(1, {Min = 0, Max = 5}),
} }
return Script.Define("References", References)
)" } })
					.has_value());
			auto root = setup.Call("entity.create", Json{ { "name", "Root" } });
			REQUIRE(root.has_value());
			auto child = setup.Call("entity.create", Json{ { "name", "Child" }, { "parent", "/Root" } });
			REQUIRE(child.has_value());
			auto outside = setup.Call("entity.create", Json{ { "name", "Outside" } });
			REQUIRE(outside.has_value());
			const std::string childID = ReadEntityId(*child);
			const std::string outsideID = ReadEntityId(*outside);
			const Json fields{ { "Target", childID }, { "Targets", Json::array({ Json::array({ childID, outsideID }) }) },
				{ "External", outsideID }, { "Label", childID }, { "Amount", 3 } };
			const auto assigned = setup.Call("entity.update", Json{ { "entity", "/Root" }, { "components", Json{ { "Script", Json{ { "Script", path }, { "Fields", fields } } } } } });
			REQUIRE_MESSAGE(assigned.has_value(), assigned.error().ToString());
			const auto patched = setup.Call("entity.update", Json{ { "entity", "/Root" }, { "components", Json{ { "Script", Json{ { "Fields", Json{ { "Amount", 4 } } } } } } } });
			REQUIRE_MESSAGE(patched, (patched ? "" : patched.error().ToString()));
			CHECK((*patched)["entity"]["components"]["Script"]["Fields"]["Amount"] == Json(4));
			const auto invalid = setup.Call("entity.update", Json{ { "entity", "/Root" }, { "components", Json{ { "Script", Json{ { "Fields", Json{ { "Amount", 6 } } } } } } } });
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(invalid.error().GetIssues().size() == 1);
			CHECK(invalid.error().GetIssues()[0].JsonPointer == "/components/Script/Fields/Amount");
			CHECK_FALSE(invalid.error().GetMessageText().contains("no script is assigned"));
			auto copies = setup.Call("entity.duplicate", Json{ { "entities", Json::array({ "/Root" }) } });
			REQUIRE_MESSAGE(copies.has_value(), copies.error().ToString());
			const Json copyID = (*copies)["entities"][0]["id"];
			const auto copy = setup.Call("entity.get", Json{ { "entity", copyID } });
			REQUIRE(copy.has_value());
			const Json& copiedFields = (*copy)["entity"]["components"]["Script"]["Fields"];
			CHECK(copiedFields["Target"] != Json(childID));
			CHECK(copiedFields["Targets"][0][0] == copiedFields["Target"]);
			CHECK(copiedFields["Targets"][0][1] == Json(outsideID));
			CHECK(copiedFields["External"] == Json(outsideID));
			CHECK(copiedFields["Label"] == Json(childID));
			CHECK(copiedFields["Amount"] == Json(4));
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 3);
			REQUIRE(setup.Call("edit.redo", Json::object()).has_value());
			const auto restored = setup.Call("entity.get", Json{ { "entity", copyID } });
			REQUIRE(restored.has_value());
			CHECK((*restored)["entity"]["components"]["Script"]["Fields"] == copiedFields);
		}

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

		TEST_CASE("EntityMethods: play-scene creations stop at Simulation.MaxEntities with InvalidState")
		{
			// §5.7: creating beyond the cap is an automation InvalidState error, never a crash; the edit scene is untouched.
			Test::AutomationFixture setup("EntityCap");
			REQUIRE(setup.Call("project.setSettings", ParseEntityMethodJson(R"({"patch": {"Simulation": {"MaxEntities": 3}}})")).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "A" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "B" } }).has_value());
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true } }).has_value());

			const Result<Json> third = setup.Call("entity.create", Json{ { "name", "C" }, { "target", "play" } });
			REQUIRE_MESSAGE(third.has_value(), third.error().ToString());
			for (const auto& [method, params] : { std::pair{ "entity.create", Json{ { "name", "D" }, { "target", "play" } } },
					 std::pair{ "entity.duplicate", Json{ { "entities", Json::array({ "/A" }) }, { "target", "play" } } } })
			{
				INFO(std::string(method));
				const Result<Json> refused = setup.Call(method, params);
				REQUIRE_FALSE(refused.has_value());
				INFO(refused.error().ToString());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
				CHECK(refused.error().GetMessageText().contains("entity limit 3 reached"));
			}
			const Result<Json> state = setup.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["entityCount"] == Json(3));
			CHECK((*state)["maxEntities"] == Json(3));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 2);
			REQUIRE(setup.Call("play.stop", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 2);
		}

		TEST_CASE("EntityMethods: asset references in component values accept handles, project paths and engine paths")
		{
			Test::AutomationFixture setup("EntityAssetReferences");
			Result<Json> material = setup.Call("asset.create", ParseEntityMethodJson(R"({"type": "Material", "path": "Assets/Materials/Red.material"})"));
			REQUIRE_MESSAGE(material.has_value(), material.error().ToString());
			const std::string materialId = JsonReader((*material)["asset"]["id"]).ReadString().value_or(std::string());

			// §7.1: an engine path and a project path resolve to their handles, which entity.get reports.
			Result<Json> created = setup.Call("entity.create", ParseEntityMethodJson(R"({"name": "Wall", "components": {
				"MeshRenderer": {"Mesh": "engine://Meshes/Cube", "Materials": ["Assets/Materials/Red.material"]}}})"));
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			Result<Json> fetched = setup.Call("entity.get", Json{ { "entity", "/Wall" } });
			REQUIRE(fetched.has_value());
			CHECK((*fetched)["entity"]["components"]["MeshRenderer"]["Mesh"] == Json("0000000000000101"));
			CHECK((*fetched)["entity"]["components"]["MeshRenderer"]["Materials"] == Json::array({ materialId }));

			// An asset of another type is InvalidParams; a path that names nothing is NotFound; both located at the value.
			Json wrongType = setup.Request("entity.update", ParseEntityMethodJson(R"({"entity": "/Wall", "components": {
				"MeshRenderer": {"Mesh": "Assets/Materials/Red.material"}}})"));
			CHECK(wrongType["error"]["code"] == Json(-32602));
			CHECK(wrongType["error"]["data"]["issues"][0]["pointer"] == Json("/components/MeshRenderer/Mesh"));
			Json unknown = setup.Request("entity.update", ParseEntityMethodJson(R"({"entity": "/Wall", "components": {
				"MeshRenderer": {"Materials": ["Assets/Materials/Rde.material"]}}})"));
			CHECK(unknown["error"]["code"] == Json(-32001));
			CHECK(unknown["error"]["data"]["issues"][0]["pointer"] == Json("/components/MeshRenderer/Materials/0"));
			CHECK(unknown["error"]["data"]["hint"].dump().contains("Assets/Materials/Red.material"));

			// The ops of edit.batch resolve them too.
			Result<Json> batch = setup.Call("edit.batch", ParseEntityMethodJson(R"({"ops": [{"method": "entity.update", "params": {"entity": "/Wall",
				"components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}}}]})"));
			REQUIRE_MESSAGE(batch.has_value(), batch.error().ToString());
			fetched = setup.Call("entity.get", Json{ { "entity", "/Wall" } });
			REQUIRE(fetched.has_value());
			CHECK((*fetched)["entity"]["components"]["MeshRenderer"]["Mesh"] == Json("0000000000000102"));
		}
	}

}
