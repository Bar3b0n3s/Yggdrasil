#include "TestsPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		VfsPath OpenScenesPath(std::string_view text)
		{
			Result<VfsPath> path = VfsPath::Parse(text);
			REQUIRE_MESSAGE(path.has_value(), path.error().ToString());
			return *path;
		}

		// Writes an empty scene named after `path` and opens it as an additional open scene (EditorContext::AddScene).
		OpenSceneId AddOpenScene(EditorContext& editor, std::string_view path)
		{
			const VfsPath file = OpenScenesPath(path);
			Scope<Scene> scene = editor.CreateScene(std::string(file.GetStem()));
			const Result<std::string> text = SceneSerializer::SaveToString(*scene);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			REQUIRE(editor.WriteProjectFile(file, std::as_bytes(std::span(text->data(), text->size()))).has_value());
			const Result<OpenSceneId> added = editor.AddScene(std::move(scene), file);
			REQUIRE_MESSAGE(added.has_value(), added.error().ToString());
			return *added;
		}

		// Creates an entity named `name` in the addressed scene as one undoable edit; returns its id.
		UUID CreateEdited(EditorContext& editor, std::string name)
		{
			SceneEdit edit(editor, "Create " + name);
			const UUID id = editor.GetScene().CreateEntity(std::move(name)).GetUUID();
			REQUIRE(edit.Commit().has_value());
			return id;
		}

		std::string OpenScenesName(const Json& entity)
		{
			return JsonReader(entity["name"]).ReadString().value_or(std::string());
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0031-m20-contract.md decision 13 (E-SCENES; Roadmap M20 acceptance "undo is isolated per open scene"):
		// every open scene is its own document. Edits, undo, the dirty flag, the selection and the revision of one never
		// touch another, switching the active scene keeps each scene's state, and revisions never repeat across them.
		TEST_CASE("EditorContext: each open scene keeps its own history, dirty flag, selection and revision" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("OpenScenesIsolated");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(main != NoOpenScene);
			const UUID inMain = CreateEdited(editor, "InMain");
			editor.SetSelection({ inMain });
			const uint64_t mainRevision = editor.GetRevision();

			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			CHECK(second != main);
			CHECK(editor.GetActiveSceneId() == second);
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ main, second });
			CHECK(editor.GetScene().GetName() == "Second");
			CHECK_FALSE(editor.IsSceneDirty());
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK(editor.GetSelection().empty());
			// A revision names one state of one scene: the new scene's never equals one the first reported.
			CHECK(editor.GetRevision() > mainRevision);

			const UUID inSecond = CreateEdited(editor, "InSecond");
			CHECK(editor.IsSceneDirty());
			const Result<OpenSceneInfo> mainInfo = editor.DescribeOpenScene(main);
			REQUIRE(mainInfo.has_value());
			CHECK(mainInfo->Revision == mainRevision);
			CHECK(mainInfo->Dirty);
			CHECK_FALSE(mainInfo->Active);
			CHECK(mainInfo->EntityCount == 1);
			CHECK(editor.IsAnySceneDirty());

			// Undo in the second scene leaves the first as it was.
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK_FALSE(editor.GetScene().FindEntityByID(inSecond).IsValid());
			CHECK(editor.DescribeOpenScene(main)->Revision == mainRevision);
			REQUIRE(editor.FindScene(main) != nullptr);
			CHECK(editor.FindScene(main)->FindEntityByID(inMain).IsValid());

			// Switching back restores the first scene's history, dirty flag and selection.
			REQUIRE(editor.ActivateScene(main).has_value());
			CHECK(editor.GetScene().GetName() == "Main");
			CHECK(editor.GetHistory().CanUndo());
			CHECK(editor.IsSceneDirty());
			CHECK(std::vector<UUID>(editor.GetSelection().begin(), editor.GetSelection().end()) == std::vector<UUID>{ inMain });
			CHECK(editor.GetRevision() == mainRevision);
			CHECK(editor.FindOpenScene(OpenScenesPath("project://Assets/Scenes/Second.scene")) == second);

			// Closing the active scene activates its neighbour; an id never names another scene afterwards.
			REQUIRE(editor.CloseScene(main).has_value());
			CHECK(editor.GetOpenScenes() == std::vector<OpenSceneId>{ second });
			CHECK(editor.GetActiveSceneId() == second);
			CHECK(editor.FindScene(main) == nullptr);
			CHECK(editor.DescribeOpenScene(main).error().GetCode() == ErrorCode::NotFound);
			CHECK(editor.ActivateScene(main).error().GetCode() == ErrorCode::NotFound);
		}

		// Decision 13: GetScene() is the active scene for every caller outside a targeted request; an EditorSceneTargetScope
		// makes another open scene the addressed one, and commands executed inside it land in that scene and its history,
		// without changing the active scene.
		TEST_CASE("EditorContext: a scene target scope addresses another open scene and leaves the active one" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("OpenScenesTargetScope");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Main.scene");
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			REQUIRE(main != NoOpenScene);
			const OpenSceneId second = AddOpenScene(editor, "project://Assets/Scenes/Second.scene");
			REQUIRE(editor.GetActiveSceneId() == second);

			UUID targeted;
			{
				Result<Scope<EditorSceneTargetScope>> scope = EditorSceneTargetScope::Begin(editor, main);
				REQUIRE_MESSAGE(scope.has_value(), scope.error().ToString());
				CHECK((*scope)->GetScene() == main);
				CHECK(editor.GetAddressedSceneId() == main);
				CHECK(editor.GetActiveSceneId() == second);
				CHECK(editor.GetScene().GetName() == "Main");
				targeted = CreateEdited(editor, "Targeted");
				CHECK(editor.GetHistory().CanUndo());
				// Scopes do not nest, and the active scene cannot change under one.
				CHECK(EditorSceneTargetScope::Begin(editor, second).error().GetCode() == ErrorCode::InvalidState);
				CHECK(editor.ActivateScene(second).error().GetCode() == ErrorCode::InvalidState);
			}
			CHECK(editor.GetAddressedSceneId() == second);
			CHECK(editor.GetScene().GetName() == "Second");
			CHECK_FALSE(editor.GetHistory().CanUndo());
			CHECK_FALSE(editor.GetScene().FindEntityByID(targeted).IsValid());
			REQUIRE(editor.FindScene(main) != nullptr);
			CHECK(editor.FindScene(main)->FindEntityByID(targeted).IsValid());
			CHECK(editor.DescribeOpenScene(main)->Dirty);
			CHECK(EditorSceneTargetScope::Begin(editor, OpenSceneId(9999)).error().GetCode() == ErrorCode::NotFound);
		}

		// Decision 15 (Roadmap M20 acceptance "a prefab update rebuilds every open scene"): applying an edit of an instance
		// in one open scene rebuilds the prefab's instances in another, as an entry of that scene's own history, so undo
		// there reverts only that scene; requests address the second scene through the scene target "scene".
		TEST_CASE("EditorContext: a prefab update rebuilds its instances in every open scene, each in its own history" * doctest::skip(true))
		{
			Test::AutomationFixture setup("OpenScenesPrefabUpdate");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Lamp" } }).has_value());
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Bulb" }, { "parent", "/Lamp" } }).has_value());
			REQUIRE(setup.Call("prefab.create", Json{ { "entity", "/Lamp" }, { "path", "Assets/Prefabs/Lamp.prefab" }, { "replaceWithInstance", true } }).has_value());
			const Result<Json> second = setup.Call("scene.new", Json{ { "path", "Assets/Scenes/Second.scene" }, { "keepOpen", true } });
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			REQUIRE(setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Lamp.prefab" }, { "name", "Copy" } }).has_value());
			const Result<Json> secondHistory = setup.Call("edit.history", Json::object());
			REQUIRE(secondHistory.has_value());
			const size_t secondEntries = (*secondHistory)["entries"].size();

			// Back in Main, rename the bulb and apply it to the prefab.
			const Json main{ { "scene", "Assets/Scenes/Main.scene" } };
			REQUIRE(setup.Call("scene.activate", main).has_value());
			REQUIRE(setup.Call("entity.update", Json{ { "entity", "/Lamp/Bulb" }, { "name", "Globe" } }).has_value());
			const Result<Json> applied = setup.Call("prefab.apply", Json{ { "instance", "/Lamp" } });
			REQUIRE_MESSAGE(applied.has_value(), applied.error().ToString());

			// The second scene's instance followed, recorded in its own history.
			const Json inSecond{ { "scene", "Assets/Scenes/Second.scene" } };
			Json tree = inSecond;
			tree["format"] = "json";
			const Result<Json> secondTree = setup.Call("scene.tree", tree);
			REQUIRE(secondTree.has_value());
			std::vector<std::string> names;
			for (const Json& entity : (*secondTree)["entities"])
				names.push_back(OpenScenesName(entity));
			CHECK(names == std::vector<std::string>{ "Copy", "Globe" });
			const Result<Json> afterHistory = setup.Call("edit.history", inSecond);
			REQUIRE(afterHistory.has_value());
			REQUIRE((*afterHistory)["entries"].size() == secondEntries + 1);
			CHECK(JsonReader((*afterHistory)["entries"].back()["label"]).ReadString().value_or(std::string()).contains("Update Prefab Instances"));

			// Undo there reverts only the second scene; Main keeps its applied edit.
			REQUIRE(setup.Call("edit.undo", inSecond).has_value());
			const Result<Json> undone = setup.Call("scene.tree", tree);
			REQUIRE(undone.has_value());
			CHECK(OpenScenesName((*undone)["entities"][1]) == "Bulb");
			CHECK(setup.GetEditor().GetScene().FindEntityByPath("/Lamp/Globe").IsValid());
		}
	}

}
