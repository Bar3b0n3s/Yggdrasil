#include "TestsPCH.h"

#include "EditorCore/Commands/AssetDeleteCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/EditorTestFixture.h"

namespace Engine {

	namespace {

		VfsPath MakeProjectPath(std::string_view relative)
		{
			Result<VfsPath> path = VfsPath::Create("project", relative);
			REQUIRE(path.has_value());
			return *path;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("AssetDeleteCommand: trashes an asset with its meta and undo restores it with its handle" * doctest::skip(true))
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
			Result<Scope<AssetDeleteCommand>> command = AssetDeleteCommand::Create(editor, handle);
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());
			CHECK_FALSE(editor.GetVfs().Exists(MakeProjectPath("Assets/Red.material")));
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Library/Trash/" + handle.ToString() + "/Assets/Red.material")));
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Library/Trash/" + handle.ToString() + "/Assets/Red.material.meta")));
			CHECK_FALSE(editor.GetAssets().Resolve("Assets/Red.material").has_value());
			CHECK(editor.GetProvenance()->Find("Assets/Red.material") == nullptr);

			// A second delete of the same asset (after undo and redo) picks the next free entry.
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(editor.GetAssets().Resolve("Assets/Red.material") == handle);
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Assets/Red.material.meta")));
			REQUIRE(editor.WriteProjectFile(MakeProjectPath("Library/Trash/" + handle.ToString() + "/Placeholder.txt"), AsBytes(material)).has_value());
			Result<VfsPath> next = AssetDeleteCommand::ChooseTrashDirectory(editor, handle);
			REQUIRE(next.has_value());
			CHECK(next->ToString() == "project://Library/Trash/" + handle.ToString() + "-2");
		}
	}

}
