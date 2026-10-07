#include "TestsPCH.h"

#include "EditorCore/Automation/SceneMethods.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <map>
#include <string>

namespace Engine {

	static Json ParseSceneMethodJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("SceneMethods: scene.new writes and opens a scene")
		{
			Test::AutomationFixture setup("SceneNew", false);
			Result<Json> created = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Level1.scene" } });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK((*created)["scene"]["path"] == Json("Assets/Scenes/Level1.scene"));
			CHECK((*created)["scene"]["name"] == Json("Level1"));
			CHECK((*created)["scene"]["dirty"] == Json(false));
			CHECK(FileSystem::Exists(setup.GetEditorFixture().GetProjectRoot() / "Assets/Scenes/Level1.scene"));
			CHECK(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Level1.scene" } }).error().GetCode() == ErrorCode::AlreadyExists);
			CHECK(setup.Call("scene.new", Json{ { "path", "../Escape.scene" } }).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/NoExtension" } }).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("SceneMethods: scene.open with a dirty scene requires save or discardChanges")
		{
			Test::AutomationFixture setup("SceneOpenDirty");
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Other.scene" }, { "save", true } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Unsaved" } }).has_value());

			const Result<Json> refused = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			// §13.5: with a dirty scene exactly one of the two, else InvalidState; both together for a clean scene is a params error.
			const Result<Json> both = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "save", true }, { "discardChanges", true } });
			REQUIRE_FALSE(both.has_value());
			CHECK(both.error().GetCode() == ErrorCode::InvalidState);
			const Result<Json> bothNew = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Third.scene" }, { "save", true }, { "discardChanges", true } });
			REQUIRE_FALSE(bothNew.has_value());
			CHECK(bothNew.error().GetCode() == ErrorCode::InvalidState);

			REQUIRE(setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "discardChanges", true } }).has_value());
			const Result<Json> bothClean =
				setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Other.scene" }, { "save", true }, { "discardChanges", true } });
			REQUIRE_FALSE(bothClean.has_value());
			CHECK(bothClean.error().GetCode() == ErrorCode::InvalidArgument);
			Result<Json> other = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Other.scene" } });
			REQUIRE(other.has_value());
			CHECK((*other)["scene"]["entityCount"] == Json(0)); // the unsaved entity was discarded
			CHECK(setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Other.scene" } }).error().GetCode() == ErrorCode::InvalidState);
			CHECK(setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Other.scene" }, { "reload", true } }).has_value());
		}

		TEST_CASE("SceneMethods: scene.open with repair loads a defective file and reports the repairs")
		{
			Test::AutomationFixture setup("SceneOpenRepair");
			const Result<std::string> defective = Test::ReadTestDataText("Scenes/Invalid/DuplicateIds.scene");
			REQUIRE(defective.has_value());
			const std::filesystem::path file = setup.GetEditorFixture().GetProjectRoot() / "Assets/Scenes/Broken.scene";
			REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(defective->data(), defective->size()))).has_value());

			const Result<Json> strict = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Broken.scene" } });
			REQUIRE_FALSE(strict.has_value());
			CHECK(strict.error().GetCode() == ErrorCode::Validation);
			// Each load diagnostic is logged as "'<file>' <pointer>: <message> (<code>)" (Utils::FormatLoadDiagnostic).
			const Test::ExpectLog logged(LogLevel::Warn, "'Assets/Scenes/Broken.scene' /Entities/");
			Result<Json> repaired = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Broken.scene" }, { "repair", true } });
			REQUIRE(repaired.has_value());
			CHECK((*repaired)["repairs"].size() >= 1);
			CHECK((*repaired)["repairs"][0]["code"] == Json("SCENE_DUPLICATE_ID"));
			CHECK((*repaired)["scene"]["dirty"] == Json(true));
		}

		TEST_CASE("SceneMethods: scene.save writes the scene and records provenance")
		{
			Test::AutomationFixture setup("SceneSave");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Board" } }).has_value());
			Result<Json> saved = setup.Call("scene.save", Json::object());
			REQUIRE(saved.has_value());
			CHECK((*saved)["file"] == Json("Assets/Scenes/Main.scene"));
			CHECK((*saved)["scene"]["dirty"] == Json(false));
			CHECK(FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "Assets/Scenes/Main.scene").value_or("").contains("\"Board\""));
			const ProvenanceRecorder* provenance = setup.GetEditor().GetProvenance();
			REQUIRE(provenance != nullptr);
			const ProvenanceEntry* entry = provenance->Find("Assets/Scenes/Main.scene");
			REQUIRE(entry != nullptr);
			CHECK(entry->Method == "scene.save");

			Result<Json> savedAs = setup.Call("scene.save", Json{ { "path", "Assets/Scenes/Copy.scene" } });
			REQUIRE(savedAs.has_value());
			CHECK((*savedAs)["scene"]["path"] == Json("Assets/Scenes/Copy.scene"));
		}

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

		TEST_CASE("SceneMethods: scene.diff reports per-entity patches against the saved file and a revision")
		{
			Test::AutomationFixture setup("SceneDiff");
			// scene.diff revisions are the editor's (EditorContext::GetRevision, ADR 0008 decision 28), not the Scene object's.
			const uint64_t start = setup.GetEditor().GetRevision();
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Board" } }).has_value());
			Result<Json> saved = setup.Call("scene.diff", Json{ { "against", "saved" } });
			REQUIRE(saved.has_value());
			REQUIRE((*saved)["entities"].size() == 1);
			CHECK((*saved)["entities"][0]["change"] == Json("Created"));
			CHECK((*saved)["entities"][0]["name"] == Json("Board"));

			REQUIRE(setup.Call("entity.update", Json{ { "entity", "/Board" }, { "name", "Grid" } }).has_value());
			Result<Json> sinceStart = setup.Call("scene.diff", Json{ { "against", "revision" }, { "revision", start } });
			REQUIRE(sinceStart.has_value());
			CHECK((*sinceStart)["fromRevision"] == Json(start));
			CHECK((*sinceStart)["entities"][0]["change"] == Json("Created"));
			CHECK(setup.Call("scene.diff", Json{ { "against", "revision" }, { "revision", 999999 } }).error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("SceneMethods: scene.diff rebuilds revisions on both sides of the undo position, through batches")
		{
			Test::AutomationFixture setup("SceneDiffBothSides");
			const uint64_t start = setup.GetEditor().GetRevision();
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Board" } }).has_value());
			const uint64_t created = setup.GetEditor().GetRevision();
			Json ops = Json::array();
			ops.push_back(Json{ { "method", "entity.update" }, { "params", Json{ { "entity", "/Board" }, { "name", "Grid" } } } });
			ops.push_back(Json{ { "method", "entity.create" }, { "params", Json{ { "name", "Piece" } } } });
			REQUIRE(setup.Call("edit.batch", Json{ { "label", "Rename and add" }, { "ops", ops } }).has_value());
			const uint64_t batched = setup.GetEditor().GetRevision();
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());

			// The batch (one CompositeCommand) is undone, so its revision lies in the redo branch: replayed forward.
			Result<Json> ahead = setup.Call("scene.diff", Json{ { "against", "revision" }, { "revision", batched } });
			REQUIRE_MESSAGE(ahead.has_value(), ahead.error().ToString());
			CHECK((*ahead)["fromRevision"] == Json(batched));
			REQUIRE((*ahead)["entities"].size() == 2);
			std::map<std::string, std::string> changes;
			for (Json& entity : (*ahead)["entities"])
				changes[entity["name"].dump()] = entity["change"].dump();
			CHECK(changes["\"Board\""] == "\"Modified\"");
			CHECK(changes["\"Piece\""] == "\"Destroyed\"");

			// The current state is the state after the creation, so nothing differs from that revision.
			Result<Json> same = setup.Call("scene.diff", Json{ { "against", "revision" }, { "revision", created } });
			REQUIRE(same.has_value());
			CHECK((*same)["entities"].empty());

			// Before the creation: reverted backwards.
			Result<Json> behind = setup.Call("scene.diff", Json{ { "against", "revision" }, { "revision", start } });
			REQUIRE(behind.has_value());
			REQUIRE((*behind)["entities"].size() == 1);
			CHECK((*behind)["entities"][0]["change"] == Json("Created"));
			CHECK((*behind)["entities"][0]["name"] == Json("Board"));
			CHECK(setup.GetEditor().GetRevision() > batched);
		}
	}

}
