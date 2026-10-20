#include "TestsPCH.h"

#include "EditorCore/Automation/AssetMethods.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/AutomationTestClient.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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

		// M16 friction S3-07: asset.delete knew only assets, so the folder a trashed recording left behind could not be removed
		// ("NotFound: no asset 'Assets/Tests/Probe'"). An empty folder is now removed as one undoable command, and a folder
		// that holds files is refused, naming them.
		TEST_CASE("AssetMethods: asset.delete removes an empty folder as one undoable command and refuses a folder that holds files")
		{
			Test::AutomationFixture setup("AssetDeleteFolder");
			const VirtualFileSystem& vfs = setup.GetEditor().GetVfs();
			const auto exists = [&vfs](std::string_view path)
			{
				return vfs.Exists(VfsPath::Create("project", std::string(path)).value_or(VfsPath()));
			};
			CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Probe/Inner"})"));
			CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Assets/Full/Red.material"})"));

			Json refused = setup.Request("asset.delete", Json{ { "asset", "Assets/Full" } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			CHECK(refused["error"]["data"]["errorCode"] == Json("Validation"));
			const std::string detail = refused["error"]["data"]["issues"][0]["message"].dump();
			CHECK(detail.find("not empty") != std::string::npos);
			CHECK(detail.find("Assets/Full/Red.material") != std::string::npos);
			CHECK(detail.find("Assets/Full/Red.material.meta") != std::string::npos);
			CHECK(refused["error"]["data"]["issues"][0]["pointer"] == Json("/asset"));
			CHECK(refused["error"]["data"]["issues"][0]["hint"].dump().find("delete its assets first") != std::string::npos);
			CHECK(exists("Assets/Full/Red.material"));

			Json deleted = CallOrFail(setup, "asset.delete", Json{ { "asset", "Assets/Probe" } });
			CHECK(deleted["asset"] == Json{ { "id", "" }, { "path", "Assets/Probe" }, { "type", "None" } });
			CHECK(deleted["removedFolders"] == Json::array({ "Assets/Probe", "Assets/Probe/Inner" }));
			CHECK(deleted["trashDirectory"] == Json(""));
			CHECK(deleted["trashedFiles"] == Json::array());
			CHECK(deleted["undoIndex"] != Json(0));
			CHECK_FALSE(exists("Assets/Probe"));
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK(exists("Assets/Probe/Inner"));
			REQUIRE(setup.Call("edit.redo", Json::object()).has_value());
			CHECK_FALSE(exists("Assets/Probe"));

			// S3's case: the folder an asset leaves behind once the asset went to the trash.
			CallOrFail(setup, "asset.delete", Json{ { "asset", "Assets/Full/Red.material" } });
			CHECK(CallOrFail(setup, "asset.delete", Json{ { "asset", "project://Assets/Full" } })["removedFolders"] == Json::array({ "Assets/Full" }));
			CHECK_FALSE(exists("Assets/Full"));

			Json root = setup.Request("asset.delete", Json{ { "asset", "Assets" } });
			CHECK(root["error"]["data"]["errorCode"] == Json("InvalidArgument"));
			CHECK(exists("Assets"));
			Json missing = setup.Request("asset.delete", Json{ { "asset", "Assets/Nothing" } });
			CHECK(missing["error"]["data"]["errorCode"] == Json("NotFound"));
		}

		// FX-6 review: the folder form judged emptiness from VFS listings, which leave out host names no VfsPath can spell, and
		// the removal was recursive, so such a file was deleted for good with its "empty" folder. Folders are now removed
		// without recursion: the host refuses one that still holds anything, the command restores what it removed, and the
		// file survives.
		TEST_CASE("AssetMethods: asset.delete refuses a folder holding a file no project path can spell and deletes nothing")
		{
			Test::AutomationFixture setup("AssetDeleteHiddenFile");
			const VirtualFileSystem& vfs = setup.GetEditor().GetVfs();
			const auto exists = [&vfs](std::string_view path)
			{
				return vfs.Exists(VfsPath::Create("project", std::string(path)).value_or(VfsPath()));
			};
			CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Probe/Inner"})"));

			// "nul.txt" is a Windows reserved device name, which VfsPath rejects on every host, so no listing shows it. POSIX
			// hosts create it like any other name; Windows needs the extended-length form, which never resolves it to the device.
			const std::filesystem::path folder = setup.GetEditor().GetProject().GetProjectFile().parent_path() / "Assets" / "Probe";
