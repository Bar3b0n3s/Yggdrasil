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
#include <filesystem>
#include <format>
#include <optional>
#include <string>
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

	// A Script component value of one slot (Script version 2, Docs/Decisions/0031-m20-contract.md decision 17).
	static Json ScriptSlots(Json slot)
	{
		return Json{ { "Scripts", Json::array({ std::move(slot) }) } };
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
			const Json scriptComponent = ScriptSlots(Json{ { "Script", "Assets/Scripts/Links.luau" }, { "Fields", scriptFields } });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/A" }, { "components", Json{ { "Script", scriptComponent } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/A" }, { "path", "Assets/Prefabs/Links.prefab" }, { "replaceWithInstance", true } });
			CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Links.prefab" }, { "name", "B" } });
			const auto check = [&setup](std::string_view root, int amount)
			{
				const Json entity = CallOrFail(setup, "entity.get", Json{ { "entity", root } })["entity"];
				const Json target = CallOrFail(setup, "entity.get", Json{ { "entity", std::format("{}/Child", root) } })["entity"]["id"];
				const Json& fields = entity["components"]["Script"]["Scripts"][0]["Fields"];
				CHECK(fields["Target"] == target);
				CHECK(fields["Targets"] == Json::array({ target }));
				CHECK(fields["Amount"] == Json(amount));
			};
			// A list write replaces a slot's Fields whole: the instance's own values with a new Amount.
			const auto setAmount = [&setup](std::string_view root, int amount)
			{
				Json fields = CallOrFail(setup, "entity.get", Json{ { "entity", root } })["entity"]["components"]["Script"]["Scripts"][0]["Fields"];
				fields["Amount"] = amount;
				CallOrFail(setup, "entity.update", Json{ { "entity", root }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", fields } }) } } } });
			};
			check("/A", 1);
			check("/B", 1);
			setAmount("/B", 2);
			CallOrFail(setup, "prefab.apply", Json{ { "instance", "/B" } });
			check("/A", 2);
			check("/B", 2);
			setAmount("/B", 3);
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
			CallOrFail(setup, "entity.create", Json{ { "name", "Brick" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Script", "Assets/Scripts/Brick.luau" }, { "Fields", Json{ { "Points", 2 } } } }) } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Brick" }, { "path", "Assets/Prefabs/Brick.prefab" } });

			const Json response = setup.Request("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Brick2" } });
			INFO(response.dump());
			REQUIRE(response.contains("result"));
			const Json& diagnostics = response["result"]["_meta"]["diagnostics"];
			CHECK(diagnostics["newWarnings"] == Json(0));
			CHECK(diagnostics["newErrors"] == Json(0));
			const Json brick = CallOrFail(setup, "entity.get", Json{ { "entity", "/Brick2" } })["entity"];
			CHECK(brick["components"]["Script"]["Scripts"][0]["Fields"]["Points"] == Json(2));
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
			CallOrFail(setup, "entity.create", Json{ { "name", "Brick" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Script", "Assets/Scripts/Brick.luau" }, { "Fields", Json{ { "Points", 1 } } } }) }, { "MeshRenderer", Json{ { "Mesh", "engine://Meshes/Cube" } } } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Brick" }, { "path", "Assets/Prefabs/Brick.prefab" } });
			CallOrFail(setup, "entity.destroy", Json{ { "entities", Json::array({ "/Brick" }) } });

			// The Script handle of a Fields-only value comes from the prefab's root, as entity.update takes it from the entity.
			const Json components{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Points", 5 } } } }) }, { "MeshRenderer", Json{ { "CastShadows", false } } },
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
				CHECK(together[index].Path == separate[index].Path);
				CHECK(together[index].Kind == separate[index].Kind);
				CHECK(together[index].Component == separate[index].Component);
				CHECK(together[index].Field == separate[index].Field);
				CHECK(together[index].Value == separate[index].Value);
			}
			CHECK(std::ranges::any_of(together, [](const PrefabOverride& entry)
			{
				return entry.Component == "Script" && entry.Field == "Scripts/0/Fields" && entry.Value.Get() == Json{ { "Points", 5 } };
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
			CHECK(row1["Script"]["Scripts"][0]["Fields"] == Json{ { "Points", 5 } });

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
				ops.push_back(Json{ { "method", "prefab.instantiate" }, { "params", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", std::format("Grid{}", points) }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Points", points } } } }) } } } } } });
			}
			CallOrFail(setup, "edit.batch", Json{ { "label", "Grid" }, { "ops", ops } });
			CHECK(CallOrFail(setup, "entity.get", Json{ { "entity", "/Grid3" } })["entity"]["components"]["Script"]["Scripts"][0]["Fields"]["Points"] == Json(3));

			// Reopening the scene rebuilds every instance from the prefab and its overrides, script fields included, quietly.
			CallOrFail(setup, "scene.save", Json::object());
			const Json reopened = setup.Request("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true } });
			INFO(reopened.dump());
			REQUIRE(reopened.contains("result"));
			CHECK(reopened["result"]["_meta"]["diagnostics"]["newWarnings"] == Json(0));
			CHECK(CallOrFail(setup, "entity.get", Json{ { "entity", "/Grid3" } })["entity"]["components"]["Script"]["Scripts"][0]["Fields"]["Points"] == Json(3));

			// Invalid values are the params' errors, located under /components, and create nothing.
			const Result<Json> unknown = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Bad" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Pointz", 2 } } } }) } } } });
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(unknown.error().GetIssues().empty());
			CHECK(unknown.error().GetIssues()[0].JsonPointer == "/components/Script/Scripts/0/Fields/Pointz");
			const Result<Json> range = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Bad" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Points", 0 } } } }) } } } });
			REQUIRE_FALSE(range.has_value());
			CHECK(range.error().GetIssues().front().JsonPointer == "/components/Script/Scripts/0/Fields/Points");
			const Result<Json> missingPrefab = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Missing.prefab" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Points", 2 } } } }) } } } });
			REQUIRE_FALSE(missingPrefab.has_value());
			CHECK(missingPrefab.error().GetCode() == ErrorCode::NotFound);
			const Result<Json> hidden = setup.Call("prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Brick.prefab" }, { "name", "Bad" }, { "components", Json{ { "PrefabLink", Json::object() } } } });
			REQUIRE_FALSE(hidden.has_value());
			CHECK_FALSE(setup.GetEditor().GetScene().FindEntityByPath("/Bad").IsValid());
		}

		// Docs/Decisions/0031-m20-contract.md decision 7 (E-PFB-DEL): RemoveEntity replaces the setup's refusal (ADR 0028
		// decision 16), whose test this case rewrites. Replacing a member with an instance of a new prefab destroys the member
		// and records its RemoveEntity on the outer instance root, and the new instance's root keeps the member's id as a user
		// child of the outer instance, in one undo step; a dry run, and saving the member without replacing it, change
		// nothing in the scene; reverting the RemoveEntity would rebuild the member over the new instance's id, so it fails
		// as any update does then; and the outer instance still updates, so a reopened scene rebuilds it around the new
		// instance.
		TEST_CASE("PrefabMethods: prefab.create replaceWithInstance of a member records its deletion and keeps its id")
		{
			Test::AutomationFixture setup("PrefabReplaceMemberRecorded");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Lamp"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Filament", "parent": "/Lamp/Bulb"})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			const UUID bulb = editor.GetScene().FindEntityByPath("/Lamp/Bulb").GetUUID();
			const Json before = CaptureEntity(editor, "/Lamp/Bulb");
			const Json lampBefore = CaptureEntity(editor, "/Lamp");
			const std::filesystem::path bulbPrefab = setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Bulb.prefab";

			// Saving the member's subtree without replacing it leaves the instance as it is.
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "path": "Assets/Prefabs/BulbCopy.prefab"})"));
			CHECK(CaptureEntity(editor, "/Lamp/Bulb") == before);
			CHECK(CaptureEntity(editor, "/Lamp") == lampBefore);
			const size_t steps = editor.GetHistory().GetUndoCount();

			const Json dryRun = CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "path": "Assets/Prefabs/Bulb.prefab", "replaceWithInstance": true, "dryRun": true})"));
			CHECK(dryRun["instance"]["id"] == Json(bulb.ToString()));
			CHECK(CaptureEntity(editor, "/Lamp/Bulb") == before);
			CHECK(CaptureEntity(editor, "/Lamp") == lampBefore);
			CHECK(editor.GetHistory().GetUndoCount() == steps);
			CHECK_FALSE(FileSystem::Exists(bulbPrefab));

			const Json replaced = CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "path": "Assets/Prefabs/Bulb.prefab", "replaceWithInstance": true})"));
			CHECK(replaced["instance"]["path"] == Json("/Lamp/Bulb"));
			CHECK(replaced["instance"]["id"] == Json(bulb.ToString()));
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);
			CHECK(FileSystem::Exists(bulbPrefab));
			const Json lamp = CaptureEntity(editor, "/Lamp");
			REQUIRE(lamp["Components"]["Prefab"]["Overrides"].size() == 1);
			CHECK(lamp["Components"]["Prefab"]["Overrides"][0]["Kind"] == Json("RemoveEntity"));
			CHECK(lamp["Components"]["Prefab"]["Overrides"][0]["Path"] == Json::array({ before["Components"]["PrefabLink"]["PrefabEntityID"] }));
			const Json instance = CaptureEntity(editor, "/Lamp/Bulb");
			CHECK(instance["Components"].contains("Prefab"));
			CHECK(instance["Components"]["PrefabLink"]["InstanceRoot"] == Json(bulb.ToString()));
			CHECK(editor.GetScene().FindEntityByPath("/Lamp/Bulb/Filament").IsValid());

			// The replacement is one undo step: undo brings the member back with the override gone, redo replaces it again.
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(CaptureEntity(editor, "/Lamp/Bulb") == before);
			CHECK(CaptureEntity(editor, "/Lamp") == lampBefore);
			CHECK_FALSE(FileSystem::Exists(bulbPrefab));
			REQUIRE(setup.Call("edit.redo", Json::object()).has_value());
			CHECK(CaptureEntity(editor, "/Lamp/Bulb") == instance);
			CHECK(CaptureEntity(editor, "/Lamp") == lamp);

			// Reverting the RemoveEntity would rebuild the member with the id the new instance's root holds: the outer
			// instance's update refuses it, naming that id, and nothing changes.
			const Result<Json> reverted = setup.Call("prefab.revert", ParsePrefabMethodJson(R"({"instance": "/Lamp"})"));
			REQUIRE_FALSE(reverted.has_value());
			CHECK(reverted.error().GetCode() == ErrorCode::InvalidState);
			CHECK(reverted.error().ToString().contains(bulb.ToString()));
			CHECK(CaptureEntity(editor, "/Lamp") == lamp);
			CHECK(CaptureEntity(editor, "/Lamp/Bulb") == instance);

			// The outer instance still updates: reopening the saved scene rebuilds it, keeping the new instance in place.
			CallOrFail(setup, "scene.save", Json::object());
			const Json reopened = setup.Request("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true } });
			INFO(reopened.dump());
			REQUIRE(reopened.contains("result"));
			CHECK(editor.GetScene().FindEntityByID(bulb).IsValid());
			CHECK(editor.GetScene().FindEntityByPath("/Lamp/Bulb/Filament").IsValid());
			CHECK(CaptureEntity(editor, "/Lamp")["Components"]["Prefab"]["Overrides"].size() == 1);
		}

		// Docs/Decisions/0031-m20-contract.md decisions 8 and 9 (E-PFB-NEST; Roadmap M20 acceptance "inner prefab edits
		// propagate"): a Room prefab created around a Lamp instance keeps it live. An edit of a lamp inside a room, applied at
		// the Lamp level, changes Lamp.prefab, rewrites Room.prefab and reaches every room; the same apply without a level
		// writes to the outermost prefab, Room, and leaves Lamp alone. One undo step each. The level root's own placement
		// (Name, Transform, Parent) is never written into its prefab.
		TEST_CASE("PrefabMethods: an applied edit of a nested prefab reaches the prefab nesting it and its instances")
		{
			Test::AutomationFixture setup("PrefabNestedApply");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "parent": "/Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Room/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room", "path": "Assets/Prefabs/Room.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Room.prefab", "name": "Room2"})"));
			// Room.prefab holds the lamp as a live instance.
			const Result<std::string> roomText = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Room.prefab");
			REQUIRE(roomText.has_value());
			CHECK(roomText->contains("\"PrefabLink\""));

			// At the Lamp level: Lamp.prefab changes, Room.prefab is rewritten, both rooms follow.
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			const size_t steps = editor.GetHistory().GetUndoCount();
			const Json applied = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Room/Lamp", "level": "/Room/Lamp"})"));
			CHECK(applied["prefab"]["path"] == Json("Assets/Prefabs/Lamp.prefab"));
			REQUIRE(applied["updatedPrefabs"].size() == 1);
			CHECK(applied["updatedPrefabs"][0]["path"] == Json("Assets/Prefabs/Room.prefab"));
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);
			CHECK(CaptureEntity(editor, "/Room2/Lamp/Bulb")["Components"]["MeshRenderer"]["CastShadows"] == Json(false));
			const Result<std::string> lampText = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Lamp.prefab");
			REQUIRE(lampText.has_value());
			CHECK(lampText->contains("\"CastShadows\": false"));
			// The room's override is gone: the value now comes from the lamp.
			CHECK(CaptureEntity(editor, "/Room")["Components"]["Prefab"]["Overrides"].empty());

			// Without a level, the outermost prefab receives the edit; Lamp.prefab keeps its value.
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"Visible": false}}})"));
			const Json outermost = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Room"})"));
			CHECK(outermost["prefab"]["path"] == Json("Assets/Prefabs/Room.prefab"));
			CHECK(outermost["updatedPrefabs"].empty());
			CHECK(CaptureEntity(editor, "/Room2/Lamp/Bulb")["Components"]["MeshRenderer"]["Visible"] == Json(false));
			const Result<std::string> lampAfter = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Lamp.prefab");
			REQUIRE(lampAfter.has_value());
			CHECK_FALSE(lampAfter->contains("\"Visible\": false"));

			// Overrides of the level root itself: its Name, Transform and Parent are its placement in the room, so applied at
			// its own level they stay recorded on the outermost root and never reach Lamp.prefab, while its other overrides
			// (its tags) are re-based onto Lamp.prefab's root and written there.
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "name": "Lantern", "tags": ["Lit"], "components": {"Transform": {"Translation": [1, 0, 0]}}})"));
			CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Room/Lantern", "level": "/Room/Lantern"})"));
			const Json otherLamp = CaptureEntity(editor, "/Room2/Lamp");
			CHECK(otherLamp["Tags"] == Json::array({ "Lit" }));
			CHECK(otherLamp["Components"]["Transform"]["Translation"][0] == Json(0.0));
			const Json kept = CaptureEntity(editor, "/Room")["Components"]["Prefab"]["Overrides"];
			REQUIRE(kept.size() == 2);
			CHECK(kept[0]["Kind"] == Json("EntityKey"));
			CHECK(kept[0]["Field"] == Json("Name"));
			CHECK(kept[1]["Kind"] == Json("Field"));
			CHECK(kept[1]["Component"] == Json("Transform"));
			for (const Json& entry : kept)
				CHECK(entry["Path"].size() == 1);
			const Result<std::string> lampRoot = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Lamp.prefab");
			REQUIRE(lampRoot.has_value());
			CHECK(lampRoot->contains("\"Lit\""));
			CHECK_FALSE(lampRoot->contains("Lantern"));

			// A level outside the instance's chain is refused.
			const Result<Json> wrongLevel = setup.Call("prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Room", "level": "/Room2/Lamp"})"));
			REQUIRE_FALSE(wrongLevel.has_value());
			CHECK(wrongLevel.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// Docs/Decisions/0031-m20-contract.md decision 9: prefab.apply's level is one of the instance's levels, the outermost
		// instance root by default; a top-level instance is its own only level. A level that is not one of them, or names no
		// entity, is refused at /level before anything is written, and the hint lists the levels.
		TEST_CASE("PrefabMethods: prefab.apply writes to the outermost level by default and refuses a level outside the instance")
		{
			Test::AutomationFixture setup("PrefabApplyLevel");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Cell", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Plain"})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Cell", "path": "Assets/Prefabs/Cell.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Cell.prefab", "name": "Cell2"})"));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Cell2", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			const std::filesystem::path cellPrefab = setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Cell.prefab";
			const Result<std::string> before = FileSystem::ReadText(cellPrefab);
			REQUIRE(before.has_value());
			const size_t steps = editor.GetHistory().GetUndoCount();

			struct RefusedLevel
			{
				std::string_view Level{};
				ErrorCode Code = ErrorCode::InvalidArgument;
			};
			for (const RefusedLevel refused : { RefusedLevel{ "/Cell", ErrorCode::InvalidArgument }, RefusedLevel{ "/Plain", ErrorCode::InvalidArgument },
					 RefusedLevel{ "/Missing", ErrorCode::NotFound } })
			{
				CAPTURE(refused.Level);
				const Result<Json> applied = setup.Call("prefab.apply", Json{ { "instance", "/Cell2" }, { "level", refused.Level } });
				REQUIRE_FALSE(applied.has_value());
				CHECK(applied.error().GetCode() == refused.Code);
				REQUIRE_FALSE(applied.error().GetIssues().empty());
				CHECK(applied.error().GetIssues()[0].JsonPointer == "/level");
				if (refused.Code == ErrorCode::InvalidArgument)
					CHECK(applied.error().GetIssues()[0].Hint.contains("'/Cell2'"));
			}
			CHECK(editor.GetHistory().GetUndoCount() == steps);
			const Result<std::string> unchanged = FileSystem::ReadText(cellPrefab);
			REQUIRE(unchanged.has_value());
			CHECK(*unchanged == *before);

			// The instance's own root is its only level: the same apply as without a level, which no prefab nests.
			const Json applied = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Cell2", "level": "/Cell2"})"));
			CHECK(applied["prefab"]["path"] == Json("Assets/Prefabs/Cell.prefab"));
			CHECK(applied["updatedInstances"] == Json(2));
			CHECK(applied["updatedPrefabs"] == Json::array());
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);
			CHECK(CaptureEntity(editor, "/Cell")["Components"]["MeshRenderer"]["CastShadows"] == Json(false));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Cell", "components": {"MeshRenderer": {"Visible": false}}})"));
			const Json outermost = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Cell"})"));
			CHECK(outermost["updatedPrefabs"] == Json::array());
			CHECK(CaptureEntity(editor, "/Cell2")["Components"]["MeshRenderer"]["Visible"] == Json(false));
		}

		// Docs/Decisions/0031-m20-contract.md decision 10: a PrefabOverrideKey names its override by the path the instance's
		// Prefab component records, and its kind by any registered PrefabOverrideKind name in any case. Only the overrides a key
		// selects go; a key of another entity's path selects none; a malformed key is refused at its pointer and reverts
		// nothing; the version 1 key prefabEntityId is an unknown member.
		TEST_CASE("PrefabMethods: prefab.revert removes the overrides its keys select by path")
		{
			Test::AutomationFixture setup("PrefabRevertPath");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb", "components": {"MeshRenderer": {"CastShadows": false, "Visible": false}}})"));
			const auto overrides = [&editor]()
			{
				return editor.GetScene().FindEntityByPath("/Lamp").GetComponent<PrefabInstanceComponent>().Overrides;
			};
			REQUIRE(overrides().size() == 2);
			const std::string bulb = editor.GetScene().FindEntityByPath("/Lamp/Bulb").GetComponent<PrefabLinkComponent>().PrefabEntityID.ToString();
			const std::string lamp = editor.GetScene().FindEntityByPath("/Lamp").GetComponent<PrefabLinkComponent>().PrefabEntityID.ToString();
			const size_t steps = editor.GetHistory().GetUndoCount();

			const auto revert = [&setup](const Json& key)
			{
				return setup.Call("prefab.revert", Json{ { "instance", "/Lamp" }, { "overrides", Json::array({ key }) } });
			};
			struct MalformedKey
			{
				Json Key{};
				std::string_view Pointer{};
			};
			const MalformedKey malformed[] = { { Json{ { "kind", "Field" } }, "/overrides/0/path" },
				{ Json{ { "path", Json::array() } }, "/overrides/0/path" },
				{ Json{ { "path", Json::array({ bulb, "not-an-id" }) } }, "/overrides/0/path/1" },
				{ Json{ { "path", Json::array({ bulb }) }, { "kind", "Deleted" } }, "/overrides/0/kind" } };
			for (const MalformedKey& entry : malformed)
			{
				CAPTURE(entry.Key.dump());
				const Result<Json> refused = revert(entry.Key);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				REQUIRE_FALSE(refused.error().GetIssues().empty());
				CHECK(refused.error().GetIssues()[0].JsonPointer == entry.Pointer);
			}
			const Json unknown = setup.Request("prefab.revert", Json{ { "instance", "/Lamp" }, { "overrides", Json::array({ Json{ { "prefabEntityId", bulb } } }) } });
			INFO(unknown.dump());
			CHECK(unknown["error"]["code"] == Json(-32602));
			CHECK(overrides().size() == 2);
			CHECK(editor.GetHistory().GetUndoCount() == steps);

			// Another entity's path selects nothing; the bulb's path, a lower-case kind and the field select one override.
			const Result<Json> none = revert(Json{ { "path", Json::array({ lamp }) }, { "component", "MeshRenderer" } });
			REQUIRE_MESSAGE(none.has_value(), none.error().ToString());
			CHECK((*none)["removedOverrides"] == Json(0));
			CHECK(overrides().size() == 2);
			const Result<Json> one = revert(Json{ { "path", Json::array({ bulb }) }, { "kind", "field" }, { "component", "MeshRenderer" }, { "field", "CastShadows" } });
			REQUIRE_MESSAGE(one.has_value(), one.error().ToString());
			CHECK((*one)["removedOverrides"] == Json(1));
			REQUIRE(overrides().size() == 1);
			CHECK(overrides()[0].Field == "Visible");
			const Json reverted = CaptureEntity(editor, "/Lamp/Bulb")["Components"]["MeshRenderer"];
			CHECK(reverted["CastShadows"] == Json(true));
			CHECK(reverted["Visible"] == Json(false));
		}

		// Docs/Decisions/0031-m20-contract.md decision 8 (E-PFB-NEST): prefab.revert on a nested instance root removes the
		// overrides its outermost instance records within the nested instance (the nested root's path is their prefix), keeps
		// the others, and rebuilds the outermost instance; with keys, only the matching ones among those go.
		TEST_CASE("PrefabMethods: prefab.revert of a nested instance root removes the overrides within it")
		{
			Test::AutomationFixture setup("PrefabNestedRevert");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Room", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "parent": "/Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Room/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room", "path": "Assets/Prefabs/Room.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"CastShadows": false, "Visible": false}}})"));
			const auto overrides = [&editor]()
			{
				return CaptureEntity(editor, "/Room")["Components"]["Prefab"]["Overrides"];
			};
			REQUIRE(overrides().size() == 3);

			// A key within the nested instance: one override goes, the room's own and the other bulb override stay.
			Json path;
			for (const Json& entry : overrides())
			{
				if (entry.contains("Path") && entry["Field"] == Json("Visible"))
					path = entry["Path"];
			}
			REQUIRE(path.size() == 2);
			const Json one = CallOrFail(setup, "prefab.revert", Json{ { "instance", "/Room/Lamp" }, { "overrides", Json::array({ Json{ { "path", path }, { "field", "Visible" } } }) } });
			CHECK(one["removedOverrides"] == Json(1));
			CHECK(one["instance"]["path"] == Json("/Room/Lamp"));
			CHECK(overrides().size() == 2);
			CHECK(CaptureEntity(editor, "/Room/Lamp/Bulb")["Components"]["MeshRenderer"]["Visible"] == Json(true));

			// Without keys every override within the nested instance goes; the room's own stays.
			const Json all = CallOrFail(setup, "prefab.revert", ParsePrefabMethodJson(R"({"instance": "/Room/Lamp"})"));
			CHECK(all["removedOverrides"] == Json(1));
			const Json kept = overrides();
			REQUIRE(kept.size() == 1);
			REQUIRE(kept[0].contains("Path"));
			CHECK(kept[0]["Path"].size() == 1);
			CHECK(CaptureEntity(editor, "/Room/Lamp/Bulb")["Components"]["MeshRenderer"]["CastShadows"] == Json(true));
			CHECK(CaptureEntity(editor, "/Room")["Components"]["MeshRenderer"]["CastShadows"] == Json(false));
		}

		// Docs/Decisions/0031-m20-contract.md decision 8 (E-PFB-NEST): prefab.unpack unpacks one level. A nested instance root
		// is refused with the call that unpacks its outermost instance, and nothing changes; that call unpacks the room, after
		// which the lamp is an outermost instance of its own, keeping its overridden state, and can be unpacked in turn.
		TEST_CASE("PrefabMethods: prefab.unpack refuses a nested instance root and unpacks one level from the outermost")
		{
			Test::AutomationFixture setup("PrefabNestedUnpack");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "parent": "/Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Room/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room", "path": "Assets/Prefabs/Room.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			const Json before = CaptureEntity(editor, "/Room/Lamp");
			const size_t steps = editor.GetHistory().GetUndoCount();

			const Result<Json> refused = setup.Call("prefab.unpack", ParsePrefabMethodJson(R"({"instance": "/Room/Lamp"})"));
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			REQUIRE_FALSE(refused.error().GetIssues().empty());
			CHECK(refused.error().GetIssues()[0].JsonPointer == "/instance");
			CHECK(refused.error().GetIssues()[0].Hint.contains("prefab.unpack {\"instance\":\"/Room\"}"));
			CHECK(CaptureEntity(editor, "/Room/Lamp") == before);
			CHECK(editor.GetHistory().GetUndoCount() == steps);

			CallOrFail(setup, "prefab.unpack", ParsePrefabMethodJson(R"({"instance": "/Room"})"));
			CHECK_FALSE(CaptureEntity(editor, "/Room")["Components"].contains("Prefab"));
			const Json lamp = CaptureEntity(editor, "/Room/Lamp");
			REQUIRE(lamp["Components"].contains("Prefab"));
			REQUIRE(lamp["Components"]["PrefabLink"].contains("OuterPrefabEntityID"));
			CHECK(lamp["Components"]["PrefabLink"]["OuterPrefabEntityID"].is_null());
			const Json& lampOverrides = lamp["Components"]["Prefab"]["Overrides"];
			REQUIRE(lampOverrides.size() == 1);
			CHECK(lampOverrides[0]["Field"] == Json("CastShadows"));
			CHECK(CaptureEntity(editor, "/Room/Lamp/Bulb")["Components"]["MeshRenderer"]["CastShadows"] == Json(false));
			CallOrFail(setup, "prefab.unpack", ParsePrefabMethodJson(R"({"instance": "/Room/Lamp"})"));
			CHECK_FALSE(CaptureEntity(editor, "/Room/Lamp/Bulb")["Components"].contains("PrefabLink"));
		}

		// Docs/Decisions/0031-m20-contract.md decision 8: prefab.create keeps an instance nested in the subtree live, outermost
		// in the new prefab, with its record recomputed against its own prefab. Room.prefab records the lamp's hidden bulb and
		// an instance of Room records nothing for it, so the instance's records alone would leave the new prefab's lamp without
		// that override, and the next rewrite of the new prefab from Lamp.prefab would show the bulb again.
		TEST_CASE("PrefabMethods: prefab.create keeps a nested instance live with the record of the prefab that nested it")
		{
			Test::AutomationFixture setup("PrefabCreateNested");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "parent": "/Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Room/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"Visible": false}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room", "path": "Assets/Prefabs/Room.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Room.prefab", "name": "Room2"})"));
			REQUIRE(CaptureEntity(editor, "/Room2")["Components"]["Prefab"]["Overrides"].empty());
			const Json bulbPrefabID = CaptureEntity(editor, "/Room2/Lamp/Bulb")["Components"]["PrefabLink"]["PrefabEntityID"];

			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room2", "path": "Assets/Prefabs/Hall.prefab"})"));
			const Result<std::string> text = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Hall.prefab");
			REQUIRE(text.has_value());
			const Result<Json> hall = JsonReader::Parse(*text);
			REQUIRE(hall.has_value());
			const Json& entities = (*hall)["Entities"];
			const auto lamp = std::find_if(entities.begin(), entities.end(), [](const Json& entity)
			{
				return entity["Name"] == Json("Lamp");
			});
			REQUIRE(lamp != entities.end());
			REQUIRE((*lamp)["Components"].contains("Prefab"));
			CHECK((*lamp)["Components"]["PrefabLink"]["OuterPrefabEntityID"].is_null());
			const Json& record = (*lamp)["Components"]["Prefab"]["Overrides"];
			REQUIRE(record.size() == 1);
			CHECK(record[0]["Path"] == Json::array({ bulbPrefabID }));
			CHECK(record[0]["Field"] == Json("Visible"));
			CHECK(record[0]["Value"] == Json(false));

			// A Lamp-level apply rewrites both prefabs nesting Lamp.prefab, and the hall's lamp keeps its hidden bulb.
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Hall.prefab", "name": "Hall"})"));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			const Json applied = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Room/Lamp", "level": "/Room/Lamp"})"));
			REQUIRE(applied["updatedPrefabs"].size() == 2);
			CHECK(applied["updatedPrefabs"][0]["path"] == Json("Assets/Prefabs/Hall.prefab"));
			CHECK(applied["updatedPrefabs"][1]["path"] == Json("Assets/Prefabs/Room.prefab"));
			const Json hallBulb = CaptureEntity(editor, "/Hall/Lamp/Bulb")["Components"]["MeshRenderer"];
			CHECK(hallBulb["CastShadows"] == Json(false));
			CHECK(hallBulb["Visible"] == Json(false));
		}

		// Docs/Decisions/0031-m20-contract.md decisions 8 and 9: an instance a user placed under a member of another instance
		// belongs to no prefab, so it is outermost, its own only level: prefab.apply, prefab.revert and prefab.unpack act on it
		// as on any top-level instance, and the instance it sits in is not one of its levels.
		TEST_CASE("PrefabMethods: an instance placed under a member of another instance is its own only level")
		{
			Test::AutomationFixture setup("PrefabUserChildInstance");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Moth", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Moth", "path": "Assets/Prefabs/Moth.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "entity.reparent", ParsePrefabMethodJson(R"({"entity": "/Moth", "parent": "/Lamp/Bulb"})"));
			CHECK(CaptureEntity(editor, "/Lamp/Bulb/Moth")["Components"]["PrefabLink"]["OuterPrefabEntityID"].is_null());

			const Result<Json> wrongLevel = setup.Call("prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Lamp/Bulb/Moth", "level": "/Lamp"})"));
			REQUIRE_FALSE(wrongLevel.has_value());
			CHECK(wrongLevel.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(wrongLevel.error().GetIssues().empty());
			CHECK(wrongLevel.error().GetIssues()[0].JsonPointer == "/level");

			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb/Moth", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			const Json applied = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Lamp/Bulb/Moth", "level": "/Lamp/Bulb/Moth"})"));
			CHECK(applied["prefab"]["path"] == Json("Assets/Prefabs/Moth.prefab"));
			CHECK(applied["updatedPrefabs"].empty());
			const Result<std::string> moth = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Moth.prefab");
			REQUIRE(moth.has_value());
			CHECK(moth->contains("\"CastShadows\": false"));

			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Lamp/Bulb/Moth", "components": {"MeshRenderer": {"Visible": false}}})"));
			const Json reverted = CallOrFail(setup, "prefab.revert", ParsePrefabMethodJson(R"({"instance": "/Lamp/Bulb/Moth"})"));
			CHECK(reverted["removedOverrides"] == Json(1));
			CHECK(CaptureEntity(editor, "/Lamp/Bulb/Moth")["Components"]["MeshRenderer"]["Visible"] == Json(true));

			CallOrFail(setup, "prefab.unpack", ParsePrefabMethodJson(R"({"instance": "/Lamp/Bulb/Moth"})"));
			CHECK_FALSE(CaptureEntity(editor, "/Lamp/Bulb/Moth")["Components"].contains("Prefab"));
			CHECK(CaptureEntity(editor, "/Lamp")["Components"].contains("Prefab"));
		}

		// Docs/Decisions/0031-m20-contract.md decision 8: a nested root's OuterPrefabEntityID names its place in the instance
		// enclosing it, and strict loading refuses one that no ancestor instance root derives the root's id from. Dragging a
		// lamp out of its room (entity.reparent, which the hierarchy panel's drag calls) left that key behind, so play, a
		// strict reopen and prefab.unpack of the lamp failed. The commit clears it: the lamp becomes an outermost instance
		// recording its own overrides, and the room records the lamp's deletion, in one undo step. A lamp moved from one room
		// into another is an outermost instance there, a user child of that room.
		TEST_CASE("PrefabMethods: a nested instance moved out of its enclosing instance stays loadable")
		{
			Test::AutomationFixture setup("PrefabNestedMovedOut");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "parent": "/Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Room/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room", "path": "Assets/Prefabs/Room.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Room.prefab", "name": "Room2"})"));
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			const Json lampLink = CaptureEntity(editor, "/Room/Lamp")["Components"]["PrefabLink"];
			REQUIRE_FALSE(lampLink["OuterPrefabEntityID"].is_null());
			const Json bulbPrefabID = CaptureEntity(editor, "/Room/Lamp/Bulb")["Components"]["PrefabLink"]["PrefabEntityID"];
			const Json roomBefore = CaptureEntity(editor, "/Room");
			const size_t steps = editor.GetHistory().GetUndoCount();

			CallOrFail(setup, "entity.reparent", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "parent": ""})"));
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);
			const Json lamp = CaptureEntity(editor, "/Lamp");
			CHECK(lamp["Components"]["PrefabLink"]["OuterPrefabEntityID"].is_null());
			const Json& lampOverrides = lamp["Components"]["Prefab"]["Overrides"];
			REQUIRE(lampOverrides.size() == 1);
			CHECK(lampOverrides[0]["Path"] == Json::array({ bulbPrefabID }));
			CHECK(lampOverrides[0]["Field"] == Json("CastShadows"));
			const Json room = CaptureEntity(editor, "/Room");
			const Json& roomOverrides = room["Components"]["Prefab"]["Overrides"];
			REQUIRE(roomOverrides.size() == 1);
			CHECK(roomOverrides[0]["Kind"] == Json("RemoveEntity"));
			CHECK(roomOverrides[0]["Path"] == Json::array({ lampLink["OuterPrefabEntityID"] }));

			// One undo step: undo puts the lamp back, nested, with the room's record of its bulb.
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(CaptureEntity(editor, "/Room/Lamp")["Components"]["PrefabLink"] == lampLink);
			CHECK(CaptureEntity(editor, "/Room") == roomBefore);
			REQUIRE(setup.Call("edit.redo", Json::object()).has_value());
			CHECK(CaptureEntity(editor, "/Lamp") == lamp);
			CHECK(CaptureEntity(editor, "/Room") == room);

			// The scene loads strictly: play starts, and the saved file reopens with the lamp's own record.
			CallOrFail(setup, "play.start", Json{ { "lockstep", true } });
			CallOrFail(setup, "play.stop", Json::object());
			CallOrFail(setup, "scene.save", Json::object());
			CallOrFail(setup, "scene.open", Json{ { "path", "Assets/Scenes/Main.scene" }, { "reload", true } });
			CHECK(CaptureEntity(editor, "/Lamp/Bulb")["Components"]["MeshRenderer"]["CastShadows"] == Json(false));
			CHECK_FALSE(editor.GetScene().FindEntityByPath("/Room/Lamp").IsValid());

			// A lamp moved into another room is outermost there; the room it left records its deletion, the other only its own.
			CallOrFail(setup, "entity.reparent", ParsePrefabMethodJson(R"({"entity": "/Room2/Lamp", "parent": "/Room"})"));
			CHECK(CaptureEntity(editor, "/Room/Lamp")["Components"]["PrefabLink"]["OuterPrefabEntityID"].is_null());
			const Json room2Overrides = CaptureEntity(editor, "/Room2")["Components"]["Prefab"]["Overrides"];
			REQUIRE(room2Overrides.size() == 1);
			CHECK(room2Overrides[0]["Kind"] == Json("RemoveEntity"));
			CHECK(CaptureEntity(editor, "/Room")["Components"]["Prefab"]["Overrides"] == roomOverrides);
			CallOrFail(setup, "play.start", Json{ { "lockstep", true } });
			CallOrFail(setup, "play.stop", Json::object());

			// Each lamp is an instance of its own, which unpacks like any outermost instance.
			CallOrFail(setup, "prefab.unpack", ParsePrefabMethodJson(R"({"instance": "/Lamp"})"));
			CHECK_FALSE(CaptureEntity(editor, "/Lamp")["Components"].contains("Prefab"));
			CallOrFail(setup, "prefab.unpack", ParsePrefabMethodJson(R"({"instance": "/Room/Lamp"})"));
			CHECK_FALSE(CaptureEntity(editor, "/Room/Lamp")["Components"].contains("Prefab"));
		}

		// Docs/Decisions/0031-m20-contract.md decisions 8 and 9: a nested root moved under a user child of the instance
		// enclosing it stays nested (that instance's root still derives its id), and its levels are still its enclosing
		// instance's root and itself. prefab.apply, prefab.revert and prefab.unpack took a nested root whose parent is no
		// prefab entity for an outermost one, so they refused it or named the wrong error.
		TEST_CASE("PrefabMethods: a nested instance moved under a user child of its enclosing instance keeps its levels")
		{
			Test::AutomationFixture setup("PrefabNestedUnderUserChild");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "parent": "/Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Room/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room", "path": "Assets/Prefabs/Room.prefab", "replaceWithInstance": true})"));
			const std::string lamp = editor.GetScene().FindEntityByPath("/Room/Lamp").GetUUID().ToString();
			const std::string bulb = editor.GetScene().FindEntityByPath("/Room/Lamp/Bulb").GetUUID().ToString();
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Group", "parent": "/Room"})"));
			CallOrFail(setup, "entity.reparent", Json{ { "entity", lamp }, { "parent", "/Room/Group" } });
			REQUIRE(editor.GetScene().FindEntityByPath("/Room/Group/Lamp").IsValid());
			CHECK_FALSE(CaptureEntity(editor, "/Room/Group/Lamp")["Components"]["PrefabLink"]["OuterPrefabEntityID"].is_null());
			const size_t steps = editor.GetHistory().GetUndoCount();

			// prefab.unpack refuses the nested root with the call that unpacks the room, and nothing changes.
			const Result<Json> refused = setup.Call("prefab.unpack", Json{ { "instance", lamp } });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			REQUIRE_FALSE(refused.error().GetIssues().empty());
			CHECK(refused.error().GetIssues()[0].Hint.contains("prefab.unpack {\"instance\":\"/Room\"}"));
			CHECK(editor.GetHistory().GetUndoCount() == steps);

			// Its levels are the room and itself: the group is refused as a level, with the two named.
			const Result<Json> wrongLevel = setup.Call("prefab.apply", Json{ { "instance", lamp }, { "level", "/Room/Group" } });
			REQUIRE_FALSE(wrongLevel.has_value());
			CHECK(wrongLevel.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE_FALSE(wrongLevel.error().GetIssues().empty());
			CHECK(wrongLevel.error().GetIssues()[0].Hint.contains("'/Room', '/Room/Group/Lamp'"));

			// prefab.apply at its own level writes Lamp.prefab (undone below, which puts the lamp back under the group).
			CallOrFail(setup, "entity.update", Json{ { "entity", bulb }, { "components", Json{ { "MeshRenderer", Json{ { "CastShadows", false } } } } } });
			const Json applied = CallOrFail(setup, "prefab.apply", Json{ { "instance", lamp }, { "level", lamp } });
			CHECK(applied["prefab"]["path"] == Json("Assets/Prefabs/Lamp.prefab"));
			const Result<std::string> lampText = FileSystem::ReadText(setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Lamp.prefab");
			REQUIRE(lampText.has_value());
			CHECK(lampText->contains("\"CastShadows\": false"));
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			REQUIRE(editor.GetScene().FindEntityByPath("/Room/Group/Lamp/Bulb").IsValid());

			// prefab.revert of it removes the overrides the room records within it.
			const Json reverted = CallOrFail(setup, "prefab.revert", Json{ { "instance", lamp } });
			CHECK(reverted["removedOverrides"] == Json(1));
			CHECK(CaptureEntity(editor, "/Room")["Components"]["Prefab"]["Overrides"].empty());
		}

		// Docs/Decisions/0031-m20-contract.md decision 9: updatedPrefabs names the prefabs nesting the applied one that the
		// apply rewrote. Applying a lamp without overrides at its own level leaves Lamp.prefab as it is, and the room's
		// document rebuilt from it is the same, so nothing is written and no prefab is reported; every prefab nesting it was
		// reported before.
		TEST_CASE("PrefabMethods: prefab.apply reports only the nesting prefabs it rewrites")
		{
			Test::AutomationFixture setup("PrefabApplyUnchangedNesting");
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lamp", "parent": "/Room"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Bulb", "parent": "/Room/Lamp", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Sphere"}}})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp", "path": "Assets/Prefabs/Lamp.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Room", "path": "Assets/Prefabs/Room.prefab", "replaceWithInstance": true})"));
			const std::filesystem::path root = setup.GetEditorFixture().GetProjectRoot();
			const Result<std::string> lampBefore = FileSystem::ReadText(root / "Assets/Prefabs/Lamp.prefab");
			const Result<std::string> roomBefore = FileSystem::ReadText(root / "Assets/Prefabs/Room.prefab");
			REQUIRE(lampBefore.has_value());
			REQUIRE(roomBefore.has_value());

			const Json unchanged = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Room/Lamp", "level": "/Room/Lamp"})"));
			CHECK(unchanged["prefab"]["path"] == Json("Assets/Prefabs/Lamp.prefab"));
			CHECK(unchanged["updatedPrefabs"] == Json::array());
			CHECK(FileSystem::ReadText(root / "Assets/Prefabs/Lamp.prefab").value_or(std::string()) == *lampBefore);
			CHECK(FileSystem::ReadText(root / "Assets/Prefabs/Room.prefab").value_or(std::string()) == *roomBefore);

			// An edit that changes the room's document reports it.
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Room/Lamp/Bulb", "components": {"MeshRenderer": {"CastShadows": false}}})"));
			const Json changed = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Room/Lamp", "level": "/Room/Lamp"})"));
			REQUIRE(changed["updatedPrefabs"].size() == 1);
			CHECK(changed["updatedPrefabs"][0]["path"] == Json("Assets/Prefabs/Room.prefab"));
			CHECK(FileSystem::ReadText(root / "Assets/Prefabs/Room.prefab").value_or(std::string()) != *roomBefore);
		}

		// Docs/Decisions/0031-m20-contract.md decision 8: only a nested instance makes a prefab nest another. A spawner prefab
		// whose script field names the crate prefab, and a crate prefab whose own script field names itself, nest nothing:
		// applying a crate rewrites neither, reports no nesting prefab and meets no cycle.
		TEST_CASE("PrefabMethods: a prefab another prefab's script field names is not nested by it")
		{
			Test::AutomationFixture setup("PrefabScriptFieldNotNesting");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "script.write", Json{ { "path", "Assets/Scripts/Spawner.luau" }, { "source", R"(
local Spawner = { Fields = { Spawnable = Field.Asset("Prefab") } }
return Script.Define("Spawner", Spawner)
)" } });
			const Json spawnerScript = ScriptSlots(Json{ { "Script", "Assets/Scripts/Spawner.luau" }, { "Fields", Json{ { "Spawnable", "Assets/Prefabs/Crate.prefab" } } } });
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Crate", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cube"}}})"));
			const Json created = CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Crate", "path": "Assets/Prefabs/Crate.prefab", "replaceWithInstance": true})"));
			const std::optional<UUID> crate = UUID::FromString(JsonReader(created["prefab"]["id"]).ReadString().value_or(std::string()));
			REQUIRE(crate.has_value());
			CallOrFail(setup, "entity.create", Json{ { "name", "Spawner" }, { "components", Json{ { "Script", spawnerScript } } } });
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/Spawner", "path": "Assets/Prefabs/Spawner.prefab"})"));
			const std::filesystem::path spawnerFile = setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Spawner.prefab";
			const Result<std::string> spawnerBefore = FileSystem::ReadText(spawnerFile);
			REQUIRE(spawnerBefore.has_value());
			REQUIRE(spawnerBefore->contains(AssetHandle(*crate).ToString()));

			// The crate's own script names the crate prefab; applied twice, the second time from a prefab naming itself.
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Crate" }, { "components", Json{ { "Script", spawnerScript }, { "MeshRenderer", Json{ { "CastShadows", false } } } } } });
			const Json first = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Crate"})"));
			CHECK(first["updatedPrefabs"] == Json::array());
			CallOrFail(setup, "entity.update", ParsePrefabMethodJson(R"({"entity": "/Crate", "components": {"MeshRenderer": {"Visible": false}}})"));
			const Json second = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/Crate"})"));
			CHECK(second["updatedPrefabs"] == Json::array());
			CHECK(FileSystem::ReadText(spawnerFile).value_or(std::string()) == *spawnerBefore);
			const AssetHandle crateOnly[] = { AssetHandle(*crate) };
			const Result<std::vector<AssetHandle>> nesting = editor.GetNestingPrefabs(crateOnly);
			REQUIRE_MESSAGE(nesting.has_value(), nesting.error().ToString());
			CHECK(nesting->empty());
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
