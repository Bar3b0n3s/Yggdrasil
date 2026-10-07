#include "TestsPCH.h"

#include "EditorCore/Automation/PrefabMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

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

		// Hot-reload frames: a poll every 0.5 s from `start` (debounced, so a change is reported by the second poll that sees
		// it), each followed by the main-thread queue, then the asset manager's jobs.
		void RunHotReloadFrames(EditorContext& editor, double start)
		{
			for (double seconds = start; seconds <= start + 2.0; seconds += 0.5)
			{
				editor.Update(seconds);
				static_cast<void>(editor.GetEngine().GetMainThreadQueue().Drain());
			}
			editor.GetAssets().WaitIdle();
		}

		// The SceneChangedOnDisk events so far.
		std::vector<EngineEvent> ReadSceneChangedEvents(EditorContext& editor)
		{
			const EngineEventType types[] = { EngineEventType::SceneChangedOnDisk };
			return editor.GetEngine().GetEventLog().Read(0, types, 100).Events;
		}

		// The canonical JSON of the open scene's entity at `path`.
		Json CaptureEntity(EditorContext& editor, std::string_view path)
		{
			const Entity entity = editor.GetScene().FindEntityByPath(path);
			REQUIRE(entity.IsValid());
			Result<Json> json = SceneSerializer::EntityToJson(entity);
			REQUIRE(json.has_value());
			return std::move(*json);
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("PrefabMethods: create, instantiate, apply, revert and unpack are undoable steps")
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

		TEST_CASE("PrefabMethods: an edit of an instance member is recorded as an override and survives reopening the scene")
		{
			Test::AutomationFixture setup("PrefabMemberOverride");
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Lamp", "components": {
				"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			const auto overrides = [&setup]()
			{
				return setup.GetEditor().GetScene().FindEntityByPath("/Lamp").GetComponent<PrefabInstanceComponent>().Overrides;
			};
			REQUIRE(overrides().empty());

			// §5.5: the edit of a member records a field-level override in the same undo step.
			Json updated = CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			REQUIRE(overrides().size() == 1);
			CHECK(overrides()[0].Kind == PrefabOverrideKind::Field);
			CHECK(overrides()[0].Component == "MeshRenderer");
			CHECK(overrides()[0].Field == "CastShadows");
			CHECK(updated["undoIndex"] == Json(setup.GetEditor().GetHistory().GetCurrentSequence()));

			// Reopening the scene rebuilds the instance as prefab + overrides (ADR 0010 decision 11), which keeps the edit.
			CallOrFail(setup, "scene.save", Json::object());
			CallOrFail(setup, "scene.open", ParsePrefabMethodJson(R"({"path": "Assets/Scenes/Main.scene", "reload": true})"));
			Json bulb = CallOrFail(setup, "entity.get", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "components": ["MeshRenderer"]})"));
			CHECK(bulb["entity"]["components"]["MeshRenderer"]["CastShadows"] == Json(false));
			REQUIRE(overrides().size() == 1);

			// Undoing the edit removes the record with the value.
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "components": {"MeshRenderer": {"Visible": false}}})"));
			CHECK(overrides().size() == 2);
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(overrides().size() == 1);
		}

		TEST_CASE("HotReload: a changed prefab used by the open scene raises SceneChangedOnDisk and is not applied")
		{
			// §7.5 race rule 3: a prefab the open scene instantiates changes on disk (a git checkout): the editor raises
			// SceneChangedOnDisk and leaves the instances as they are until the scene is reloaded.
			Test::AutomationFixture setup("PrefabChangedOnDisk");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Cell", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Cell", "path": "Assets/Prefabs/Cell.prefab", "replaceWithInstance": true})"));
			editor.Update(0.0);
			REQUIRE_FALSE(editor.IsSceneChangedOnDisk());
			const uint64_t revision = editor.GetRevision();
			const Json instance = CaptureEntity(editor, "/Cell");

			// Another program rewrites the prefab: a cast-shadows change, so its content and its size differ.
			const std::filesystem::path file = setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Cell.prefab";
			Result<std::string> text = FileSystem::ReadText(file);
			REQUIRE(text.has_value());
			Result<Json> document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			for (Json& entity : (*document)["Entities"])
			{
				if (entity["Components"].contains("MeshRenderer"))
					entity["Components"]["MeshRenderer"]["CastShadows"] = false;
			}
			const std::string changed = document->dump(1, '\t') + "\n";
			REQUIRE(changed != *text);
			REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(changed.data(), changed.size()))).has_value());
			RunHotReloadFrames(editor, 0.5);

			CHECK(editor.IsSceneChangedOnDisk());
			const std::vector<EngineEvent> events = ReadSceneChangedEvents(editor);
			REQUIRE(events.size() == 1);
			CHECK(events.front().Path == "Assets/Prefabs/Cell.prefab");
			CHECK(editor.GetRevision() == revision);
			CHECK(CaptureEntity(editor, "/Cell") == instance);
		}

		TEST_CASE("HotReload: a changed dependency file of an instanced glTF raises SceneChangedOnDisk and is not applied")
		{
			// The glTF's external buffer is a dependency file: its change is its owner's (the prefab the instance names).
			Test::AutomationFixture setup("GltfDependencyChangedOnDisk");
			EditorContext& editor = setup.GetEditor();
			const std::filesystem::path models = setup.GetEditorFixture().GetProjectRoot() / "Assets/Models";
			for (const std::string_view name : { "Textured.gltf", "Textured.bin", "Textures/Checker.png" })
			{
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/" + std::string(name)));
				REQUIRE(bytes.has_value());
				REQUIRE(FileSystem::CreateDirectories((models / std::string(name)).parent_path()).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(models / std::string(name), *bytes).has_value());
			}
			CallOrFail(setup, "project.refreshAssets", Json::object());
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Models/Textured.gltf", "name": "Model"})"));
			editor.Update(0.0);
			const uint64_t revision = editor.GetRevision();
			const Json instance = CaptureEntity(editor, "/Model");

			// Bytes appended past the buffer's byteLength: a different file whose import still succeeds.
			Result<Buffer> buffer = FileSystem::ReadFile(models / "Textured.bin");
			REQUIRE(buffer.has_value());
			buffer->insert(buffer->end(), 16, std::byte{ 0x5a });
			REQUIRE(FileSystem::WriteFileAtomic(models / "Textured.bin", *buffer).has_value());
			RunHotReloadFrames(editor, 0.5);

			CHECK(editor.IsSceneChangedOnDisk());
			const std::vector<EngineEvent> events = ReadSceneChangedEvents(editor);
			REQUIRE(events.size() == 1);
			CHECK(events.front().Path == "Assets/Models/Textured.bin");
			CHECK(events.front().Id == editor.GetAssets().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle()));
			CHECK(editor.GetRevision() == revision);
			CHECK(CaptureEntity(editor, "/Model") == instance);
		}

		TEST_CASE("PrefabMethods: errors name the problem")
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
