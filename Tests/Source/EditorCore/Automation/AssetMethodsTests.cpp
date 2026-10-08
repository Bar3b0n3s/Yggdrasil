#include "TestsPCH.h"

#include "EditorCore/Automation/AssetMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/AutomationTestClient.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		Json ParseAssetMethodJson(std::string_view text)
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

		// Writes Tests/Data/Assets/Gltf/<file> to project://Assets/Models/<file> through the editor's write path.
		void WriteGltfFixture(Test::AutomationFixture& setup, std::string_view file)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/" + std::string(file)));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			const VfsPath path = VfsPath::Create("project", "Assets/Models/" + std::string(file)).value_or(VfsPath());
			REQUIRE(setup.GetEditor().CreateProjectDirectory(path.GetParent()).has_value());
			REQUIRE(setup.GetEditor().WriteProjectFile(path, *bytes).has_value());
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("AssetMethods: asset.create, setProperties, move and delete are undoable steps")
		{
			Test::AutomationFixture setup("AssetCreateSetMoveDelete");
			Json created = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "material", "path": "Assets/Materials/Red.material",
				"values": {"BaseColor": [0.9, 0.15, 0.15, 1], "Roughness": 0.45}})"));
			CHECK(created["asset"]["type"] == Json("Material"));
			CHECK(created["asset"]["path"] == Json("Assets/Materials/Red.material"));
			const std::string id = JsonReader(created["asset"]["id"]).ReadString().value_or(std::string());
			REQUIRE(id.size() == 16);
			CHECK(created["undoIndex"] != Json(0));

			Json properties = CallOrFail(setup, "asset.getProperties", Json{ { "asset", id } });
			CHECK(properties["values"]["Roughness"] == Json(0.45f));
			Json set = CallOrFail(setup, "asset.setProperties", ParseAssetMethodJson(R"({"asset": "Assets/Materials/Red.material", "values": {"Metallic": 1}})"));
			CHECK(set["values"]["Metallic"] == Json(1.0f));
			CHECK(set["values"]["Roughness"] == Json(0.45f));

			Json moved = CallOrFail(setup, "asset.move", Json{ { "asset", id }, { "path", "Assets/Red.material" } });
			CHECK(moved["asset"]["id"] == Json(id));
			CHECK(moved["asset"]["path"] == Json("Assets/Red.material"));
			Json deleted = CallOrFail(setup, "asset.delete", Json{ { "asset", id } });
			CHECK(deleted["trashDirectory"] == Json("Library/Trash/" + id));
			CHECK_FALSE(setup.Call("asset.info", Json{ { "asset", id } }).has_value());

			// Undo the delete, the move and the edit: the file is back with its first content and its handle.
			REQUIRE(setup.Call("edit.undo", Json{ { "steps", 3 } }).has_value());
			Json info = CallOrFail(setup, "asset.info", Json{ { "asset", "Assets/Materials/Red.material" } });
			CHECK(info["asset"]["id"] == Json(id));
			CHECK(CallOrFail(setup, "asset.getProperties", Json{ { "asset", id } })["values"]["Metallic"] == Json(0.0f));
		}

		TEST_CASE("AssetMethods: asset.create supports dry runs and rejects bad paths and values")
		{
			Test::AutomationFixture setup("AssetCreateDryRun");
			Json dry = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Assets/Dry.material", "dryRun": true})"));
			CHECK(dry["dryRun"] == Json(true));
			CHECK_FALSE(setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", "Assets/Dry.material").value_or(VfsPath())));

			Json outside = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Library/Red.material"})"));
			CHECK(outside["error"]["code"] == Json(-32602));
			Json wrongExtension = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Assets/Red.mat"})"));
			CHECK(wrongExtension["error"]["code"] == Json(-32602));
			Json invalid = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Assets/Red.material", "values": {"Roughness": 2}})"));
			CHECK(invalid["error"]["data"]["issues"][0]["pointer"] == Json("/values/Roughness"));
			Json soundEffect = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Lock.sfx"})"));
			CHECK(soundEffect["error"]["code"] == Json(-32009));
			// New projects already have Assets/Audio (ProjectManager's folders), so a second one is AlreadyExists.
			Json existingFolder = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Audio"})"));
			CHECK(existingFolder["error"]["data"]["issues"][0]["pointer"] == Json("/path"));
			Json folder = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Levels/Desert"})"));
			CHECK(folder["asset"]["id"] == Json(""));
			CHECK(folder["path"] == Json("Assets/Levels/Desert"));
			const Result<FileInfo> created = setup.GetEditor().GetVfs().GetInfo(VfsPath::Create("project", "Assets/Levels/Desert").value_or(VfsPath()));
			REQUIRE(created.has_value());
			CHECK(created->IsDirectory);
		}

		TEST_CASE("AssetMethods: asset.list and asset.info report sub-assets, dependencies and diagnostics")
		{
			Test::AutomationFixture setup("AssetListInfo");
			for (const std::string_view file : { "Textured.gltf", "Textured.bin", "Textures/Checker.png" })
			{
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/" + std::string(file)));
				REQUIRE(bytes.has_value());
				const VfsPath path = VfsPath::Create("project", "Assets/Models/" + std::string(file)).value_or(VfsPath());
				REQUIRE(setup.GetEditor().CreateProjectDirectory(path.GetParent()).has_value());
				REQUIRE(setup.GetEditor().WriteProjectFile(path, *bytes).has_value());
			}
			CallOrFail(setup, "project.refreshAssets", Json::object());
			Json listed = CallOrFail(setup, "asset.list", ParseAssetMethodJson(R"({"type": "mesh"})"));
			REQUIRE(listed["assets"].size() >= 1);
			CHECK(listed["assets"][0]["path"].dump().find("Assets/Models/Textured.gltf#mesh:") != std::string::npos);
			Json all = CallOrFail(setup, "asset.list", ParseAssetMethodJson(R"({"subAssets": false, "limit": 1})"));
			CHECK(all["assets"].size() == 1);
			CHECK(all["nextCursor"] != Json(""));

			Json info = CallOrFail(setup, "asset.info", Json{ { "asset", "Assets/Models/Textured.gltf" } });
			CHECK(info["asset"]["type"] == Json("Prefab"));
			CHECK(info["importer"] == Json("Gltf"));
			CHECK(info["dependencyFiles"] == ParseAssetMethodJson(R"(["Assets/Models/Textured.bin", "Assets/Models/Textures/Checker.png"])"));
			CHECK_FALSE(info["subAssets"].empty());
			Json unknown = setup.Request("asset.info", Json{ { "asset", "Assets/Models/Missing.gltf" } });
			CHECK(unknown["error"]["code"] == Json(-32001));
		}

		TEST_CASE("AssetMethods: import settings merge, persist and trigger a reimport")
		{
			Test::AutomationFixture setup("AssetImportSettings");
			const VfsPath png = VfsPath::Create("project", "Assets/Normal.png").value_or(VfsPath());
			REQUIRE(setup.GetEditor().WriteProjectFile(png, Test::MakeTestPng(8, 8)).has_value());
			CallOrFail(setup, "project.refreshAssets", Json::object());
			Json settings = CallOrFail(setup, "asset.getImportSettings", Json{ { "asset", "Assets/Normal.png" } });
			CHECK(settings["importer"] == Json("Texture"));
			CHECK(settings["settings"]["Usage"] == Json("Color"));
			Json changed = CallOrFail(setup, "asset.setImportSettings", ParseAssetMethodJson(R"({"asset": "Assets/Normal.png", "settings": {"Usage": "normalmap"}})"));
			CHECK(changed["settings"]["Usage"] == Json("NormalMap"));
			Json invalid = setup.Request("asset.setImportSettings", ParseAssetMethodJson(R"({"asset": "Assets/Normal.png", "settings": {"Usage": "Sometimes"}})"));
			CHECK(invalid["error"]["data"]["issues"][0]["pointer"] == Json("/settings/Usage"));
			Json reimported = CallOrFail(setup, "asset.reimport", Json{ { "asset", "Assets/Normal.png" } });
			CHECK(reimported["asset"]["path"] == Json("Assets/Normal.png"));
		}

		TEST_CASE("AssetMethods: asset.import copies a glTF's dependency closure with dependency metas")
		{
			Test::AutomationFixture setup("AssetImportClosure");
			const std::string source = (Test::GetTestDataPath("Assets/Gltf") / "Textured.gltf").generic_string();
			Json imported = CallOrFail(setup, "asset.import", Json{ { "source", source }, { "destDir", "Assets/Imported" } });
			CHECK(imported["asset"]["type"] == Json("Prefab"));
			CHECK(imported["copiedFiles"] == ParseAssetMethodJson(R"(["Assets/Imported/Textured.bin", "Assets/Imported/Textured.bin.meta",
				"Assets/Imported/Textured.gltf", "Assets/Imported/Textured.gltf.meta", "Assets/Imported/Textures/Checker.png",
				"Assets/Imported/Textures/Checker.png.meta"])"));
			// A rejected URI in the closure refuses the whole import before anything is copied.
			const std::string escaping = (Test::GetTestDataPath("Assets/Gltf") / "ParentEscape.gltf").generic_string();
			Json refused = setup.Request("asset.import", Json{ { "source", escaping }, { "destDir", "Assets/Escaping" } });
			CHECK(refused["error"]["data"]["errorCode"] == Json("ImportFailed"));
			CHECK_FALSE(setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", "Assets/Escaping").value_or(VfsPath())));
		}

		TEST_CASE("AssetMethods: asset.setImportSettings on an instanced glTF updates its instances in the same undo step")
		{
			Test::AutomationFixture setup("AssetSettingsInstances");
			WriteGltfFixture(setup, "Box.glb");
			CallOrFail(setup, "project.refreshAssets", Json::object());
			Json instance = CallOrFail(setup, "prefab.instantiate", Json{ { "prefab", "Assets/Models/Box.glb" } });
			const Json root = instance["entity"]["id"];
			const Json boundsParams = Json{ { "entities", Json::array({ root }) } };
			Json before = CallOrFail(setup, "entity.bounds", boundsParams);
			REQUIRE(before["bounds"][0]["hasBounds"] == Json(true));

			// Scale is prefab content (baked by GltfImporter), so the placed instance follows it at once, in one undo step.
			Json changed = CallOrFail(setup, "asset.setImportSettings", ParseAssetMethodJson(R"({"asset": "Assets/Models/Box.glb", "settings": {"Scale": 0.5}})"));
			CHECK(changed["settings"]["Scale"] == Json(0.5f));
			Json after = CallOrFail(setup, "entity.bounds", boundsParams);
			CHECK(after["bounds"][0]["size"][0] == Json(JsonReader(before["bounds"][0]["size"][0]).ReadFloat().value_or(0.0f) * 0.5f));
			CHECK(setup.GetEditor().IsSceneDirty());

			// One undo restores the settings and the instance.
			REQUIRE(setup.Call("edit.undo", Json{ { "steps", 1 } }).has_value());
			CHECK(CallOrFail(setup, "asset.getImportSettings", Json{ { "asset", "Assets/Models/Box.glb" } })["settings"]["Scale"] == Json(1.0f));
			CHECK(CallOrFail(setup, "entity.bounds", boundsParams)["bounds"][0]["size"] == before["bounds"][0]["size"]);
		}

		TEST_CASE("AssetMethods: asset.move refuses a standalone texture that a glTF finds by path")
		{
			Test::AutomationFixture setup("AssetMovePathDependent");
			// The image gets its own Texture meta first; the glTF that references it arrives later and reuses it by path.
			WriteGltfFixture(setup, "Textures/Shared.png");
			CallOrFail(setup, "project.refreshAssets", Json::object());
			WriteGltfFixture(setup, "StandaloneTexture.gltf");
			CallOrFail(setup, "project.refreshAssets", Json::object());
			CallOrFail(setup, "asset.reimport", Json{ { "asset", "Assets/Models/StandaloneTexture.gltf" } });

			Json refused = setup.Request("asset.move", Json{ { "asset", "Assets/Models/Textures/Shared.png" }, { "path", "Assets/Shared.png" } });
			CHECK(refused["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(refused["error"]["message"].dump().find("Assets/Models/StandaloneTexture.gltf") != std::string::npos);
			CHECK(setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", "Assets/Models/Textures/Shared.png").value_or(VfsPath())));
			CHECK_FALSE(setup.GetEditor().GetHistory().CanUndo());
		}

		TEST_CASE("AssetMethods: references, folders, cursors and destinations are validated")
		{
			Test::AutomationFixture setup("AssetValidation");
			CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Assets/Red.material"})"));
			CHECK(setup.Request("asset.info", Json{ { "asset", "" } })["error"]["code"] == Json(-32602));
			CHECK(setup.Request("asset.info", Json{ { "asset", "Library/Red.material" } })["error"]["code"] == Json(-32602));
			Json misspelt = setup.Request("asset.info", Json{ { "asset", "Assets/Rad.material" } });
			CHECK(misspelt["error"]["code"] == Json(-32001));
			CHECK(misspelt["error"]["data"]["hint"].dump().find("Assets/Red.material") != std::string::npos);

			CHECK(setup.Request("asset.list", Json{ { "dir", "Library" } })["error"]["code"] == Json(-32602));
			CHECK(setup.Request("asset.list", Json{ { "dir", "Assets/Missing" } })["error"]["code"] == Json(-32001));
			CHECK(setup.Request("asset.list", Json{ { "cursor", "page two" } })["error"]["code"] == Json(-32602));
			CHECK(setup.Request("asset.list", Json{ { "limit", 0 } })["error"]["code"] == Json(-32602));
			Json listed = CallOrFail(setup, "asset.list", ParseAssetMethodJson(R"({"type": "Material", "recursive": false})"));
			CHECK(listed["total"] == Json(1));
			CHECK(listed["nextCursor"] == Json(""));

			// Built-ins have no .meta to change, a move keeps the extension, and a material has no import settings to patch with
			// a non-object.
			CHECK(setup.Request("asset.getImportSettings", Json{ { "asset", "engine://Meshes/Cube" } })["error"]["code"] == Json(-32602));
			CHECK(setup.Request("asset.move", Json{ { "asset", "Assets/Red.material" }, { "path", "Assets/Red.png" } })["error"]["code"] == Json(-32602));
			CHECK(setup.Request("asset.setProperties", Json{ { "asset", "Assets/Red.material" }, { "values", 3 } })["error"]["code"] == Json(-32602));
			Json created = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "Scene", "path": "Assets/Red.scene", "values": {}})"));
			CHECK(created["error"]["code"] == Json(-32602));
			Json taken = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Assets/Red.material"})"));
			CHECK(taken["error"]["code"] == Json(-32004));

			// An unchanged patch writes nothing and records no undo step.
			const size_t undoCount = setup.GetEditor().GetHistory().GetUndoCount();
			Json same = CallOrFail(setup, "asset.setProperties", ParseAssetMethodJson(R"({"asset": "Assets/Red.material", "values": {"Roughness": 0.5}})"));
			CHECK(same["undoIndex"] == Json(0));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == undoCount);
		}

		TEST_CASE("AssetMethods: an external change of the open scene is reported in _meta until the scene is reloaded")
		{
			Test::AutomationFixture setup("AssetSceneChangedMeta");
			CallOrFail(setup, "project.refreshAssets", Json::object());
			constexpr std::string_view External = R"({
	"Format": "Scene",
	"Version": 1,
	"Name": "Main",
	"Seed": 11,
	"ComponentVersions": {
		"Transform": 1
	},
	"Entities": [
		{
			"ID": "2b00000000000002",
			"Name": "Outside",
			"Parent": null,
			"Active": true,
			"Tags": [],
			"Components": {
				"Transform": {
					"Translation": [1, 2, 3],
					"Rotation": [0, 0, 0, 1],
					"Scale": [1, 1, 1]
				}
			}
		}
	]
}
)";
			EditorContext& editor = setup.GetEditor();
			REQUIRE(FileSystem::WriteFileAtomic(editor.GetProject().GetRoot() / "Assets/Scenes/Main.scene", AsBytes(External)).has_value());
			Json refreshed = setup.Request("project.refreshAssets", Json::object());
			REQUIRE(refreshed.contains("result"));
			CHECK(refreshed["result"]["_meta"]["sceneChangedOnDisk"] == Json(true));
			CHECK_FALSE(editor.GetScene().FindEntityByPath("/Outside").IsValid());

			Json reloaded = setup.Request("scene.open", ParseAssetMethodJson(R"({"path": "Assets/Scenes/Main.scene", "reload": true})"));
			REQUIRE(reloaded.contains("result"));
			CHECK_FALSE(reloaded["result"]["_meta"].contains("sceneChangedOnDisk"));
			CHECK(editor.GetScene().FindEntityByPath("/Outside").IsValid());
		}

		// M12 (Docs/Decisions/0015-m12-decisions.md): sound effects through asset.create, asset.getProperties and
		// asset.setProperties (Architecture §6.6, §10.3, §13.5). Skipped skeletons: stream C implements them over stream B's
		// .sfx document functions and removes the skips, and turns the SoundEffect refusal of "asset.create supports dry runs
		// and rejects bad paths and values" into a success.

		TEST_CASE("AssetMethods: asset.create makes a SoundEffect from values over the defaults" * doctest::skip(true))
		{
			Test::AutomationFixture setup("AssetCreateSoundEffect");
			const Json created = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Lock.sfx",
				"values": {"Seed": 3, "Layers": [{"Wave": "Square", "Notes": ["A3:0.05"]}]}})"));
			CHECK(created["asset"]["type"] == Json("AudioClip"));
			CHECK(created["path"] == Json("Assets/Audio/Lock.sfx"));
			CHECK(created["undoIndex"] != Json(0));
			const Result<std::string> text = setup.GetEditor().GetVfs().ReadText(VfsPath::Create("project", "Assets/Audio/Lock.sfx").value_or(VfsPath()));
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(text->starts_with("{\n\t\"Format\": \"SoundEffect\",\n\t\"Version\": 1,"));
			// The new clip imports through SoundEffectImporter.
			const Json info = CallOrFail(setup, "asset.info", ParseAssetMethodJson(R"({"asset": "Assets/Audio/Lock.sfx"})"));
			CHECK(info["importer"] == Json("SoundEffect"));
			CHECK(info["diagnostics"].empty());
			// Undo removes the file and its .meta.
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK_FALSE(setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", "Assets/Audio/Lock.sfx").value_or(VfsPath())));
		}

		TEST_CASE("AssetMethods: asset.getProperties and asset.setProperties read and patch a sound effect" * doctest::skip(true))
		{
			Test::AutomationFixture setup("AssetSoundEffectProperties");
			CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Coin.sfx",
				"values": {"Layers": [{"Wave": "Square", "Notes": ["B5:0.05", "E6:0.2"]}]}})"));
			const Json properties = CallOrFail(setup, "asset.getProperties", ParseAssetMethodJson(R"({"asset": "Assets/Audio/Coin.sfx"})"));
			CHECK(properties["values"]["Seed"] == Json(0));
			CHECK(properties["values"]["Volume"] == Json(1.0));
			REQUIRE(properties["values"]["Layers"].size() == 1);
			CHECK(properties["values"]["Layers"][0]["Envelope"]["Sustain"] == Json(1.0));

			const Json patched = CallOrFail(setup, "asset.setProperties", ParseAssetMethodJson(R"({"asset": "Assets/Audio/Coin.sfx", "values": {"Volume": 0.5}})"));
			CHECK(patched["values"]["Volume"] == Json(0.5));
			CHECK(patched["values"]["Layers"].size() == 1);
			CallOrFail(setup, "edit.undo", Json::object());
			const Json undone = CallOrFail(setup, "asset.getProperties", ParseAssetMethodJson(R"({"asset": "Assets/Audio/Coin.sfx"})"));
			CHECK(undone["values"]["Volume"] == Json(1.0));
		}

		TEST_CASE("AssetMethods: an invalid sound effect is InvalidParams located in its values" * doctest::skip(true))
		{
			Test::AutomationFixture setup("AssetInvalidSoundEffect");
			const Json badNote = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Bad.sfx",
				"values": {"Layers": [{"Notes": ["Z9:0.1"]}]}})"));
			CHECK(badNote["error"]["code"] == Json(-32602));
			CHECK(badNote["error"]["data"]["issues"][0]["pointer"] == Json("/values/Layers/0/Notes/0"));
			const Json outOfRange = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Loud.sfx",
				"values": {"Volume": 4, "Layers": [{"Duration": 0.1}]}})"));
			CHECK(outOfRange["error"]["data"]["issues"][0]["pointer"] == Json("/values/Volume"));
			const Json noLayers = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Empty.sfx"})"));
			CHECK(noLayers["error"]["code"] == Json(-32602));
			CHECK_FALSE(setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", "Assets/Audio/Bad.sfx").value_or(VfsPath())));
		}
	}

}