#if defined(ENGINE_PLATFORM_WINDOWS)
			std::error_code error;
			const std::filesystem::path absolute = std::filesystem::absolute(folder / "nul.txt", error).lexically_normal();
			REQUIRE_FALSE(error);
			const std::filesystem::path hidden(L"\\\\?\\" + absolute.native());
#else
			const std::filesystem::path hidden = folder / "nul.txt";
#endif
			REQUIRE(FileSystem::WriteFileAtomic(hidden, AsBytes("notes"), { .KeepBackup = false }).has_value());
			const Result<std::vector<VfsEntry>> listed = vfs.List(VfsPath::Create("project", "Assets/Probe").value_or(VfsPath()), true);
			REQUIRE(listed.has_value());
			REQUIRE(listed->size() == 1);
			CHECK((*listed)[0].Path.GetPath() == "Assets/Probe/Inner");

			Json refused = setup.Request("asset.delete", Json{ { "asset", "Assets/Probe" } });
			REQUIRE_MESSAGE(refused.contains("error"), refused.dump());
			CHECK(refused["error"]["data"]["errorCode"] == Json("InvalidState"));
			const std::string detail = refused.dump();
			CHECK(detail.find("cannot list") != std::string::npos);
			CHECK(detail.find("nothing was removed") != std::string::npos);
			CHECK(FileSystem::ReadText(hidden) == std::string("notes"));
			CHECK(exists("Assets/Probe/Inner")); // removed first, then restored when the folder above it was refused

			// Once the file is gone the folder is empty for the host too.
			REQUIRE(FileSystem::Remove(hidden).has_value());
			CHECK(CallOrFail(setup, "asset.delete", Json{ { "asset", "Assets/Probe" } })["removedFolders"] == Json::array({ "Assets/Probe", "Assets/Probe/Inner" }));
			CHECK_FALSE(exists("Assets/Probe"));
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
			// An existing folder: "asset.create of an existing folder succeeds without a command and a file in the way is still
			// refused" below.
			Json folder = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Levels/Desert"})"));
			CHECK(folder["asset"]["id"] == Json(""));
			CHECK(folder["path"] == Json("Assets/Levels/Desert"));
			CHECK(folder["created"] == Json(true));
			const Result<FileInfo> created = setup.GetEditor().GetVfs().GetInfo(VfsPath::Create("project", "Assets/Levels/Desert").value_or(VfsPath()));
			REQUIRE(created.has_value());
			CHECK(created->IsDirectory);
		}

		// M17 friction F07: new projects already have the template's folders (ProjectManager), so asset.create {type: Folder}
		// of Assets/Materials failed with AlreadyExists and rolled back the whole edit.batch it was the first op of. A folder
		// that is already there now succeeds without a command (created: false); a file at the path is still refused.
		TEST_CASE("AssetMethods: asset.create of an existing folder succeeds without a command and a file in the way is still refused")
		{
			Test::AutomationFixture setup("AssetCreateExistingFolder");
			const VirtualFileSystem& vfs = setup.GetEditor().GetVfs();
			const auto exists = [&vfs](std::string_view path)
			{
				return vfs.Exists(VfsPath::Create("project", std::string(path)).value_or(VfsPath()));
			};
			const size_t undoCount = setup.GetEditor().GetHistory().GetUndoCount();
			for (const std::string_view folder : { "Assets/Materials", "Assets/Tests", "project://Assets/Audio" })
			{
				CAPTURE(folder);
				const Json existing = CallOrFail(setup, "asset.create", Json{ { "type", "Folder" }, { "path", folder } });
				CHECK(existing["created"] == Json(false));
				CHECK(existing["undoIndex"] == Json(0));
				CHECK(existing["asset"] == Json{ { "id", "" }, { "path", "" }, { "type", "None" } });
			}
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == undoCount);

			// A new folder is one undo step; asking for it again changes nothing.
			const Json created = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Levels/Desert"})"));
			CHECK(created["created"] == Json(true));
			CHECK(created["undoIndex"] != Json(0));
			const Json again = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Levels/Desert"})"));
			CHECK(again["created"] == Json(false));
			CHECK(again["undoIndex"] == Json(0));
			CHECK(again["path"] == Json("Assets/Levels/Desert"));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == undoCount + 1);

			// A file at the path is still AlreadyExists, and stays a file.
			const Json material = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Assets/Red.material"})"));
			CHECK(material["created"] == Json(true));
			const Json file = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "Folder", "path": "Assets/Red.material"})"));
			REQUIRE_MESSAGE(file.contains("error"), file.dump());
			CHECK(file["error"]["data"]["errorCode"] == Json("AlreadyExists"));
			CHECK(file["error"]["data"]["issues"][0]["pointer"] == Json("/path"));
			const Result<FileInfo> info = vfs.GetInfo(VfsPath::Create("project", "Assets/Red.material").value_or(VfsPath()));
			REQUIRE(info.has_value());
			CHECK_FALSE(info->IsDirectory);

			// S1's batch: a template folder first, then an asset in it, now runs as one undo step.
			const Json batch = CallOrFail(setup, "edit.batch", ParseAssetMethodJson(R"({"label": "Materials", "ops": [
				{"method": "asset.create", "params": {"type": "Folder", "path": "Assets/Materials"}},
				{"method": "asset.create", "params": {"type": "Material", "path": "Assets/Materials/Blue.material"}}]})"));
			CHECK(batch["results"][0]["created"] == Json(false));
			CHECK(batch["results"][1]["created"] == Json(true));
			CHECK(exists("Assets/Materials/Blue.material"));
			REQUIRE(setup.Call("edit.undo", Json::object()).has_value());
			CHECK_FALSE(exists("Assets/Materials/Blue.material"));
			CHECK(exists("Assets/Materials"));
		}

		// M17 friction F02: asset.list refused every dir outside the project, so a builder could find a built-in asset only
		// by guessing its path (engine://Environments/Studio). engine:// and the folders below it now list the built-in
		// catalogue (Resources/EngineAssets.json) as project assets are listed.
		TEST_CASE("AssetMethods: asset.list lists the built-in assets under engine://")
		{
			Test::AutomationFixture setup("AssetListBuiltins", true, std::nullopt, true);
			const BuiltinAssetCatalog& builtins = setup.GetEditor().GetAssets().GetBuiltins();
			REQUIRE(builtins.FindByPath("engine://Environments/Studio") != nullptr);

			const Json all = CallOrFail(setup, "asset.list", Json{ { "dir", "engine://" }, { "limit", 1000 } });
			CHECK(all["total"] == Json(builtins.GetEntries().size()));
			CHECK(all["nextCursor"] == Json(""));
			REQUIRE(all["assets"].size() == builtins.GetEntries().size());
			std::vector<std::string> paths;
			for (const Json& asset : all["assets"])
			{
				const std::string path = JsonReader(asset).ReadMember<std::string>("path").value_or(std::string());
				CAPTURE(path);
				const BuiltinAssetEntry* entry = builtins.FindByPath(path);
				REQUIRE(entry != nullptr);
				CHECK(asset == Json{ { "id", entry->Handle.ToString() }, { "path", entry->Path }, { "type", std::string(AssetTypeToString(entry->Type)) } });
				paths.push_back(path);
			}
			CHECK(std::ranges::is_sorted(paths));

			const Json environments = CallOrFail(setup, "asset.list", Json{ { "dir", "engine://Environments" } });
			CHECK(environments["assets"] == ParseAssetMethodJson(R"([{"id": "0000000000000202", "path": "engine://Environments/Sky", "type": "Environment"},
				{"id": "0000000000000201", "path": "engine://Environments/Studio", "type": "Environment"}])"));

			// type, recursive, limit and cursor work as for project assets.
			CHECK(CallOrFail(setup, "asset.list", Json{ { "dir", "engine://" }, { "type", "mesh" }, { "recursive", false } })["total"] == Json(0));
			const Json first = CallOrFail(setup, "asset.list", Json{ { "dir", "engine://" }, { "type", "Mesh" }, { "limit", 4 } });
			CHECK(first["total"] == Json(7));
			CHECK(first["assets"].size() == 4);
			CHECK(first["assets"][0]["path"] == Json("engine://Meshes/Capsule"));
			const Json rest = CallOrFail(setup, "asset.list", Json{ { "dir", "engine://Meshes" }, { "limit", 4 }, { "cursor", first["nextCursor"] } });
			CHECK(rest["assets"].size() == 3);
			CHECK(rest["assets"][2]["path"] == Json("engine://Meshes/Sphere"));
			CHECK(rest["nextCursor"] == Json(""));

			// A folder no built-in lies below, a built-in asset, a malformed engine path and every other scheme are refused.
			CHECK(setup.Request("asset.list", Json{ { "dir", "engine://Nothing" } })["error"]["code"] == Json(-32001));
			CHECK(setup.Request("asset.list", Json{ { "dir", "engine://Meshes/Cube" } })["error"]["code"] == Json(-32602));
			CHECK(setup.Request("asset.list", Json{ { "dir", "engine://Meshes/" } })["error"]["code"] == Json(-32602));
			CHECK(setup.Request("asset.list", Json{ { "dir", "user://Replays" } })["error"]["code"] == Json(-32602));
			CHECK(setup.Request("asset.list", Json{ { "dir", "enginecache://" } })["error"]["code"] == Json(-32602));
		}

		// M17 friction F02: an unknown engine path was compared with the project's paths, so asset.info
		// {asset: "engine://Environments/Default"} got only "asset.list lists the project's assets". It now suggests the
		// closest built-in paths, in method params and component values alike.
		TEST_CASE("AssetMethods: an unknown engine path suggests the built-in assets it resembles")
		{
			Test::AutomationFixture setup("AssetBuiltinSuggestions", true, std::nullopt, true);
			const Json unknown = setup.Request("asset.info", Json{ { "asset", "engine://Environments/Default" } });
			REQUIRE_MESSAGE(unknown.contains("error"), unknown.dump());
			CHECK(unknown["error"]["code"] == Json(-32001));
			const std::string hint = JsonReader(unknown["error"]["data"]).ReadMember<std::string>("hint").value_or(std::string());
			CHECK(hint.find("did you mean") != std::string::npos);
			CHECK(hint.find("'engine://Environments/Studio'") != std::string::npos);
			CHECK(hint.find("'engine://Environments/Sky'") != std::string::npos);

			const Json far = setup.Request("asset.info", Json{ { "asset", "engine://Qqqqqqqqqqqqqqqqqqqqqqqq" } });
			CHECK(far["error"]["code"] == Json(-32001));
			CHECK(far["error"]["data"]["hint"] == Json("asset.list {dir: \"engine://\"} lists the built-in assets"));

			const Json component = setup.Request("entity.create", ParseAssetMethodJson(R"({"name": "Box", "components": {"MeshRenderer": {"Mesh": "engine://Meshes/Cub"}}})"));
			REQUIRE_MESSAGE(component.contains("error"), component.dump());
			CHECK(component.dump().find("did you mean 'engine://Meshes/Cube'") != std::string::npos);
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

		// M12 (Docs/Decisions/0015-m12-decisions.md decision 16): sound effects through asset.create, asset.getProperties and
		// asset.setProperties (Architecture §6.6, §10.3, §13.5) over the .sfx document functions and SoundEffectImporter.
		// (SoundEffect is no longer refused by "asset.create supports dry runs and rejects bad paths and values"; its dry run
		// and its refusals are checked here.)

		TEST_CASE("AssetMethods: asset.create makes a SoundEffect from values over the defaults")
		{
			Test::AutomationFixture setup("AssetCreateSoundEffect");
			const Json dry = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Lock.sfx",
				"values": {"Layers": [{"Duration": 0.1}]}, "dryRun": true})"));
			CHECK(dry["dryRun"] == Json(true));
			CHECK(dry["asset"]["type"] == Json("AudioClip"));
			CHECK_FALSE(setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", "Assets/Audio/Lock.sfx").value_or(VfsPath())));
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

		TEST_CASE("AssetMethods: asset.getProperties and asset.setProperties read and patch a sound effect")
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

		TEST_CASE("AssetMethods: invalid sound effect values are ValidationFailed located in them, as a material's are")
		{
			// Both native asset kinds follow one rule: data that breaks the asset's rules is Validation (ValidationFailed,
			// -32003) located under /values; values that are not an object, or that set the file header, are InvalidParams.
			Test::AutomationFixture setup("AssetInvalidSoundEffect");
			const Json badNote = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Bad.sfx",
				"values": {"Layers": [{"Notes": ["Z9:0.1"]}]}})"));
			CHECK(badNote["error"]["code"] == Json(-32003));
			CHECK(badNote["error"]["data"]["errorCode"] == Json("Validation"));
			CHECK(badNote["error"]["data"]["issues"][0]["pointer"] == Json("/values/Layers/0/Notes/0"));
			const Json outOfRange = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Loud.sfx",
				"values": {"Volume": 4, "Layers": [{"Duration": 0.1}]}})"));
			CHECK(outOfRange["error"]["code"] == Json(-32003));
			CHECK(outOfRange["error"]["data"]["issues"][0]["pointer"] == Json("/values/Volume"));
			const Json noLayers = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Empty.sfx"})"));
			CHECK(noLayers["error"]["code"] == Json(-32003));
			const Json format = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Bad.sfx",
				"values": {"Version": 2, "Layers": [{"Duration": 0.1}]}})"));
			CHECK(format["error"]["code"] == Json(-32602));
			CHECK(format["error"]["data"]["issues"][0]["pointer"] == Json("/values/Version"));
			const Json notObject = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Bad.sfx", "values": [1]})"));
			CHECK(notObject["error"]["code"] == Json(-32602));
			// A material is not a sound effect: its properties keep their own rules, with the same codes.
			CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "Material", "path": "Assets/Red.material"})"));
			const Json wrongKind = setup.Request("asset.setProperties", ParseAssetMethodJson(R"({"asset": "Assets/Red.material", "values": {"Layers": []}})"));
			CHECK(wrongKind["error"]["code"] == Json(-32003));
			CHECK(wrongKind["error"]["data"]["issues"][0]["pointer"] == Json("/values/Layers"));
			CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "SoundEffect", "path": "Assets/Audio/Coin.sfx",
				"values": {"Layers": [{"Duration": 0.1}]}})"));
			const Json badPatch = setup.Request("asset.setProperties", ParseAssetMethodJson(R"({"asset": "Assets/Audio/Coin.sfx", "values": {"Volume": -1}})"));
			CHECK(badPatch["error"]["code"] == Json(-32003));
			CHECK(badPatch["error"]["data"]["issues"][0]["pointer"] == Json("/values/Volume"));
			CHECK_FALSE(setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", "Assets/Audio/Bad.sfx").value_or(VfsPath())));
		}

		// M18 (Docs/Decisions/0029-m18-contract.md decision 18): sprite sheets through asset.create, asset.getProperties and
		// asset.setProperties, checked by ValidateSpriteSheet and, when their texture is registered and loads, against its size.
		TEST_CASE("AssetMethods: a sprite sheet is created over the defaults and patched, its frames inside a texture that loads")
		{
			Test::AutomationFixture setup("AssetSpriteSheet");
			// Tests/Data's 16 x 8 texture.
			const Result<Buffer> png = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Textures/Rgba.png"));
			REQUIRE_MESSAGE(png.has_value(), png.error().ToString());
			const VfsPath texturePath = VfsPath::Create("project", "Assets/Textures/Rgba.png").value_or(VfsPath());
			REQUIRE(setup.GetEditor().CreateProjectDirectory(texturePath.GetParent()).has_value());
			REQUIRE(setup.GetEditor().WriteProjectFile(texturePath, *png).has_value());
			CallOrFail(setup, "project.refreshAssets", Json::object());
			const std::string texture = JsonReader(CallOrFail(setup, "asset.info", Json{ { "asset", "Assets/Textures/Rgba.png" } })["asset"]["id"])
											.ReadString()
											.value_or(std::string());
			REQUIRE(texture.size() == 16);

			// Without values: the empty sheet, every field written.
			const Json empty = CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "SpriteSheet", "path": "Assets/Sprites/Empty.spritesheet"})"));
			CHECK(empty["asset"]["type"] == Json("SpriteSheet"));
			const Result<std::string> emptyText = setup.GetEditor().GetVfs().ReadText(VfsPath::Create("project", "Assets/Sprites/Empty.spritesheet").value_or(VfsPath()));
			REQUIRE_MESSAGE(emptyText.has_value(), emptyText.error().ToString());
			CHECK(*emptyText == "{\n\t\"Format\": \"SpriteSheet\",\n\t\"Version\": 1,\n\t\"Texture\": null,\n\t\"Frames\": [],\n\t\"Clips\": []\n}\n");

			// Over the texture: a frame that ends at the texture's corner is inside it; the texture is the sheet's dependency.
			Json values = ParseAssetMethodJson(R"({"Frames": [{"Name": "Corner", "Rect": {"X": 8, "Y": 0, "Width": 8, "Height": 8}}],
				"Clips": [{"Name": "Idle", "Frames": [{"Frame": 0, "Ticks": 4}]}]})");
			values["Texture"] = texture;
			const Json created = CallOrFail(setup, "asset.create", Json{ { "type", "SpriteSheet" }, { "path", "Assets/Sprites/Hero.spritesheet" }, { "values", values } });
			CHECK(created["undoIndex"] != Json(0));
			// A created asset imports at its first use; its import records the texture.
			CallOrFail(setup, "asset.reimport", ParseAssetMethodJson(R"({"asset": "Assets/Sprites/Hero.spritesheet"})"));
			const Json info = CallOrFail(setup, "asset.info", ParseAssetMethodJson(R"({"asset": "Assets/Sprites/Hero.spritesheet"})"));
			CHECK(info["importer"] == Json("SpriteSheet"));
			REQUIRE(info["dependencies"].size() == 1);
			CHECK(info["dependencies"][0]["id"] == Json(texture));
			CHECK(info["diagnostics"].empty());
			const Json dependents = CallOrFail(setup, "asset.info", Json{ { "asset", texture } })["dependents"];
			REQUIRE(dependents.size() == 1);
			CHECK(dependents[0]["path"] == Json("Assets/Sprites/Hero.spritesheet"));
			const Json properties = CallOrFail(setup, "asset.getProperties", ParseAssetMethodJson(R"({"asset": "Assets/Sprites/Hero.spritesheet"})"));
			CHECK(properties["values"]["Texture"] == Json(texture));
			CHECK(properties["values"]["Frames"][0]["Pivot"] == Json::array({ 0.5, 0.5 }));
			CHECK(properties["values"]["Clips"][0]["Loop"] == Json(true));

			// A frame one texel past the texture's right edge is refused at its Rect, by a create and by a patch, which leaves the
			// file as it was.
			Json outside = values;
			outside["Frames"][0]["Rect"]["X"] = 9;
			const Json refused = setup.Request("asset.create", Json{ { "type", "SpriteSheet" }, { "path", "Assets/Sprites/Outside.spritesheet" }, { "values", outside } });
			CHECK(refused["error"]["code"] == Json(-32003));
			CHECK(refused["error"]["data"]["errorCode"] == Json("Validation"));
			CHECK(refused["error"]["data"]["issues"][0]["pointer"] == Json("/values/Frames/0/Rect"));
			CHECK_FALSE(setup.GetEditor().GetVfs().Exists(VfsPath::Create("project", "Assets/Sprites/Outside.spritesheet").value_or(VfsPath())));
			const Json tall = setup.Request("asset.setProperties",
				ParseAssetMethodJson(R"({"asset": "Assets/Sprites/Hero.spritesheet", "values": {"Frames": [{"Rect": {"X": 0, "Y": 4, "Width": 4, "Height": 5}}]}})"));
			CHECK(tall["error"]["code"] == Json(-32003));
			CHECK(tall["error"]["data"]["issues"][0]["pointer"] == Json("/values/Frames/0/Rect"));
			const Json unchanged = CallOrFail(setup, "asset.getProperties", ParseAssetMethodJson(R"({"asset": "Assets/Sprites/Hero.spritesheet"})"));
			CHECK(unchanged["values"]["Frames"][0]["Rect"]["X"] == Json(8));

			// A patch replaces an array whole and is one undo step.
			const Json patched = CallOrFail(setup, "asset.setProperties",
				ParseAssetMethodJson(R"({"asset": "Assets/Sprites/Hero.spritesheet", "values": {"Frames": [{"Rect": {"X": 0, "Y": 0, "Width": 16, "Height": 8}}]}})"));
			CHECK(patched["values"]["Frames"].size() == 1);
			CHECK(patched["values"]["Frames"][0]["Name"] == Json(""));
			CHECK(patched["values"]["Frames"][0]["Rect"]["Width"] == Json(16));
			CHECK(patched["values"]["Clips"][0]["Name"] == Json("Idle"));
			CallOrFail(setup, "edit.undo", Json::object());
			const Json undone = CallOrFail(setup, "asset.getProperties", ParseAssetMethodJson(R"({"asset": "Assets/Sprites/Hero.spritesheet"})"));
			CHECK(undone["values"]["Frames"][0]["Name"] == Json("Corner"));

			// A texture that is not registered is not checked (project.validate reports it): only the sheet's own rules apply.
			CallOrFail(setup, "asset.create", ParseAssetMethodJson(R"({"type": "SpriteSheet", "path": "Assets/Sprites/Loose.spritesheet",
				"values": {"Texture": "7777000077770001", "Frames": [{"Rect": {"Width": 64, "Height": 64}}]}})"));
			const Json invalid = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SpriteSheet", "path": "Assets/Sprites/Bad.spritesheet",
				"values": {"Clips": [{"Name": "Run", "Frames": [{"Frame": 0, "Ticks": 1}]}]}})"));
			CHECK(invalid["error"]["code"] == Json(-32003));
			CHECK(invalid["error"]["data"]["issues"][0]["pointer"] == Json("/values/Clips/0/Frames/0/Frame"));
			const Json header = setup.Request("asset.create", ParseAssetMethodJson(R"({"type": "SpriteSheet", "path": "Assets/Sprites/Bad.spritesheet",
				"values": {"Format": "SpriteSheet"}})"));
			CHECK(header["error"]["code"] == Json(-32602));
		}
	}

}
