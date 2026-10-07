#include "TestsPCH.h"

#include "EditorCore/Commands/AssetEditCommand.h"

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

		Buffer MakeBuffer(std::string_view text)
		{
			return Buffer(AsBytes(text).begin(), AsBytes(text).end());
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("AssetEditCommand: execute writes and undo restores the file bytes")
		{
			Test::EditorTestFixture fixture("AssetEditCommand");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const VfsPath path = MakeProjectPath("Assets/Red.material");
			const std::string before = R"({"Format": "Material", "Version": 1, "Roughness": 0.5})";
			REQUIRE(editor.WriteProjectFile(path, AsBytes(before)).has_value());

			const std::string after = R"({"Format": "Material", "Version": 1, "Roughness": 0.9})";
			Result<Scope<AssetEditCommand>> command = AssetEditCommand::CreateForWrite(editor, path, AsBytes(after), "Set Material 'Red'");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			CHECK_FALSE((*command)->ChangesScene());
			CHECK((*command)->GetMemorySize() >= before.size() + after.size());
			Result<uint64_t> executed = editor.Execute(std::move(*command));
			REQUIRE_MESSAGE(executed.has_value(), executed.error().ToString());
			CHECK(editor.GetVfs().ReadText(path).value_or(std::string()) == after);
			// Provenance records the write (§13.4).
			REQUIRE(editor.GetProvenance() != nullptr);
			CHECK(editor.GetProvenance()->Find("Assets/Red.material") != nullptr);

			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(editor.GetVfs().ReadText(path).value_or(std::string()) == before);
			REQUIRE(editor.GetHistory().Redo(editor).has_value());
			CHECK(editor.GetVfs().ReadText(path).value_or(std::string()) == after);
		}

		TEST_CASE("AssetEditCommand: a created file and folder are removed by undo")
		{
			Test::EditorTestFixture fixture("AssetEditCreate");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			std::vector<AssetFileEdit> edits;
			edits.push_back({ .Path = MakeProjectPath("Assets/Materials"), .Kind = AssetFileKind::Directory, .Before = std::nullopt, .After = Buffer() });
			edits.push_back({ .Path = MakeProjectPath("Assets/Materials/Blue.material"), .Kind = AssetFileKind::File, .Before = std::nullopt, .After = MakeBuffer(R"({"Format": "Material", "Version": 1})") });
			REQUIRE(editor.Execute(CreateScope<AssetEditCommand>("Create Material 'Blue'", std::move(edits))).has_value());
			CHECK(editor.GetVfs().Exists(MakeProjectPath("Assets/Materials/Blue.material")));
			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK_FALSE(editor.GetVfs().Exists(MakeProjectPath("Assets/Materials/Blue.material")));
			CHECK_FALSE(editor.GetVfs().Exists(MakeProjectPath("Assets/Materials")));
			CHECK(editor.GetProvenance()->Find("Assets/Materials/Blue.material") == nullptr);
		}

		TEST_CASE("AssetEditCommand: a failing entry restores the earlier ones")
		{
			Test::EditorTestFixture fixture("AssetEditAtomic");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const VfsPath first = MakeProjectPath("Assets/First.material");
			REQUIRE(editor.WriteProjectFile(first, AsBytes(std::string_view("old"))).has_value());
			std::vector<AssetFileEdit> edits;
			edits.push_back({ .Path = first, .Kind = AssetFileKind::File, .Before = MakeBuffer("old"), .After = MakeBuffer("new") });
			// The second write fails: its parent directory does not exist.
			edits.push_back({ .Path = MakeProjectPath("Assets/Missing/Second.material"), .Kind = AssetFileKind::File, .Before = std::nullopt, .After = MakeBuffer("second") });
			Result<uint64_t> executed = editor.Execute(CreateScope<AssetEditCommand>("Write Two", std::move(edits)));
			REQUIRE_FALSE(executed.has_value());
			CHECK(editor.GetVfs().ReadText(first).value_or(std::string()) == "old");
			CHECK_FALSE(editor.GetHistory().CanUndo());
		}

		TEST_CASE("AssetEditCommand: an edit that removes a file brings it back on undo")
		{
			Test::EditorTestFixture fixture("AssetEditRemove");
			{
				// No project: nothing to write to.
				Result<Scope<AssetEditCommand>> launcher = AssetEditCommand::CreateForWrite(fixture.GetEditor(), MakeProjectPath("Assets/Red.material"),
					AsBytes(std::string_view("{}")), "Write");
				REQUIRE_FALSE(launcher.has_value());
				CHECK(launcher.error().GetCode() == ErrorCode::InvalidState);
			}
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			const VfsPath path = MakeProjectPath("Assets/Old.material");
			const std::string content = R"({"Format": "Material", "Version": 1})";
			REQUIRE(editor.WriteProjectFile(path, AsBytes(content)).has_value());
			std::vector<AssetFileEdit> edits;
			edits.push_back({ .Path = path, .Kind = AssetFileKind::File, .Before = MakeBuffer(content), .After = std::nullopt });
			REQUIRE(editor.Execute(CreateScope<AssetEditCommand>("Remove 'Old'", std::move(edits))).has_value());
			CHECK_FALSE(editor.GetVfs().Exists(path));
			CHECK(editor.GetProvenance()->Find("Assets/Old.material") == nullptr);
			CHECK_FALSE(editor.IsSceneDirty());

			REQUIRE(editor.GetHistory().Undo(editor).has_value());
			CHECK(editor.GetVfs().ReadText(path).value_or(std::string()) == content);
			CHECK(editor.GetProvenance()->Find("Assets/Old.material") != nullptr);

			// CreateForWrites reads each file's state as Before: a new file has none, an existing one its bytes.
			const std::pair<VfsPath, Buffer> files[] = { { path, MakeBuffer("{}") }, { MakeProjectPath("Assets/New.material"), MakeBuffer("{}") } };
			Result<Scope<AssetEditCommand>> command = AssetEditCommand::CreateForWrites(editor, files, "Write Both");
			REQUIRE(command.has_value());
			REQUIRE((*command)->GetEdits().size() == 2);
			CHECK((*command)->GetEdits()[0].Before == std::optional<Buffer>(MakeBuffer(content)));
			CHECK_FALSE((*command)->GetEdits()[1].Before.has_value());
		}
	}

}
