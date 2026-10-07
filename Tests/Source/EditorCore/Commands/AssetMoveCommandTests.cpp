#include "TestsPCH.h"

#include "EditorCore/Commands/AssetMoveCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <algorithm>

namespace Engine {

	namespace {

		VfsPath MakeProjectPath(std::string_view relative)
		{
			Result<VfsPath> path = VfsPath::Create("project", relative);
			REQUIRE(path.has_value());
			return *path;
		}

		// Textured.gltf of the glTF fixtures with its external buffer and image, written through the editor and refreshed, so
		// the editor registers it with dependency metas for its two files.
		void WriteTexturedGltf(EditorContext& editor)
		{
			for (const std::string_view file : { "Textured.gltf", "Textured.bin", "Textures/Checker.png" })
			{
				Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Gltf/" + std::string(file)));
				REQUIRE(bytes.has_value());
				const VfsPath path = MakeProjectPath("Assets/Models/" + std::string(file));
				REQUIRE(editor.CreateProjectDirectory(path.GetParent()).has_value());
				REQUIRE(editor.WriteProjectFile(path, *bytes).has_value());
			}
			REQUIRE(editor.GetAssets().Refresh().has_value());
		}

		// Every file and folder (with a trailing '/') under project://Assets and project://Library/Trash, sorted: the state
		// §12.3's property compares (Execute then Undo equals the original, Execute then Undo then Redo equals Execute).
		std::vector<std::string> ListAssetTree(EditorContext& editor)
		{
			std::vector<std::string> paths;
			for (const std::string_view root : { "Assets", "Library/Trash" })
			{
				const VfsPath directory = MakeProjectPath(root);
				if (!editor.GetVfs().Exists(directory))
					continue;
				Result<std::vector<VfsEntry>> entries = editor.GetVfs().List(directory, true);
				REQUIRE_MESSAGE(entries.has_value(), entries.error().ToString());
				for (const VfsEntry& entry : *entries)
					paths.push_back(std::string(entry.Path.GetPath()) + (entry.Info.IsDirectory ? "/" : ""));
			}
			std::ranges::sort(paths);
			return paths;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("AssetMoveCommand: moves a glTF with its dependency files and keeps every handle")
		{
			Test::EditorTestFixture fixture("AssetMoveCommand");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			WriteTexturedGltf(editor);
			const AssetHandle handle = editor.GetAssets().Resolve("Assets/Models/Textured.gltf").value_or(AssetHandle());
			REQUIRE(handle.IsValid());

			const std::vector<std::string> before = ListAssetTree(editor);
			CHECK(std::ranges::find(before, "Assets/Levels/") == before.end());
			Result<Scope<AssetMoveCommand>> command = AssetMoveCommand::Create(editor, handle, MakeProjectPath("Assets/Levels/Track.gltf"));
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			CHECK((*command)->GetMoves().size() == 6);
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			const std::vector<std::string> executed = ListAssetTree(editor);
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Assets/Levels/Track.gltf")));
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Assets/Levels/Textured.bin.meta")));
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Assets/Levels/Textures/Checker.png")));
			CHECK(editor.GetAssets().Resolve("Assets/Levels/Track.gltf") == handle);
			CHECK_FALSE(editor.GetAssets().Resolve("Assets/Models/Textured.gltf").has_value());
			// Provenance follows the files.
			CHECK(editor.GetProvenance()->Find("Assets/Levels/Track.gltf") != nullptr);
			CHECK(editor.GetProvenance()->Find("Assets/Models/Textured.gltf") == nullptr);

			// §12.3: Undo restores the exact prior state, the folders the move created (Assets/Levels, Assets/Levels/Textures)
			// included, and Redo gives exactly the state after Execute.
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(editor.GetAssets().Resolve("Assets/Models/Textured.gltf") == handle);
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Assets/Models/Textures/Checker.png.meta")));
			CHECK(ListAssetTree(editor) == before);
			REQUIRE(editor.GetHistory().Redo(editor).has_value());
			CHECK(ListAssetTree(editor) == executed);
			CHECK(editor.GetAssets().Resolve("Assets/Levels/Track.gltf") == handle);
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(ListAssetTree(editor) == before);
			// The folder can be created again, as before the move.
			CHECK(editor.CreateProjectDirectory(MakeProjectPath("Assets/Levels")).has_value());
		}

		TEST_CASE("AssetMoveCommand: an occupied destination is refused and nothing moves")
		{
			Test::EditorTestFixture fixture("AssetMoveOccupied");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			REQUIRE(editor.WriteProjectFile(MakeProjectPath("Assets/A.material"), AsBytes(std::string_view(R"({"Format": "Material", "Version": 1})"))).has_value());
			REQUIRE(editor.WriteProjectFile(MakeProjectPath("Assets/B.material"), AsBytes(std::string_view(R"({"Format": "Material", "Version": 1})"))).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const AssetHandle a = editor.GetAssets().Resolve("Assets/A.material").value_or(AssetHandle());
			Result<Scope<AssetMoveCommand>> command = AssetMoveCommand::Create(editor, a, MakeProjectPath("Assets/B.material"));
			REQUIRE_FALSE(command.has_value());
			CHECK(command.error().GetCode() == ErrorCode::AlreadyExists);
		}
	}

}
