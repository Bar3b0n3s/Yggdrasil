#include "TestsPCH.h"

#include "EditorCore/Automation/AssetMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
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
		TEST_CASE("AssetMethods: asset.create, setProperties, move and delete are undoable steps" * doctest::skip(true))
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

		TEST_CASE("AssetMethods: asset.create supports dry runs and rejects bad paths and values" * doctest::skip(true))
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
			Json folder = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Audio"})"));
			CHECK(folder["asset"]["id"] == Json(""));
			CHECK(folder["path"] == Json("Assets/Audio"));
		}

		TEST_CASE("AssetMethods: asset.list and asset.info report sub-assets, dependencies and diagnostics" * doctest::skip(true))
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

		TEST_CASE("AssetMethods: import settings merge, persist and trigger a reimport" * doctest::skip(true))
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

		TEST_CASE("AssetMethods: asset.import copies a glTF's dependency closure with dependency metas" * doctest::skip(true))
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

		TEST_CASE("AssetMethods: asset.setImportSettings on an instanced glTF updates its instances in the same undo step" * doctest::skip(true))
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

		TEST_CASE("AssetMethods: asset.move refuses a standalone texture that a glTF finds by path" * doctest::skip(true))
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
	}

}
