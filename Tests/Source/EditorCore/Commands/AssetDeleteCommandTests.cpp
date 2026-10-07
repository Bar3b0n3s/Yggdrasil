#include "TestsPCH.h"

#include "EditorCore/Commands/AssetDeleteCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/EditorTestFixture.h"

#include <algorithm>

namespace Engine {

	namespace {

		VfsPath MakeProjectPath(std::string_view relative)
		{
			Result<VfsPath> path = VfsPath::Create("project", relative);
			REQUIRE(path.has_value());
			return *path;
		}

		// Every file and folder (with a trailing '/') under project://Assets and project://Library/Trash, sorted: the state
		// §12.3's property compares (Execute then Undo equals the original, Execute then Undo then Redo equals Execute).
		std::vector<std::string> ListTrashTree(EditorContext& editor)
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
		TEST_CASE("AssetDeleteCommand: trashes an asset with its meta and undo restores it with its handle")
		{
			Test::EditorTestFixture fixture("AssetDeleteCommand");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const std::string material = R"({"Format": "Material", "Version": 1})";
			REQUIRE(editor.WriteProjectFile(MakeProjectPath("Assets/Red.material"), AsBytes(material)).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const AssetHandle handle = editor.GetAssets().Resolve("Assets/Red.material").value_or(AssetHandle());
			REQUIRE(handle.IsValid());

			Result<VfsPath> trash = AssetDeleteCommand::ChooseTrashDirectory(editor, handle);
			REQUIRE(trash.has_value());
			CHECK(trash->ToString() == "project://Library/Trash/" + handle.ToString());
			const std::vector<std::string> before = ListTrashTree(editor);
			Result<Scope<AssetDeleteCommand>> command = AssetDeleteCommand::Create(editor, handle);
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			const std::vector<std::string> executed = ListTrashTree(editor);
			CHECK_FALSE(editor.GetVfs().Exists(MakeProjectPath("Assets/Red.material")));
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Library/Trash/" + handle.ToString() + "/Assets/Red.material")));
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Library/Trash/" + handle.ToString() + "/Assets/Red.material.meta")));
			CHECK_FALSE(editor.GetAssets().Resolve("Assets/Red.material").has_value());
			CHECK(editor.GetProvenance()->Find("Assets/Red.material") == nullptr);

			// §12.3: Undo restores the exact prior state (the trash folders the delete created go too), and Redo gives exactly
			// the state after Execute, in the same trash entry.
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(editor.GetAssets().Resolve("Assets/Red.material") == handle);
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Assets/Red.material.meta")));
			CHECK(ListTrashTree(editor) == before);
			REQUIRE(editor.GetHistory().Redo(editor).has_value());
			CHECK(ListTrashTree(editor) == executed);
			CHECK_FALSE(editor.GetAssets().Resolve("Assets/Red.material").has_value());
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(ListTrashTree(editor) == before);
			CHECK(editor.GetAssets().Resolve("Assets/Red.material") == handle);

			// While an earlier delete of the same asset occupies its trash entry, the next delete picks the next free one.
			REQUIRE(editor.WriteProjectFile(MakeProjectPath("Library/Trash/" + handle.ToString() + "/Placeholder.txt"), AsBytes(material)).has_value());
			Result<VfsPath> next = AssetDeleteCommand::ChooseTrashDirectory(editor, handle);
			REQUIRE(next.has_value());
			CHECK(next->ToString() == "project://Library/Trash/" + handle.ToString() + "-2");
		}
	}

}
