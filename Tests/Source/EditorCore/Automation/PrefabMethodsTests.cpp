#include "TestsPCH.h"

#include "EditorCore/Automation/PrefabMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Buffer.h"
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
#include <system_error>
#include <utility>
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

		// M35's builder's report, for prefab.instantiate's components: they patch the instance root's components, which come
		// from the prefab, so a rule between a component's fields holds for the merged value, never for the patch over the
		// component's defaults (UiRect's default AnchorMin, 0.5, is above the AnchorMax written here). A value the merged
		// component breaks is still refused, and nothing is instantiated.
		TEST_CASE("PrefabMethods: instantiate checks a partial component value merged into the prefab root's component")
		{
			Test::AutomationFixture setup("PrefabInstantiatePartialWrite");
			CallOrFail(setup, "entity.create",
				ParsePrefabMethodJson(R"({"name": "Panel", "components": {"UiRect": {"AnchorMin": [0.25, 0.25], "AnchorMax": [0.75, 0.75]}}})"));
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Panel" }, { "path", "Assets/Prefabs/Panel.prefab" } });
			CallOrFail(setup, "prefab.instantiate",
				ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Panel.prefab", "name": "Lowered", "components": {"UiRect": {"AnchorMax": [0.375, 0.375]}}})"));
			Json lowered = CallOrFail(setup, "entity.get", Json{ { "entity", "/Lowered" } });
			CHECK(lowered["entity"]["components"]["UiRect"]["AnchorMin"] == ParsePrefabMethodJson("[0.25, 0.25]"));
			CHECK(lowered["entity"]["components"]["UiRect"]["AnchorMax"] == ParsePrefabMethodJson("[0.375, 0.375]"));

			Json crossed = setup.Request("prefab.instantiate",
				ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/Panel.prefab", "name": "Crossed", "components": {"UiRect": {"AnchorMax": [0.125, 0.125]}}})"));
			REQUIRE_MESSAGE(crossed.contains("error"), crossed.dump());
			CHECK(crossed["error"]["data"]["issues"][0]["pointer"] == Json("/components/UiRect/AnchorMin"));
			CHECK_FALSE(setup.GetEditor().GetScene().FindEntityByPath("/Crossed").IsValid());
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

		// M27 friction F04 (Docs/Decisions/0038-m27-rolling-ball.md): the builder added a BottomBar below a HUD instance and
		// applied; prefab.apply reported success, but HUD.prefab kept no BottomBar, so the HUD instances of the other scenes
		// never got it. prefab.apply adds every entity added below the instance to the prefab at its place: the instance holds
		// it once, as a member with a new id (addedEntities), the root's reference to it follows it, every instance in this
		// and the other open scenes gains it, and one undo step restores the prefab and every instance. An added entity that
		// an entity outside the instance references, and an added instance of the prefab itself, are refused and nothing
		// changes.
		TEST_CASE("PrefabMethods: prefab.apply adds the entities added below the instance to the prefab and every instance in one undo step")
		{
			Test::AutomationFixture setup("PrefabApplyAddedEntities");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "script.write", Json{ { "path", "Assets/Scripts/Hud.luau" }, { "source", R"(
local Hud = { Fields = { Bar = Field.Entity() } }
return Script.Define("Hud", Hud)
)" } });
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "HUD"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "TopBar", "parent": "/HUD"})"));
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Score", "parent": "/HUD"})"));
			CallOrFail(setup, "prefab.create", ParsePrefabMethodJson(R"({"entity": "/HUD", "path": "Assets/Prefabs/HUD.prefab", "replaceWithInstance": true})"));
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/HUD.prefab", "name": "HUD2"})"));
			CallOrFail(setup, "scene.new", ParsePrefabMethodJson(R"({"path": "Assets/Scenes/Level2.scene", "keepOpen": true})"));
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/HUD.prefab", "name": "HUD"})"));
			CallOrFail(setup, "scene.activate", ParsePrefabMethodJson(R"({"scene": "Assets/Scenes/Main.scene"})"));
			const Json inLevel2{ { "scene", "Assets/Scenes/Level2.scene" } };
			const auto existsInLevel2 = [&setup, &inLevel2](std::string_view path)
			{
				Json params = inLevel2;
				params["entity"] = path;
				return setup.Call("entity.get", params).has_value();
			};
			const auto childNames = [&editor](std::string_view path)
			{
				std::vector<std::string> names;
				const Entity parent = editor.GetScene().FindEntityByPath(path);
				REQUIRE(parent.IsValid());
				for (const UUID child : parent.GetChildren())
					names.push_back(editor.GetScene().FindEntityByID(child).GetName());
				return names;
			};

			// A BottomBar with a child between TopBar and Score; the HUD root's script names it.
			const Json bottom = CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "BottomBar", "parent": "/HUD", "index": 1})"))["entity"];
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Lives", "parent": "/HUD/BottomBar"})"));
			const Json hudScript = ScriptSlots(Json{ { "Script", "Assets/Scripts/Hud.luau" }, { "Fields", Json{ { "Bar", bottom["id"] } } } });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/HUD" }, { "components", Json{ { "Script", hudScript } } } });
			const std::filesystem::path prefabFile = setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/HUD.prefab";
			const Result<std::string> before = FileSystem::ReadText(prefabFile);
			REQUIRE(before.has_value());
			const size_t steps = editor.GetHistory().GetUndoCount();

			const Json applied = CallOrFail(setup, "prefab.apply", ParsePrefabMethodJson(R"({"instance": "/HUD"})"));
			CHECK(childNames("/HUD2") == std::vector<std::string>{ "TopBar", "BottomBar", "Score" });
			REQUIRE(applied.contains("addedEntities"));
			REQUIRE(applied["addedEntities"].size() == 1);
			CHECK(applied["addedEntities"][0]["path"] == Json("/HUD/BottomBar"));
			CHECK(applied["addedEntities"][0]["id"] != bottom["id"]);
			CHECK(editor.GetHistory().GetUndoCount() == steps + 1);
			const Result<std::string> written = FileSystem::ReadText(prefabFile);
			REQUIRE(written.has_value());
			CHECK(written->contains("\"BottomBar\""));
			CHECK(written->contains("\"Lives\""));
			for (const std::string_view root : { "/HUD", "/HUD2" })
			{
				CAPTURE(root);
				CHECK(childNames(root) == std::vector<std::string>{ "TopBar", "BottomBar", "Score" });
				const Json bar = CaptureEntity(editor, std::format("{}/BottomBar", root));
				CHECK(bar["Components"].contains("PrefabLink"));
				CHECK(CaptureEntity(editor, root)["Components"]["Script"]["Scripts"][0]["Fields"]["Bar"] == bar["ID"]);
				CHECK(editor.GetScene().FindEntityByPath(std::format("{}/BottomBar/Lives", root)).IsValid());
				CHECK(CaptureEntity(editor, root)["Components"]["Prefab"]["Overrides"].empty());
			}
			CHECK(CaptureEntity(editor, "/HUD/BottomBar")["ID"] == applied["addedEntities"][0]["id"]);
			CHECK(existsInLevel2("/HUD/BottomBar/Lives"));

			// One undo step restores the prefab and every instance: the BottomBar is a user child of /HUD again, with its id.
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(FileSystem::ReadText(prefabFile).value_or(std::string()) == *before);
			const Json restored = CaptureEntity(editor, "/HUD/BottomBar");
			CHECK(restored["ID"] == bottom["id"]);
			CHECK_FALSE(restored["Components"].contains("PrefabLink"));
			CHECK(childNames("/HUD2") == std::vector<std::string>{ "TopBar", "Score" });
			CHECK_FALSE(existsInLevel2("/HUD/BottomBar"));
			REQUIRE(setup.Call("edit.redo", Json::object()).has_value());
			CHECK(CaptureEntity(editor, "/HUD/BottomBar")["ID"] == applied["addedEntities"][0]["id"]);
			CHECK(existsInLevel2("/HUD/BottomBar/Lives"));

			// A Banner added below HUD2 that an entity outside it references is refused, and nothing changes.
			CallOrFail(setup, "entity.create", ParsePrefabMethodJson(R"({"name": "Banner", "parent": "/HUD2"})"));
			const Json banner = CaptureEntity(editor, "/HUD2/Banner");
			CallOrFail(setup, "entity.create", Json{ { "name", "Watcher" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Script", "Assets/Scripts/Hud.luau" }, { "Fields", Json{ { "Bar", banner["ID"] } } } }) } } } });
			const Result<std::string> applyOnce = FileSystem::ReadText(prefabFile);
			REQUIRE(applyOnce.has_value());
			const size_t refusedSteps = editor.GetHistory().GetUndoCount();
			const Json referenced = setup.Request("prefab.apply", ParsePrefabMethodJson(R"({"instance": "/HUD2"})"));
			REQUIRE(referenced.contains("error"));
			CHECK(referenced["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(referenced["error"]["data"]["location"]["entity"] == CaptureEntity(editor, "/Watcher")["ID"]);
			CHECK(editor.GetHistory().GetUndoCount() == refusedSteps);
			CHECK(FileSystem::ReadText(prefabFile).value_or(std::string()) == *applyOnce);
			CHECK(CaptureEntity(editor, "/HUD2/Banner") == banner);

			// An instance of HUD.prefab placed below HUD2 would make the prefab nest itself: refused, nothing written.
			CallOrFail(setup, "entity.destroy", ParsePrefabMethodJson(R"({"entities": ["/Watcher"]})"));
			CallOrFail(setup, "prefab.instantiate", ParsePrefabMethodJson(R"({"prefab": "Assets/Prefabs/HUD.prefab", "parent": "/HUD2/Banner", "name": "Inner"})"));
			const size_t cycleSteps = editor.GetHistory().GetUndoCount();
			const Result<Json> cycle = setup.Call("prefab.apply", ParsePrefabMethodJson(R"({"instance": "/HUD2"})"));
			REQUIRE_FALSE(cycle.has_value());
			CHECK(cycle.error().GetCode() == ErrorCode::Validation);
			REQUIRE_FALSE(cycle.error().GetIssues().empty());
			CHECK(cycle.error().GetIssues()[0].JsonPointer == "/instance");
			CHECK(editor.GetHistory().GetUndoCount() == cycleSteps);
			CHECK(FileSystem::ReadText(prefabFile).value_or(std::string()) == *applyOnce);
		}

		// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 11; World Tree's brief 13, its third item): a patch of one
		// script field at instantiation merges into the slot as the instantiation mapped it. Before M35 the slot's other entity
		// field kept the prefab's own ID of its target, which named the source entity outside the instance, and the commit only
		// warned that the overrides could not record it.
		TEST_CASE("PrefabMethods: a script-field patch at instantiation keeps the slot's other references on the instance's own members")
		{
			Test::AutomationFixture setup("PrefabScriptPatch");
			CallOrFail(setup, "script.write", Json{ { "path", "Assets/Scripts/GameFlow.luau" }, { "source", R"(
local GameFlow = { Fields = { Menus = Field.Entity(), Warden = Field.Entity() } }
return Script.Define("GameFlow", GameFlow)
)" } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Game" } });
			const Json menus = CallOrFail(setup, "entity.create", Json{ { "name", "Menus" }, { "parent", "/Game" } })["entity"]["id"];
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Game" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Script", "Assets/Scripts/GameFlow.luau" }, { "Fields", Json{ { "Menus", menus } } } }) } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Game" }, { "path", "Assets/Prefabs/GameUi.prefab" } });
			const Json warden = CallOrFail(setup, "entity.create", Json{ { "name", "Warden" } })["entity"]["id"];
			const Json response = setup.Request("prefab.instantiate",
				Json{ { "prefab", "Assets/Prefabs/GameUi.prefab" }, { "name", "Hub" },
					{ "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Warden", "/Warden" } } } }) } } } });
			REQUIRE_MESSAGE(response.contains("result"), response.dump());
			const Json& diagnostics = response["result"]["_meta"]["diagnostics"];
			CHECK(diagnostics["newWarnings"] == Json(0));
			CHECK(diagnostics["newErrors"] == Json(0));
			const Json hubMenus = CallOrFail(setup, "entity.get", Json{ { "entity", "/Hub/Menus" } })["entity"]["id"];
			const Json fields = CallOrFail(setup, "entity.get", Json{ { "entity", "/Hub" } })["entity"]["components"]["Script"]["Scripts"][0]["Fields"];
			CHECK(fields["Menus"] == hubMenus);
			CHECK(fields["Warden"] == warden);
			CHECK(fields["Menus"] != menus);
		}

		// M35 (decisions 9 and 10; World Tree's brief 13, its second item): a rebuild writes an instance's overrides of one
		// component together, so the anchors overridden together survive a change of the prefab, and every override a rebuild
		// drops is in the result of the method that rebuilt: prefab.apply's rebuildDiagnostics and scene.open's diagnostics.
		TEST_CASE("PrefabMethods: a rebuild keeps overrides valid only together and reports each it drops in prefab.apply's and scene.open's results")
		{
			Test::AutomationFixture setup("PrefabOverridesTogether");
			const Json centred = Json::array({ 0.5, 0.5 });
			CallOrFail(setup, "entity.create", Json{ { "name", "Options" } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Panel" }, { "parent", "/Options" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", centred }, { "AnchorMax", centred } } } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Options" }, { "path", "Assets/Prefabs/Options.prefab" }, { "replaceWithInstance", true } });
			CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Options.prefab" }, { "name", "Second" } });
			CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Options.prefab" }, { "name", "Third" } });
			const Json corner = Json::array({ 0, 0 });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Options/Panel" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", corner }, { "AnchorMax", corner } } } } } });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Second/Panel" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", Json::array({ 0.375, 0.375 }) } } } } } });
			CallOrFail(setup, "scene.save", Json{ { "path", "Assets/Scenes/Title.scene" } });

			// The prefab's anchors move below Second's AnchorMin: its override is dropped, and reported; Options keeps its pair.
			const Json low = Json::array({ 0.25, 0.25 });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Third/Panel" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", low }, { "AnchorMax", low } } } } } });
			const Json applied = CallOrFail(setup, "prefab.apply", Json{ { "instance", "/Third" } });
			const Json second = CallOrFail(setup, "entity.get", Json{ { "entity", "/Second" } })["entity"]["id"];
			REQUIRE(applied["rebuildDiagnostics"].size() == 1);
			CHECK(applied["rebuildDiagnostics"][0]["code"] == Json("PREFAB_STALE_OVERRIDE"));
			CHECK(applied["rebuildDiagnostics"][0]["entity"] == second);
			CHECK(applied["rebuildDiagnostics"][0]["scene"] == Json("Assets/Scenes/Title.scene"));
			const auto anchors = [&setup](std::string_view path)
			{
				const Json rect = CallOrFail(setup, "entity.get", Json{ { "entity", path } })["entity"]["components"]["UiRect"];
				return std::make_pair(rect["AnchorMin"], rect["AnchorMax"]);
			};
			CHECK(anchors("/Options/Panel") == std::make_pair(corner, corner));
			CHECK(anchors("/Second/Panel") == std::make_pair(low, low));

			// The saved scene, reopened after the prefab changed, is rebuilt the same way, and scene.open reports the drop.
			const Json opened = CallOrFail(setup, "scene.open", Json{ { "path", "Assets/Scenes/Title.scene" }, { "reload", true }, { "discardChanges", true } });
			const auto stale = std::ranges::count_if(opened["diagnostics"], [&second](const Json& diagnostic)
			{
				return diagnostic["code"] == Json("PREFAB_STALE_OVERRIDE") && diagnostic["entity"] == second;
			});
			CHECK(stale == 1);
			CHECK(anchors("/Options/Panel") == std::make_pair(corner, corner));
			CHECK(anchors("/Second/Panel") == std::make_pair(low, low));
		}

		// M35 (decision 10; the review of W35.2, its finding 6): prefab.revert, asset.reimport and asset.import {replace} each
		// rebuild the instance, and each reports what its rebuild drops. The anchors are written so that every step leaves one
		// override that the next version of the prefab, or the selective revert, makes invalid.
		TEST_CASE("PrefabMethods: prefab.revert, asset.reimport and asset.import with replace report the overrides their rebuilds drop")
		{
			Test::AutomationFixture setup("PrefabRebuildReports");
			EditorContext& editor = setup.GetEditor();
			const Json centred = Json::array({ 0.5, 0.5 });
			CallOrFail(setup, "entity.create", Json{ { "name", "Options" } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Panel" }, { "parent", "/Options" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", centred }, { "AnchorMax", centred } } } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Options" }, { "path", "Assets/Prefabs/Options.prefab" }, { "replaceWithInstance", true } });
			const Json options = CaptureEntity(editor, "/Options")["ID"];
			const auto anchors = [&setup]()
			{
				const Json rect = CallOrFail(setup, "entity.get", Json{ { "entity", "/Options/Panel" } })["entity"]["components"]["UiRect"];
				return std::make_pair(rect["AnchorMin"], rect["AnchorMax"]);
			};
			const auto checkDropped = [&options](const Json& result)
			{
				REQUIRE(result["rebuildDiagnostics"].size() == 1);
				const Json& dropped = result["rebuildDiagnostics"][0];
				CHECK(dropped["code"] == Json("PREFAB_STALE_OVERRIDE"));
				CHECK(dropped["entity"] == options);
				CHECK(dropped["scene"] == Json("Assets/Scenes/Main.scene"));
				CHECK(JsonReader(dropped["pointer"]).ReadString().value_or(std::string()).starts_with("/Components/Prefab/Overrides/"));
			};

			// A selective revert of AnchorMin leaves AnchorMax [0, 0] alone, below the prefab's AnchorMin: dropped.
			const Json corner = Json::array({ 0, 0 });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Options/Panel" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", corner }, { "AnchorMax", corner } } } } } });
			const std::string panel = editor.GetScene().FindEntityByPath("/Options/Panel").GetComponent<PrefabLinkComponent>().PrefabEntityID.ToString();
			const Json reverted = CallOrFail(setup, "prefab.revert",
				Json{ { "instance", "/Options" }, { "overrides", Json::array({ Json{ { "path", Json::array({ panel }) }, { "component", "UiRect" }, { "field", "AnchorMin" } } }) } });
			checkDropped(reverted);
			CHECK(anchors() == std::make_pair(centred, centred));

			// Another program moves the prefab's anchors to [0.25, 0.25], below the instance's AnchorMin; asset.reimport drops it.
			const Json high = Json::array({ 0.375, 0.375 });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Options/Panel" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", high } } } } } });
			const std::filesystem::path prefabFile = setup.GetEditorFixture().GetProjectRoot() / "Assets/Prefabs/Options.prefab";
			const auto withAnchors = [&prefabFile](const Json& anchor)
			{
				const Result<std::string> text = FileSystem::ReadText(prefabFile);
				REQUIRE(text.has_value());
				Json document = ParsePrefabMethodJson(*text);
				for (Json& entity : document["Entities"])
				{
					if (entity["Name"] == Json("Panel"))
					{
						entity["Components"]["UiRect"]["AnchorMin"] = anchor;
						entity["Components"]["UiRect"]["AnchorMax"] = anchor;
					}
				}
				return document.dump(2);
			};
			const Json low = Json::array({ 0.25, 0.25 });
			const std::string lowered = withAnchors(low);
			REQUIRE(FileSystem::WriteFileAtomic(prefabFile, AsBytes(lowered), {}).has_value());
			checkDropped(CallOrFail(setup, "asset.reimport", Json{ { "asset", "Assets/Prefabs/Options.prefab" } }));
			CHECK(anchors() == std::make_pair(low, low));

			// A new version imported over the prefab moves its anchors back to [0.5, 0.5], above the instance's AnchorMax (written
			// with the AnchorMin it has, the prefab's, which records no override).
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Options/Panel" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", low }, { "AnchorMax", high } } } } } });
			const std::filesystem::path incoming = setup.GetEditorFixture().GetDirectory().GetPath() / "Incoming";
			std::error_code created;
			std::filesystem::create_directories(incoming, created);
			REQUIRE_FALSE(created);
			const std::string raised = withAnchors(centred);
			REQUIRE(FileSystem::WriteFileAtomic(incoming / "Options.prefab", AsBytes(raised), {}).has_value());
			const Json replaced = CallOrFail(setup, "asset.import",
				Json{ { "source", (incoming / "Options.prefab").generic_string() }, { "destDir", "Assets/Prefabs" }, { "replace", true } });
			checkDropped(replaced);
			CHECK(anchors() == std::make_pair(centred, centred));
		}

		// M35 (decision 10; the lead's addition to the review of W35.2): undoing or redoing a prefab write makes the other open
		// scenes' instances follow it (EditorContext::PropagatePrefabUpdate), and edit.undo and edit.redo report what those
		// rebuilds drop. Level's instance gets an override the version the step restores makes invalid, before each.
		TEST_CASE("PrefabMethods: edit.undo and edit.redo of a prefab write report the overrides the other open scenes' rebuilds drop")
		{
			Test::AutomationFixture setup("PrefabUndoReports");
			const Json centred = Json::array({ 0.5, 0.5 });
			CallOrFail(setup, "entity.create", Json{ { "name", "Options" } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Panel" }, { "parent", "/Options" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", centred }, { "AnchorMax", centred } } } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Options" }, { "path", "Assets/Prefabs/Options.prefab" }, { "replaceWithInstance", true } });
			CallOrFail(setup, "scene.new", Json{ { "path", "Assets/Scenes/Level.scene" }, { "keepOpen", true } });
			const Json hud = CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Prefabs/Options.prefab" }, { "name", "Hud" } })["entity"]["id"];
			CallOrFail(setup, "scene.activate", Json{ { "scene", "Assets/Scenes/Main.scene" } });
			const auto updateHud = [&setup](const Json& rect)
			{
				CallOrFail(setup, "entity.update", Json{ { "scene", "Assets/Scenes/Level.scene" }, { "entity", "/Hud/Panel" }, { "components", Json{ { "UiRect", rect } } } });
			};
			const auto checkDropped = [&hud](const Json& result)
			{
				REQUIRE(result["rebuildDiagnostics"].size() == 1);
				const Json& dropped = result["rebuildDiagnostics"][0];
				CHECK(dropped["code"] == Json("PREFAB_STALE_OVERRIDE"));
				CHECK(dropped["entity"] == hud);
				CHECK(dropped["scene"] == Json("Assets/Scenes/Level.scene"));
			};

			// The prefab's anchors become [0, 0] through Main's instance; Level's Hud follows, dropping nothing.
			const Json corner = Json::array({ 0, 0 });
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Options/Panel" }, { "components", Json{ { "UiRect", Json{ { "AnchorMin", corner }, { "AnchorMax", corner } } } } } });
			const Json applied = CallOrFail(setup, "prefab.apply", Json{ { "instance", "/Options" } });
			CHECK(applied["rebuildDiagnostics"].empty());

			// The undo restores [0.5, 0.5], above Hud's AnchorMax [0.25, 0.25] (written with the AnchorMin it has, the prefab's);
			// the redo [0, 0], below its AnchorMin [0.375, 0.375].
			updateHud(Json{ { "AnchorMin", corner }, { "AnchorMax", Json::array({ 0.25, 0.25 }) } });
			const Json undone = CallOrFail(setup, "edit.undo", Json::object());
			CHECK(undone["undone"] == Json(1));
			checkDropped(undone);
			updateHud(Json{ { "AnchorMin", Json::array({ 0.375, 0.375 }) } });
			const Json redone = CallOrFail(setup, "edit.redo", Json::object());
			CHECK(redone["redone"] == Json(1));
			checkDropped(redone);
			// An undo that rebuilds nothing reports nothing.
			CHECK(CallOrFail(setup, "edit.undo", Json{ { "scene", "Assets/Scenes/Level.scene" } })["rebuildDiagnostics"].empty());
		}

		// M35 (decision 11; the review of W35.2, its finding 7): a patch whose reference the instance's overrides cannot record
		// (the prefab-local ID of Menus, which also names the source scene's Menus, outside the instance) refuses the call, at
		// /components, and creates nothing.
		TEST_CASE("PrefabMethods: prefab.instantiate refuses a patch whose reference the instance's overrides cannot record, creating nothing")
		{
			Test::AutomationFixture setup("PrefabUnrecordablePatch");
			EditorContext& editor = setup.GetEditor();
			CallOrFail(setup, "script.write", Json{ { "path", "Assets/Scripts/GameFlow.luau" }, { "source", R"(
local GameFlow = { Fields = { Menus = Field.Entity(), Warden = Field.Entity() } }
return Script.Define("GameFlow", GameFlow)
)" } });
			CallOrFail(setup, "entity.create", Json{ { "name", "Game" } });
			const Json menus = CallOrFail(setup, "entity.create", Json{ { "name", "Menus" }, { "parent", "/Game" } })["entity"]["id"];
			CallOrFail(setup, "entity.update", Json{ { "entity", "/Game" }, { "components", Json{ { "Script", ScriptSlots(Json{ { "Script", "Assets/Scripts/GameFlow.luau" }, { "Fields", Json{ { "Menus", menus } } } }) } } } });
			CallOrFail(setup, "prefab.create", Json{ { "entity", "/Game" }, { "path", "Assets/Prefabs/GameUi.prefab" } });
			const size_t steps = editor.GetHistory().GetUndoCount();
			const size_t entities = editor.GetScene().GetEntityCount();

			const Json refused = setup.Request("prefab.instantiate",
				Json{ { "prefab", "Assets/Prefabs/GameUi.prefab" }, { "name", "Hub" },
					{ "components", Json{ { "Script", ScriptSlots(Json{ { "Fields", Json{ { "Warden", menus } } } }) } } } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			CHECK(refused["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(refused["error"]["data"]["location"]["pointer"] == Json("/components"));
			CHECK(editor.GetHistory().GetUndoCount() == steps);
			CHECK(editor.GetScene().GetEntityCount() == entities);
			CHECK_FALSE(editor.GetScene().FindEntityByPath("/Hub").IsValid());
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
