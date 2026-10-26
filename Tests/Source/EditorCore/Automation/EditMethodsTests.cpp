#include "TestsPCH.h"
#include "EditorCore/Automation/EditMethods.h"

#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Json ParseEditMethodJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditMethods: play selection resolves runtime entities and clears to the shown target")
		{
			Test::AutomationFixture setup("EditSelectPlay");
			auto& editor = setup.GetEditor();
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			auto runtime = editor.GetPlay().GetSession()->CreateEntity("RuntimeOnly");
			REQUIRE(runtime);
			auto selected = setup.Call("edit.select", Json{ { "entities", Json::array({ "/RuntimeOnly" }) } });
			REQUIRE(selected);
			REQUIRE((*selected)["selection"].size() == 1);
			CHECK((*selected)["selection"][0]["name"] == Json("RuntimeOnly"));
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			auto current = setup.Call("edit.getSelection", Json::object());
			REQUIRE(current);
			CHECK((*current)["selection"] == (*selected)["selection"]);
			REQUIRE(setup.Call("edit.select", Json{ { "entities", Json::array() } }));
			CHECK(editor.GetSelection().empty());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			REQUIRE(editor.GetPlay().Stop());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Edit);
		}

		TEST_CASE("EditMethods: a scene played by path selects runtime entities without an open edit scene")
		{
			Test::AutomationFixture setup("EditSelectPathPlay");
			auto& editor = setup.GetEditor();
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Player" } }));
			REQUIRE(setup.Call("scene.save", Json::object()));
			editor.CloseScene();
			REQUIRE_FALSE(editor.HasScene());
			REQUIRE(setup.Call("play.start", Json{ { "scene", "Assets/Scenes/Main.scene" }, { "lockstep", true } }));
			auto selected = setup.Call("edit.select", Json{ { "entities", Json::array({ "/Player" }) } });
			REQUIRE(selected);
			REQUIRE((*selected)["selection"].size() == 1);
			CHECK((*selected)["selection"][0]["name"] == Json("Player"));
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			auto current = setup.Call("edit.getSelection", Json::object());
			REQUIRE(current);
			CHECK((*current)["selection"] == (*selected)["selection"]);
			auto cleared = setup.Call("edit.select", Json{ { "entities", Json::array() } });
			REQUIRE(cleared);
			CHECK((*cleared)["selection"].empty());
			CHECK(editor.GetSelection().empty());
			CHECK(editor.GetSelectionTarget() == SceneTarget::Play);
			REQUIRE(setup.Call("play.stop", Json::object()));
			CHECK(editor.GetSelection().empty());
		}

		TEST_CASE("EditMethods: edit.batch runs atomically with $ref substitution as one undo step")
		{
			Test::AutomationFixture setup("EditBatch");
			Result<Json> batch = setup.Call("edit.batch", ParseEditMethodJson(R"({"label":"Scaffold","ops":[
				{"method":"entity.create","params":{"name":"Game"}},
				{"method":"entity.create","params":{"name":"Board","parent":{"$ref":"0.entity.id"}}}]})"));
			REQUIRE_MESSAGE(batch.has_value(), batch.error().ToString());
			CHECK((*batch)["results"].size() == 2);
			CHECK((*batch)["results"][1]["entity"]["path"] == Json("/Game/Board"));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == 1);
			CHECK(setup.GetEditor().GetHistory().GetUndoLabel() == "[agent] Scaffold");
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EditMethods: a failing op rolls the batch back and reports failedOp")
		{
			Test::AutomationFixture setup("EditBatchRollback");
			const Result<std::string> before = SceneSerializer::SaveToString(setup.GetEditor().GetScene());
			REQUIRE(before.has_value());
			Json response = setup.Request("edit.batch", ParseEditMethodJson(R"({"label":"Broken","ops":[
				{"method":"entity.create","params":{"name":"A"}},
				{"method":"entity.create","params":{"name":"B"}},
				{"method":"entity.get","params":{"entity":"/Missing"}}]})"));
			CHECK(response["error"]["code"] == Json(-32001));
			CHECK(response["error"]["data"]["failedOp"] == Json(2));
			CHECK(SceneSerializer::SaveToString(setup.GetEditor().GetScene()).value_or(std::string()) == *before);
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("EditMethods: a dry-run batch with an op that cannot dry-run is rejected before anything runs")
		{
			Test::AutomationFixture setup("EditBatchDryRun");
			Json response = setup.Request("edit.batch", ParseEditMethodJson(R"({"label":"Dry","dryRun":true,"ops":[
				{"method":"entity.create","params":{"name":"A"}},
				{"method":"entity.get","params":{"entity":"/A"}}]})"));
			CHECK(response["error"]["code"] == Json(-32009));
			CHECK(response["error"]["data"]["failedOp"] == Json(1));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);

			Json fine = setup.Request("edit.batch", ParseEditMethodJson(R"({"label":"Dry","dryRun":true,"ops":[
				{"method":"entity.create","params":{"name":"A"}}]})"));
			CHECK(fine["result"]["dryRun"] == Json(true));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EditMethods: ops that are not allowed in a batch or carry reserved members are rejected before anything runs")
		{
			Test::AutomationFixture setup("EditBatchRefused");
			for (const std::string_view method : { "scene.save", "scene.open", "scene.new", "project.save", "project.upgrade", "session.shutdown", "edit.select" })
			{
				INFO(std::string(method));
				Json ops = ParseEditMethodJson(R"([{"method":"entity.create","params":{"name":"A"}}])");
				ops.push_back(Json{ { "method", std::string(method) }, { "params", Json::object() } });
				Json response = setup.Request("edit.batch", Json{ { "ops", ops } });
				CHECK(response["error"]["code"] == Json(-32602));
				CHECK(response["error"]["data"]["failedOp"] == Json(1));
				CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
			}

			Json reserved = setup.Request("edit.batch", ParseEditMethodJson(R"({"ops":[
				{"method":"entity.create","params":{"name":"A","dryRun":true}}]})"));
			CHECK(reserved["error"]["code"] == Json(-32602));
			CHECK(reserved["error"]["data"]["failedOp"] == Json(0));
			CHECK(reserved["error"]["data"]["issues"][0]["pointer"] == Json("/ops/0/params/dryRun"));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EditMethods: validator fixes inside a batch join its undo step")
		{
			Test::AutomationFixture setup("EditBatchValidate");
			Result<Json> batch = setup.Call("edit.batch", ParseEditMethodJson(R"({"label":"Cameras","ops":[
				{"method":"entity.create","params":{"name":"A","components":{"Camera":{"Primary":true}}}},
				{"method":"entity.create","params":{"name":"B","components":{"Camera":{"Primary":true}}}},
				{"method":"project.validate","params":{"scope":"scene","fix":true}}]})"));
			REQUIRE_MESSAGE(batch.has_value(), batch.error().ToString());
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == 1);
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == 0);
		}

		TEST_CASE("EditMethods: an undo whose command fails stops edit.undo and reports the count")
		{
			Test::AutomationFixture setup("EditUndoFailure");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "A" } }).has_value());
			REQUIRE(setup.Call("project.setSettings", Json{ { "patch", Json{ { "Name", "Renamed" } } } }).has_value());
			// The .eproj cannot be written now: its path is taken by a directory, on every platform.
			const std::filesystem::path projectFile = setup.GetEditor().GetProject().GetProjectFile();
			std::error_code error;
			std::filesystem::remove(projectFile, error);
			REQUIRE_FALSE(error);
			std::filesystem::create_directory(projectFile, error);
			REQUIRE_FALSE(error);
			Json response = setup.Request("edit.undo", Json{ { "steps", 2 } });
			CHECK(response.contains("error"));
			CHECK(response["error"]["data"]["undone"] == Json(0));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == 2); // the settings command stays applied
		}

		TEST_CASE("EditMethods: batches cannot nest and must hold an op")
		{
			Test::AutomationFixture setup("EditBatchInvalid");
			CHECK(setup.Call("edit.batch", ParseEditMethodJson(R"({"label":"Empty","ops":[]})")).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Call("edit.batch", ParseEditMethodJson(R"({"ops":[{"method":"edit.batch","params":{"ops":[]}}]})")).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Call("edit.batch", ParseEditMethodJson(R"({"ops":[{"method":"entity.create","params":{"name":"A","parent":{"$ref":"3.entity.id"}}}]})"))
					  .error()
					  .GetCode()
				== ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditMethods: edit.undo, edit.redo and edit.history move through the history")
		{
			Test::AutomationFixture setup("EditHistory");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "A" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "B" } }).has_value());
			Result<Json> undone = setup.Call("edit.undo", Json{ { "steps", 5 } });
			REQUIRE(undone.has_value());
			CHECK((*undone)["undone"] == Json(2));
			CHECK((*undone)["undoIndex"] == Json(0));
			CHECK((*undone)["redoLabel"].dump().contains("A"));
			Result<Json> redone = setup.Call("edit.redo", Json::object());
			REQUIRE(redone.has_value());
			CHECK((*redone)["redone"] == Json(1));

			Result<Json> history = setup.Call("edit.history", Json{ { "limit", 10 } });
			REQUIRE(history.has_value());
			REQUIRE((*history)["entries"].size() == 2);
			CHECK((*history)["entries"][0]["applied"] == Json(true));
			CHECK((*history)["entries"][1]["applied"] == Json(false));
			CHECK((*history)["entries"][0]["origin"] == Json("Agent"));
			CHECK((*history)["dirty"] == Json(true));
			CHECK(setup.Call("edit.undo", Json{ { "steps", 0 } }).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditMethods: edit.select and edit.getSelection keep the selection by id")
		{
			Test::AutomationFixture setup("EditSelect");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "A" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "B" } }).has_value());
			Result<Json> selected = setup.Call("edit.select", Json{ { "entities", Json::array({ "/B", "/A", "/B" }) } });
			REQUIRE(selected.has_value());
			CHECK((*selected)["selection"].size() == 2);
			CHECK((*selected)["selection"][0]["name"] == Json("B"));
			Result<Json> current = setup.Call("edit.getSelection", Json::object());
			REQUIRE(current.has_value());
			CHECK((*current)["selection"] == (*selected)["selection"]);
			CHECK(setup.Call("edit.select", Json{ { "entities", Json::array({ "/Nobody" }) } }).error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.GetEditor().GetSelection().size() == 2);
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == 2); // selection is not a command
		}

		// Docs/Decisions/0030-m19-contract.md decision 8 (A-NREF): an op named in the batch is referenced by its name, which
		// does not shift when an op is inserted before it; a name that is no identifier, or that an earlier op already has,
		// refuses the batch before any op runs, located at the op's name.
		TEST_CASE("EditMethods: edit.batch resolves a $ref to a named op and refuses a duplicate name before any op runs")
		{
			Test::AutomationFixture setup("EditBatchNamedRef");
			Result<Json> batch = setup.Call("edit.batch", ParseEditMethodJson(R"({"label":"Named","ops":[
				{"method":"entity.create","params":{"name":"Game"},"name":"game"},
				{"method":"entity.create","params":{"name":"Hud"}},
				{"method":"entity.create","params":{"name":"Board","parent":{"$ref":"game.entity.id"}},"name":"board"},
				{"method":"entity.create","params":{"name":"Cell","parent":{"$ref":"board.entity.id"}}}]})"));
			REQUIRE_MESSAGE(batch.has_value(), batch.error().ToString());
			REQUIRE((*batch)["results"].size() == 4);
			CHECK((*batch)["results"][2]["entity"]["path"] == Json("/Game/Board"));
			CHECK((*batch)["results"][3]["entity"]["path"] == Json("/Game/Board/Cell"));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == 1);

			const size_t entities = setup.GetEditor().GetScene().GetEntityCount();
			for (const std::string_view ops : { R"([{"method":"entity.create","params":{"name":"A"},"name":"a"},{"method":"entity.create","params":{"name":"B"},"name":"a"}])",
					 R"([{"method":"entity.create","params":{"name":"A"}},{"method":"entity.create","params":{"name":"B"},"name":"2b"}])" })
			{
				CAPTURE(ops);
				Json response = setup.Request("edit.batch", Json{ { "ops", ParseEditMethodJson(ops) } });
				REQUIRE_MESSAGE(response.contains("error"), response.dump());
				CHECK(response["error"]["code"] == Json(-32602));
				CHECK(response["error"]["data"]["failedOp"] == Json(1));
				CHECK(response["error"]["data"]["issues"][0]["pointer"] == Json("/ops/1/name"));
				CHECK(setup.GetEditor().GetScene().GetEntityCount() == entities);
			}

			// A $ref to a name only a later op has is a forward reference: the batch rolls back at that op.
			Json forward = setup.Request("edit.batch", ParseEditMethodJson(R"({"ops":[
				{"method":"entity.create","params":{"name":"A","parent":{"$ref":"later.entity.id"}}},
				{"method":"entity.create","params":{"name":"B"},"name":"later"}]})"));
			REQUIRE_MESSAGE(forward.contains("error"), forward.dump());
			CHECK(forward["error"]["code"] == Json(-32602));
			CHECK(forward["error"]["data"]["failedOp"] == Json(0));
			CHECK(setup.GetEditor().GetScene().GetEntityCount() == entities);
		}

		// Docs/Decisions/0030-m19-contract.md decision 7 (A-EREF): an op's component data resolves an entity path in the scene
		// as the batch has changed it so far, so it names an entity an earlier op of the same batch created; a path that still
		// names nothing rolls the batch back at that op.
		TEST_CASE("EditMethods: an op's component data resolves an entity path to an entity an earlier op created")
		{
			Test::AutomationFixture setup("EditBatchComponentPaths");
			const std::string path = "Assets/Scripts/Hud.luau";
			REQUIRE(setup.Call("script.write", Json{ { "path", path }, { "source", R"(
local Hud = { Fields = { Score = Field.Entity() } }
return Script.Define("Hud", Hud)
)" } })
					.has_value());

			Json ops = ParseEditMethodJson(R"([
				{"method":"entity.create","params":{"name":"Game"},"name":"game"},
				{"method":"entity.create","params":{"name":"ScoreText","parent":{"$ref":"game.entity.id"}},"name":"score"},
				{"method":"entity.create","params":{"name":"Hud","components":{"Script":{"Scripts":[{"Fields":{"Score":"/Game/ScoreText"}}]}}}}])");
			ops[2]["params"]["components"]["Script"]["Scripts"][0]["Script"] = path;
			Result<Json> batch = setup.Call("edit.batch", Json{ { "label", "Hud" }, { "ops", ops } });
			REQUIRE_MESSAGE(batch.has_value(), batch.error().ToString());
			const Json score = (*batch)["results"][1]["entity"]["id"];
			Result<Json> hud = setup.Call("entity.get", Json{ { "entity", "/Hud" } });
			REQUIRE_MESSAGE(hud.has_value(), hud.error().ToString());
			CHECK((*hud)["entity"]["components"]["Script"]["Scripts"][0]["Fields"]["Score"] == score);

			const Result<std::string> before = SceneSerializer::SaveToString(setup.GetEditor().GetScene());
			REQUIRE(before.has_value());
			Json missing = ParseEditMethodJson(R"([
				{"method":"entity.create","params":{"name":"Lives"}},
				{"method":"entity.update","params":{"entity":"/Hud","components":{"Script":{"Scripts":[{"Fields":{"Score":"/Game/Lives"}}]}}}}])");
			Json refused = setup.Request("edit.batch", Json{ { "ops", missing } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			CHECK(refused["error"]["data"]["errorCode"] == Json("NotFound"));
			CHECK(refused["error"]["data"]["failedOp"] == Json(1));
			CHECK(SceneSerializer::SaveToString(setup.GetEditor().GetScene()).value_or(std::string()) == *before);
		}
	}

}
