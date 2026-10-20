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

			// scene.open refreshes first (§7.3), which imports the new source and reports it as an asset that failed to import;
			// then the strict load refuses the file.
			{
				const Test::ExpectLog importFailure(LogLevel::Error, "ASSET_IMPORT_FAILED Assets/Scenes/Broken.scene");
				const Result<Json> strict = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Broken.scene" } });
				REQUIRE_FALSE(strict.has_value());
				CHECK(strict.error().GetCode() == ErrorCode::Validation);
			}
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

		// Docs/Decisions/0031-m20-contract.md decision 14 (E-SCENES): keepOpen opens a scene as an additional open scene and
		// makes it active, so a dirty scene needs neither save nor discardChanges; a path that is already open, active or
		// not, needs reload, which re-reads that scene in its tab position and makes it active.
		TEST_CASE("SceneMethods: scene.open and scene.new with keepOpen add an open scene and make it active" * doctest::skip(true))
		{
			Test::AutomationFixture setup("SceneKeepOpen");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "InMain" } }).has_value());
			const Result<Json> other = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Other.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(other.has_value(), other.error().ToString());
			CHECK((*other)["scene"]["path"] == Json("Assets/Scenes/Other.scene"));
			CHECK((*other)["id"].is_number_unsigned());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "InOther" } }).has_value());
			CHECK(setup.GetEditor().GetScene().GetName() == "Other");

			const Result<Json> listed = setup.Call("scene.list", Json::object());
			REQUIRE(listed.has_value());
			REQUIRE((*listed)["scenes"].size() == 2);
			CHECK((*listed)["scenes"][0]["scene"]["path"] == Json("Assets/Scenes/Main.scene"));
			CHECK((*listed)["scenes"][0]["scene"]["dirty"] == Json(true));
			CHECK((*listed)["scenes"][0]["active"] == Json(false));
			CHECK((*listed)["scenes"][1]["active"] == Json(true));
			CHECK((*listed)["scenes"][1]["id"] == (*other)["id"]);

			// keepOpen keeps every open scene, so save and discardChanges have nothing to do.
			const Result<Json> withSave = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Third.scene" }, { "keepOpen", true }, { "save", true } });
			REQUIRE_FALSE(withSave.has_value());
			CHECK(withSave.error().GetCode() == ErrorCode::InvalidArgument);

			// An open path needs reload, whether it is active or not; reload re-reads it in place and activates it.
			const Result<Json> again = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "keepOpen", true } });
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::InvalidState);
			const Result<Json> reloaded =
				setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true }, { "discardChanges", true } });
			REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
			CHECK((*reloaded)["scene"]["entityCount"] == Json(0));
			const Result<Json> after = setup.Call("scene.list", Json::object());
			REQUIRE(after.has_value());
			REQUIRE((*after)["scenes"].size() == 2);
			CHECK((*after)["scenes"][0]["scene"]["path"] == Json("Assets/Scenes/Main.scene"));
			CHECK((*after)["scenes"][0]["active"] == Json(true));
			CHECK((*after)["scenes"][1]["scene"]["dirty"] == Json(true));

			// Without keepOpen, scene.open replaces the active scene in its tab position and leaves the others open. Closing
			// the last tab activates the one before it.
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Third.scene" }, { "keepOpen", true } }).has_value());
			REQUIRE(setup.Call("scene.close", Json::object()).has_value());
			CHECK(setup.GetEditor().GetScene().GetName() == "Other");
			REQUIRE(setup.Call("scene.activate", Json{ { "scene", "Assets/Scenes/Main.scene" } }).has_value());
			REQUIRE(setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Third.scene" } }).has_value());
			const Result<Json> replaced = setup.Call("scene.list", Json::object());
			REQUIRE(replaced.has_value());
			REQUIRE((*replaced)["scenes"].size() == 2);
			CHECK((*replaced)["scenes"][0]["scene"]["path"] == Json("Assets/Scenes/Third.scene"));
			CHECK((*replaced)["scenes"][1]["scene"]["path"] == Json("Assets/Scenes/Other.scene"));
		}

		// Decision 14: scene.activate switches the active scene by path or id, and scene.close closes the addressed scene with
		// scene.open's save-or-discard rule, activating its neighbour. Unknown and malformed targets are refused at /scene.
		TEST_CASE("SceneMethods: scene.activate and scene.close manage the open scenes" * doctest::skip(true))
		{
			Test::AutomationFixture setup("SceneActivateClose");
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Unsaved" } }).has_value());

			const Result<Json> byPath = setup.Call("scene.activate", Json{ { "scene", "Assets/Scenes/Main.scene" } });
			REQUIRE_MESSAGE(byPath.has_value(), byPath.error().ToString());
			CHECK((*byPath)["scene"]["scene"]["path"] == Json("Assets/Scenes/Main.scene"));
			CHECK(setup.GetEditor().GetScene().GetName() == "Main");
			const Result<Json> byId = setup.Call("scene.activate", Json{ { "scene", (*second)["id"] } });
			REQUIRE(byId.has_value());
			CHECK(setup.GetEditor().GetScene().GetName() == "Second");

			CHECK(setup.Call("scene.activate", Json::object()).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Call("scene.activate", Json{ { "scene", "Assets/Scenes/Missing.scene" } }).error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Call("scene.activate", Json{ { "scene", "../Escape.scene" } }).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Call("scene.activate", Json{ { "scene", true } }).error().GetCode() == ErrorCode::InvalidArgument);

			// The dirty second scene needs save or discardChanges; closing it activates Main.
			const Result<Json> refused = setup.Call("scene.close", Json::object());
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			const Result<Json> closed = setup.Call("scene.close", Json{ { "discardChanges", true } });
			REQUIRE_MESSAGE(closed.has_value(), closed.error().ToString());
			CHECK((*closed)["closed"]["path"] == Json("Assets/Scenes/Second.scene"));
			CHECK((*closed)["file"] == Json(""));
			CHECK(setup.GetEditor().GetScene().GetName() == "Main");
			CHECK((*closed)["activeId"] != Json(0));
			// The closed scene's id names nothing any more.
			CHECK(setup.Call("scene.activate", Json{ { "scene", (*second)["id"] } }).error().GetCode() == ErrorCode::NotFound);

			// Closing the last open scene leaves none.
			const Result<Json> last = setup.Call("scene.close", Json{ { "scene", "Assets/Scenes/Main.scene" } });
			REQUIRE(last.has_value());
			CHECK((*last)["activeId"] == Json(0));
			CHECK_FALSE(setup.GetEditor().HasScene());
			const Result<Json> empty = setup.Call("scene.list", Json::object());
			REQUIRE(empty.has_value());
			CHECK((*empty)["scenes"].empty());
		}
	}

}
