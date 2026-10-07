#include "TestsPCH.h"

#include "EditorCore/Automation/EditMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/SceneSerializer.h"
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
	}

}
