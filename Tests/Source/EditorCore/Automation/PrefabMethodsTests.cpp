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

#include <algorithm>
#include <format>
#include <vector>

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
		TEST_CASE("PrefabMethods: script references stay in instance space through create apply revert and scene reload")
		{
			Test::AutomationFixture setup("PrefabScriptReferences");
			CallOrFail(setup, "script.write", Json{ { "path", "Assets/Scripts/Links.luau" }, { "source", R"(
local Links = { Fields = { Target = Field.Entity(), Targets = Field.Array(Field.Entity()), Amount = Field.Number(1) } }
return Script.Define("Links", Links)
)" } });
			CallOrFail(setup, "entity.create", Json{ { "name", "A" } });
			const Json child = CallOrFail(setup, "entity.create", Json{ { "name", "Child" }, { "parent", "/A" } })["entity"]["id"];
			const Json scriptFields{ { "Target", child }, { "Targets", Json::array({ child }) }, { "Amount", 1 } };
			const Json scriptComponent{ { "Script", "Assets/Scripts/Links.luau" }, { "Fields", scriptFields } };
			CallOrFail(setup, "entity.update", Json{ { "entity", "/A" }, { "components", Json{ { "Script", scriptComponent } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/A" }, { "path", "Assets/Prefabs/Links.prefab" }, { "replaceWithInstance", true } });
			CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Links.prefab" }, { "name", "B" } });
			const auto check = [&setup](std::string_view root, int amount)
			{
				const Json entity = CallOrFail(setup, "entity.get", Json{ { "entity", root } })["entity"];
				const Json target = CallOrFail(setup, "entity.get", Json{ { "entity", std::format("{}/Child", root) } })["entity"]["id"];
				const Json& fields = entity["components"]["Script"]["Fields"];
				CHECK(fields["Target"] == target);
				CHECK(fields["Targets"] == Json::array({ target }));
				CHECK(fields["Amount"] == Json(amount));
			};
			check("/A", 1);
			check("/B", 1);
			CallOrFail(setup, "entity.update", Json{ { "entity", "/B" }, { "components", Json{ { "Script", Json{ { "Fields", Json{ { "Amount", 2 } } } } } } } });
			CallOrFail(setup, "prefab.apply", Json{ { "instance", "/B" } });
			check("/A", 2);
			check("/B", 2);
			CallOrFail(setup, "entity.update", Json{ { "entity", "/B" }, { "components", Json{ { "Script", Json{ { "Fields", Json{ { "Amount", 3 } } } } } } } });
			CallOrFail(setup, "prefab.revert", Json{ { "instance", "/B" } });
			check("/B", 2);
			CallOrFail(setup, "scene.save", Json::object());
			CallOrFail(setup, "scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true } });
			check("/A", 2);
			check("/B", 2);
		}

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

		// S1-04: the prefab itself was read without the script field schemas, so every instance of a prefab whose root sets
		// a script field logged a REFLECTION_VARIANT_UNRESOLVED warning (fifty for Breakout's brick grid).
		TEST_CASE("PrefabMethods: instantiating a prefab whose root sets script fields logs no warnings")
		{
			Test::AutomationFixture setup("PrefabScriptFieldsQuiet");
			CallOrFail(setup, "script.write", Json{ { "path", "Assets/Scripts/Brick.luau" }, { "source", R"(
local Brick = { Fields = { Points = Field.Integer(1, { Min = 1 }) } }
return Script.Define("Brick", Brick)
)" } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Brick" }, { "components", Json{ { "Script", Json{ { "Script", "Assets/Scripts/Brick.luau" }, { "Fields", Json{ { "Points", 2 } } } } } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Brick" }, { "path", "Assets/Prefabs/Brick.prefab" } });

			const Json response = setup.Request("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Brick2" } });
			INFO(response.dump());
			REQUIRE(response.contains("result"));
			const Json& diagnostics = response["result"]["_meta"]["diagnostics"];
			CHECK(diagnostics["newWarnings"] == Json(0));
			CHECK(diagnostics["newErrors"] == Json(0));
			const Json brick = CallOrFail(setup, "entity.get", Json{ { "entity", "/Brick2" } })["entity"];
			CHECK(brick["components"]["Script"]["Fields"]["Points"] == Json(2));
		}

		// S1-17: a brick grid from one prefab without a second op per brick. Per-instance component values are ordinary writes
		// to the new root in the same step, so the recorded overrides are those of prefab.instantiate followed by
		// entity.update; one undo removes the instance, and dry runs and batches work as for the other params.
		TEST_CASE("PrefabMethods: instantiate with components records the same overrides as instantiate followed by entity.update")
		{
			Test::AutomationFixture setup("PrefabInstantiateComponents");
			CallOrFail(setup, "script.write", Json{ { "path", "Assets/Scripts/Brick.luau" }, { "source", R"(
local Brick = { Fields = { Points = Field.Integer(1, { Min = 1 }) } }
return Script.Define("Brick", Brick)
)" } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Brick" }, { "components", Json{ { "Script", Json{ { "Script", "Assets/Scripts/Brick.luau" }, { "Fields", Json{ { "Points", 1 } } } } }, { "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Cube" } } } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Brick" }, { "path", "Assets/Prefabs/Brick.prefab" } });
			CallOrFail(setup, "entity.destroy", Json{ { "entities", Json::array({ "/Brick" }) } });

			// The Script handle of a Fields-only value comes from the prefab's root, as entity.update takes it from the entity.
			const Json components{ { "Script", Json{ { "Fields", Json{ { "Points", 5 } } } } }, { "MeshRenderer", Json{ { "CastShadows", false } } },
				{ "Transform", Json{ { "EulerAngles", Json::array({ 0, 90, 0 }) } } } };
			const Json response = setup.Request("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Row1" }, { "transform", Json{ { "Translation", Json::array({ 2, 0, 0 }) } } }, { "components", components } });
			INFO(response.dump());
			REQUIRE(response.contains("result"));
			CHECK(response["result"]["entity"]["name"] == Json("Row1"));
			CHECK(response["result"]["_meta"]["diagnostics"]["newWarnings"] == Json(0));
			CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Row2" }, { "transform", Json{ { "Translation", Json::array({ 2, 0, 0 }) } } } });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Row2" }, { "components", components } });

			const auto overridesOf = [&setup](std::string_view path)
			{
				const Entity root = setup.GetEditor().GetScene().FindEntityByPath(path);
				REQUIRE(root.IsValid());
				return root.GetComponent<PrefabInstanceComponent>().Overrides;
			};
			const std::vector<PrefabOverride> together = overridesOf("/Row1");
			const std::vector<PrefabOverride> separate = overridesOf("/Row2");
			REQUIRE(together.size() == separate.size());
			for (size_t index = 0; index < together.size(); ++index)
			{
				CAPTURE(index);
				CHECK(together[index].PrefabEntityID == separate[index].PrefabEntityID);
				CHECK(together[index].Kind == separate[index].Kind);
				CHECK(together[index].Component == separate[index].Component);
				CHECK(together[index].Field == separate[index].Field);
				CHECK(together[index].Value == separate[index].Value);
			}
			CHECK(std::ranges::any_of(together, [](const PrefabOverride& entry)
			{
				return entry.Component == "Script" && entry.Field == "Fields" && entry.Value.Get() == Json{ { "Points", 5 } };
			}));
			CHECK(std::ranges::any_of(together, [](const PrefabOverride& entry)
			{
				return entry.Component == "MeshRenderer" && entry.Field == "CastShadows";
			}));
			// The same components, but for the link to each instance's own root.
			Json row1 = CallOrFail(setup, "entity.get", Json{ { "entity", "/Row1" } })["entity"]["components"];
			Json row2 = CallOrFail(setup, "entity.get", Json{ { "entity", "/Row2" } })["entity"]["components"];
			row1.erase("PrefabLink");
			row2.erase("PrefabLink");
			CHECK(row1 == row2);
			CHECK(row1["Script"]["Fields"] == Json{ { "Points", 5 } });

			// One undo step removes the instance with its values; redo brings both back.
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value()); // Row2's update
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value()); // Row2
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value()); // Row1
			CHECK_FALSE(setup.GetEditor().GetScene().FindEntityByPath("/Row1").IsValid());
			REQUIRE(setup.Call("edit.redo", Json::object()).has_value());
			REQUIRE(setup.GetEditor().GetScene().FindEntityByPath("/Row1").IsValid());
			CHECK(overridesOf("/Row1").size() == together.size());

			// A dry run creates nothing; a batch instantiates a row whose values reference nothing earlier.
			const Json dry = CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Dry" }, { "components", components }, { "dryRun", true } });
			CHECK(dry["dryRun"] == Json(true));
			CHECK_FALSE(setup.GetEditor().GetScene().FindEntityByPath("/Dry").IsValid());
			Json ops = Json::array();
			for (int points = 2; points <= 3; ++points)
			{
				ops.push_back(Json{ { "method", "prefab.instantiate" }, { "params", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", std::format("Grid{}", points) }, { "components", Json{ { "Script", Json{ { "Fields", Json{ { "Points", points } } } } } } } } } });
			}
			CallOrFail(setup, "edit.batch", Json{ { "label", "Grid" }, { "ops", ops } });
			CHECK(CallOrFail(setup, "entity.get", Json{ { "entity", "/Grid3" } })["entity"]["components"]["Script"]["Fields"]["Points"] == Json(3));

			// Reopening the scene rebuilds every instance from the prefab and its overrides, script fields included, quietly.
			CallOrFail(setup, "scene.save", Json::object());
			const Json reopened = setup.Request("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true } });
			INFO(reopened.dump());
			REQUIRE(reopened.contains("result"));
			CHECK(reopened["result"]["_meta"]["diagnostics"]["newWarnings"] == Json(0));
			CHECK(CallOrFail(setup, "entity.get", Json{ { "entity", "/Grid3" } })["entity"]["components"]["Script"]["Fields"]["Points"] == Json(3));

			// Invalid values are the params' errors, located under /components, and create nothing.
			const Result<Json> unknown = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Bad" }, { "components", Json{ { "Script", Json{ { "Fields", Json{ { "Pointz", 2 } } } } } } } });
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(unknown.error().GetIssues().empty());
			CHECK(unknown.error().GetIssues()[0].JsonPointer == "/components/Script/Fields/Pointz");
			const Result<Json> range = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Bad" }, { "components", Json{ { "Script", Json{ { "Fields", Json{ { "Points", 0 } } } } } } } });
			REQUIRE_FALSE(range.has_value());
			CHECK(range.error().GetIssues().front().JsonPointer == "/components/Script/Fields/Points");
			const Result<Json> missingPrefab = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Missing.prefab" }, { "components", Json{ { "Script", Json{ { "Fields", Json{ { "Points", 2 } } } } } } } });
			REQUIRE_FALSE(missingPrefab.has_value());
			CHECK(missingPrefab.error().GetCode() == ErrorCode::NotFound);
			const Result<Json> hidden = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Bad" }, { "components", Json{ { "PrefabLink", Json::object() } } } });
			REQUIRE_FALSE(hidden.has_value());
			CHECK_FALSE(setup.GetEditor().GetScene().FindEntityByPath("/Bad").IsValid());
		}

		// E-PFB-REFUSE (ADR 0028 decision 16): replacing a member with an instance of a new prefab took it out of its instance
		// while the new root kept its id, so every later update of the instance failed ("already used in the scene outside the
		// instance") and scene.open kept the stale state. The member is refused, as entity.destroy refuses deleting it.
		TEST_CASE("PrefabMethods: prefab.create replaceWithInstance refuses a member of another instance")
		{
			Test::AutomationFixture setup("PrefabReplaceMember");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Lamp"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Filament", "parent": "/Lamp/Bulb"})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			const Json before = CaptureEntity(editor, "/Lamp/Bulb");
			const size_t steps = editor.GetHistory().GetUndoCount();
			const std::filesystem::path bulbPrefab = setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Bulb.prefab";

			for (const bool dryRun : { false, true })
			{
				CAPTURE(dryRun);
				Json params = ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "path": "Assets/Prefabs/Bulb.prefab", "replaceWithInstance": true})");
				if (dryRun)
					params["dryRun"] = true;
				const Result<Json> refused = setup.Call("prefab.create", params);
				REQUIRE_FALSE(refused.has_value());
				INFO(refused.error().ToString());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
				REQUIRE_FALSE(refused.error().GetIssues().empty());
				CHECK(refused.error().GetIssues()[0].JsonPointer == "/entity");
				CHECK(refused.error().GetIssues()[0].Hint.contains("prefab.unpack"));
				CHECK(CaptureEntity(editor, "/Lamp/Bulb") == before);
				CHECK(editor.GetHistory().GetUndoCount() == steps);
				CHECK_FALSE(FileSystem::Exists(bulbPrefab));
			}

			// Saving the member's subtree without replacing it, and replacing the whole instance, work as before; so does the
			// member once its instance is unpacked.
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "path": "Assets/Prefabs/Bulb.prefab"})"));
			CHECK(CaptureEntity(editor, "/Lamp/Bulb") == before);
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp", "path": "Assets/Prefabs/Lamp2.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.unpack", ParsePrefabMethodJson(R"({"instance": "/Lamp"})"));
			const Json replaced = CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "path": "Assets/Prefabs/Bulb2.prefab", "replaceWithInstance": true})"));
			CHECK(replaced["instance"]["path"] == Json("/Lamp/Bulb"));
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
