#include "TestsPCH.h"

#include "EditorCore/Automation/SceneMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/TestData.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

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
		TEST_CASE("SceneMethods: scene.open and scene.new with keepOpen add an open scene and make it active")
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

			// With keepOpen, scene.open opens a file no tab holds at the end of the tab order as the active scene, and every
			// open scene keeps its changes.
			const Result<Json> opened = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			CHECK((*opened)["scene"]["path"] == Json("Assets/Scenes/Main.scene"));
			CHECK(setup.GetEditor().GetScene().GetName() == "Main");
			const Result<Json> appended = setup.Call("scene.list", Json::object());
			REQUIRE(appended.has_value());
			REQUIRE((*appended)["scenes"].size() == 3);
			CHECK((*appended)["scenes"][1]["scene"]["dirty"] == Json(true));
			CHECK((*appended)["scenes"][2]["scene"]["path"] == Json("Assets/Scenes/Main.scene"));
			CHECK((*appended)["scenes"][2]["active"] == Json(true));
			CHECK((*appended)["scenes"][2]["id"] == (*opened)["id"]);
		}

		// Decision 14: scene.activate switches the active scene by path or id, and scene.close closes the addressed scene with
		// scene.open's save-or-discard rule, activating its neighbour. Unknown and malformed targets are refused at /scene.
		TEST_CASE("SceneMethods: scene.activate and scene.close manage the open scenes")
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

		// Decision 14: what the open-scene methods refuse before they change anything: keepOpen with save, discardChanges or
		// reload (keepOpen keeps every scene open, reload reads one again in its own tab), scene.close with both flags, and a
		// scene target that is malformed or names no open scene.
		TEST_CASE("SceneMethods: the open-scene methods refuse contradictory flags and targets that name no open scene")
		{
			Test::AutomationFixture setup("SceneOpenRefusals");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Unsaved" } }).has_value());
			for (const std::string_view flag : { "save", "discardChanges" })
			{
				INFO(std::string(flag));
				Json added{ { "path", "Assets/Scenes/Third.scene" }, { "keepOpen", true } };
				added[std::string(flag)] = true;
				CHECK(setup.Call("scene.new", added).error().GetCode() == ErrorCode::InvalidArgument);
				added["path"] = "Assets/Scenes/Other.scene";
				CHECK(setup.Call("scene.open", added).error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK_FALSE(FileSystem::Exists(setup.GetEditorFixture().GetProjectRoot() / "Assets/Scenes/Third.scene"));
			const Result<Json> reloadKeepOpen = setup.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "keepOpen", true }, { "reload", true } });
			REQUIRE_FALSE(reloadKeepOpen.has_value());
			CHECK(reloadKeepOpen.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Call("scene.close", Json{ { "save", true }, { "discardChanges", true } }).error().GetCode() == ErrorCode::InvalidArgument);

			const Result<Json> untargeted = setup.Call("scene.activate", Json::object());
			REQUIRE_FALSE(untargeted.has_value());
			CHECK(untargeted.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(untargeted.error().GetIssues().empty());
			CHECK(untargeted.error().GetIssues().front().JsonPointer == "/scene");
			for (const Json& malformed : { Json("../Escape.scene"), Json("Assets/Scenes/Main.txt"), Json(0), Json(true) })
			{
				CAPTURE(malformed.dump());
				CHECK(setup.Call("scene.activate", Json{ { "scene", malformed } }).error().GetCode() == ErrorCode::InvalidArgument);
			}
			for (const Json& missing : { Json("Assets/Scenes/Missing.scene"), Json(4000) })
			{
				CAPTURE(missing.dump());
				const Result<Json> notFound = setup.Call("scene.activate", Json{ { "scene", missing } });
				REQUIRE_FALSE(notFound.has_value());
				CHECK(notFound.error().GetCode() == ErrorCode::NotFound);
				REQUIRE_FALSE(notFound.error().GetIssues().empty());
				CHECK(notFound.error().GetIssues().front().JsonPointer == "/scene");
				CHECK(setup.Call("scene.close", Json{ { "scene", missing }, { "discardChanges", true } }).error().GetCode() == ErrorCode::NotFound);
				CHECK(setup.Call("scene.tree", Json{ { "scene", missing } }).error().GetCode() == ErrorCode::NotFound);
			}
			// Nothing changed: the active scene keeps its unsaved entity.
			CHECK(setup.GetEditor().GetScene().GetName() == "Main");
			CHECK(setup.GetEditor().IsSceneDirty());
			CHECK(setup.GetEditor().GetScene().FindEntityByPath("/Unsaved").IsValid());
		}

		// Decision 14: no two open scenes share a path, and a scene never saved has no file for save to write. scene.save
		// {path} and scene.new refuse the path of another open scene (scene.new also when that scene's file is gone), and
		// scene.close {save} refuses a dirty scene that was never saved; none of them changes a file or the open scenes.
		TEST_CASE("SceneMethods: scene.save and scene.new refuse the path of another open scene and scene.close save refuses a scene never saved")
		{
			Test::AutomationFixture setup("ScenePathRefusals");
			EditorContext& editor = setup.GetEditor();
			REQUIRE(setup.Call("entity.create", Json{ { "name", "InMain" } }).has_value());
			REQUIRE(setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "InSecond" } }).has_value());
			const std::filesystem::path root = setup.GetEditorFixture().GetProjectRoot();
			const auto read = [&root](std::string_view file)
			{
				return FileSystem::ReadText(root / file).value_or(std::string());
			};
			// The open scenes as scene.list reports them, in tab order: id, path, active, dirty and entity count.
			const auto listed = [&setup]()
			{
				const Result<Json> list = setup.Call("scene.list", Json::object());
				REQUIRE(list.has_value());
				Json scenes = Json::array();
				for (const Json& entry : (*list)["scenes"])
				{
					scenes.push_back(Json::array({ entry["id"], entry["scene"]["path"], entry["active"], entry["scene"]["dirty"], entry["scene"]["entityCount"] }));
				}
				return scenes;
			};
			const std::string mainFile = read("Assets/Scenes/Main.scene");
			const std::string secondFile = read("Assets/Scenes/Second.scene");
			REQUIRE_FALSE(mainFile.empty());
			const Json before = listed();
			REQUIRE(before.size() == 2);

			// Second, the active scene, saved to Main's path.
			const Result<Json> savedAs = setup.Call("scene.save", Json{ { "path", "Assets/Scenes/Main.scene" } });
			REQUIRE_FALSE(savedAs.has_value());
			CHECK(savedAs.error().GetCode() == ErrorCode::InvalidState);
			CHECK(read("Assets/Scenes/Main.scene") == mainFile);
			CHECK(read("Assets/Scenes/Second.scene") == secondFile);
			CHECK(listed() == before);

			// Main's file is deleted while Main stays open with its path: a new scene there, replacing Second, is refused.
			REQUIRE(FileSystem::Remove(root / "Assets/Scenes/Main.scene").has_value());
			const Result<Json> created = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Main.scene" }, { "discardChanges", true } });
			REQUIRE_FALSE(created.has_value());
			CHECK(created.error().GetCode() == ErrorCode::InvalidState);
			CHECK_FALSE(FileSystem::Exists(root / "Assets/Scenes/Main.scene"));
			CHECK(read("Assets/Scenes/Second.scene") == secondFile);
			CHECK(listed() == before);

			// A dirty scene never saved.
			const Result<OpenSceneId> untitled = editor.AddScene(editor.CreateScene("Untitled"), std::nullopt);
			REQUIRE(untitled.has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Unsaved" } }).has_value());
			const Json withUntitled = listed();
			const Result<Json> closed = setup.Call("scene.close", Json{ { "save", true } });
			REQUIRE_FALSE(closed.has_value());
			CHECK(closed.error().GetCode() == ErrorCode::InvalidState);
			CHECK(listed() == withUntitled);
			CHECK(editor.GetActiveSceneId() == *untitled);
			CHECK(editor.GetScene().FindEntityByPath("/Unsaved").IsValid());
			CHECK(read("Assets/Scenes/Second.scene") == secondFile);
		}

		// Decision 14: without an open scene, scene.list lists none and scene.close has nothing to close.
		TEST_CASE("SceneMethods: without an open scene scene.list is empty and scene.close is refused")
		{
			Test::AutomationFixture setup("SceneNoneOpen", false);
			const Result<Json> listed = setup.Call("scene.list", Json::object());
			REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
			CHECK((*listed)["scenes"].empty());
			CHECK(setup.Call("scene.close", Json::object()).error().GetCode() == ErrorCode::InvalidState);
			CHECK(setup.Call("scene.activate", Json{ { "scene", "Assets/Scenes/Main.scene" } }).error().GetCode() == ErrorCode::NotFound);
		}

		// Decision 14's deny-mutations rule: scene.close {discardChanges} of a dirty scene drops changes no write boundary sees,
		// so the policy refuses it, and the scene keeps them.
		TEST_CASE("SceneMethods: deny-mutations refuses scene.close discarding the active scene's unsaved changes")
		{
			Test::AutomationFixture setup("SceneCloseDenied");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "HumanEdit" } }).has_value());
			const uint64_t revision = setup.GetEditor().GetRevision();
			setup.GetClient().GetServer().SetEditorPolicy({ .DenyMutations = true });
			const Result<Json> denied = setup.Call("scene.close", Json{ { "discardChanges", true } });
			REQUIRE_FALSE(denied.has_value());
			CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
			CHECK(setup.GetEditor().HasScene());
			CHECK(setup.GetEditor().IsSceneDirty());
			CHECK(setup.GetEditor().GetRevision() == revision);
			CHECK(setup.GetEditor().GetScene().FindEntityByPath("/HumanEdit").IsValid());
		}
	}

}
