#include "TestsPCH.h"
#include "Editor/FolderPicker.h"

#include "Editor/AssetDesignInteraction.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/SupportingPanelTestUi.h"
#include "Editor/Ui/EditorStyle.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VfsPath.h"
#include "Support/TempDirectory.h"

#include <utility>

namespace Engine {

	namespace Utils {

		static ImGuiID AssetDesignControl(ImGuiWindow* window, EditorContext& editor, const char* label)
		{
			const std::string path = FileSystem::PathToUtf8(editor.GetProject().GetProjectFile());
			return ImHashStr(label, 0, Test::AssetDesignInteraction::AuthoredScope(window->ID, path));
		}

		// Restrict the rendered-text assertion to the measured row: the same name can also appear in
		// the folder tree, a breadcrumb or a tooltip. Use copied glyphs from the active font.
		static bool AssetDesignTextInRow(const ImDrawList& draw, const std::vector<ImFontGlyph>& glyphs, const ImRect& row)
		{
			if (glyphs.empty())
				return false;
			const size_t vertices = static_cast<size_t>(draw.VtxBuffer.Size);
			for (size_t first = 0; first + glyphs.size() * 4 <= vertices; ++first)
			{
				if (!row.Contains(draw.VtxBuffer[static_cast<int>(first)].pos)
					|| !row.Contains(draw.VtxBuffer[static_cast<int>(first + glyphs.size() * 4 - 2)].pos))
					continue;
				bool matches = true;
				for (size_t index = 0; index < glyphs.size(); ++index)
				{
					const auto& top = draw.VtxBuffer[static_cast<int>(first + index * 4)];
					const auto& bottom = draw.VtxBuffer[static_cast<int>(first + index * 4 + 2)];
					const auto& glyph = glyphs[index];
					if (top.uv.x < glyph.U0 || top.uv.y < glyph.V0 || bottom.uv.x > glyph.U1 || bottom.uv.y > glyph.V1 || top.uv.x >= bottom.uv.x || top.uv.y >= bottom.uv.y)
					{
						matches = false;
						break;
					}
				}
				if (matches)
					return true;
			}
			return false;
		}

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorAssetDesign: authored folder labels retain distinct browsing and undoable move targets")
		{
			Test::PanelInteractionFixture fixture("AssetLiteralPaths", false);
			auto& context = fixture.GetContext();
			auto& editor = context.Editor;
			const std::vector<std::string> destinations{ "Assets/##One", "Assets/##Two", "Assets/A###Same", "Assets/B###Same" };
			for (const auto& path : destinations)
				REQUIRE(FileSystem::CreateDirectories(editor.GetProject().GetRoot() / FileSystem::PathFromUtf8(path)));
			REQUIRE(fixture.GetAutomation().Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Preview.material" } }));
			const auto asset = editor.GetAssets().Resolve("Assets/Preview.material");
			REQUIRE(asset);
			const auto originalSourceFile = editor.GetProject().GetRoot() / "Assets/Preview.material";
			const auto originalMetaFile = editor.GetProject().GetRoot() / "Assets/Preview.material.meta";
			editor.GetUiState().SetSelectedAsset(*asset);
			ContentBrowserPanel panel;
			Test::AssetDesignInteraction ui;
			std::string visibleLabel = destinations.front();
			std::vector<ImFontGlyph> glyphs;
			const auto draw = [&panel, &context, &visibleLabel, &glyphs]() -> Status
			{
				ENGINE_TRY(panel.Draw(context));
				glyphs = Test::SupportingPanelTestUi::CaptureGlyphs(visibleLabel);
				return {};
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			for (const auto& path : destinations)
			{
				CAPTURE(path);
				visibleLabel = path;
				ui.Click(draw, root, Utils::AssetDesignControl(root, editor, "Actions"));
				ui.Click(draw, ui.Popup(), "Move to folder...");
				auto* popup = ui.Popup();
				auto* list = ui.Child(popup, popup->GetID("Destinations"));
				const ImGuiID id = ImHashStr("##Destination", 0, Test::AssetDesignInteraction::AuthoredScope(list->ID, path));
				const ImRect row = ui.Locate(draw, list, id);
				CHECK(Utils::AssetDesignTextInRow(*list->DrawList, glyphs, row));
				REQUIRE(ui.Click(draw, row.GetCenter()));
				ui.Click(draw, popup, "Move");
				CHECK(editor.GetAssets().Resolve("Assets/Preview.material") == asset);
				context.Actions.Pump();
				const std::string moved = path + "/Preview.material";
				const auto movedPath = VfsPath::Create("project", moved);
				REQUIRE(movedPath);
				const auto movedMetaPath = GetMetaPath(*movedPath);
				REQUIRE(movedMetaPath);
				// Resolve parses '#' as a sub-asset separator; these authored file paths require a literal lookup.
				const auto* movedRecord = editor.GetAssets().GetRegistry().FindBySourcePath(*movedPath);
				REQUIRE(movedRecord != nullptr);
				CHECK(movedRecord->Metadata.Handle == *asset);
				CHECK(movedRecord->SourcePath == *movedPath);
				CHECK(movedRecord->MetaPath == *movedMetaPath);
				const auto movedSourceFile = editor.GetProject().GetRoot() / FileSystem::PathFromUtf8(movedPath->GetPath());
				const auto movedMetaFile = editor.GetProject().GetRoot() / FileSystem::PathFromUtf8(movedMetaPath->GetPath());
				CHECK(FileSystem::Exists(movedSourceFile));
				CHECK(FileSystem::Exists(movedMetaFile));
				CHECK_FALSE(FileSystem::Exists(originalSourceFile));
				CHECK_FALSE(FileSystem::Exists(originalMetaFile));
				CHECK_FALSE(editor.GetAssets().Resolve("Assets/Preview.material"));
				const auto history = editor.GetHistory().GetEntries(10);
				REQUIRE(history.size() == 2);
				CHECK(history.back().Origin == CommandOrigin::User);
				REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
				CHECK(editor.GetAssets().Resolve("Assets/Preview.material") == asset);
				CHECK(editor.GetAssets().GetRegistry().FindBySourcePath(*movedPath) == nullptr);
				CHECK_FALSE(FileSystem::Exists(movedSourceFile));
				CHECK_FALSE(FileSystem::Exists(movedMetaFile));
				CHECK(FileSystem::Exists(originalSourceFile));
				CHECK(FileSystem::Exists(originalMetaFile));
				REQUIRE(ui.Frame(draw));
			}
			// Exercise the folder tile, literal breadcrumb, and tree with the same colliding suffixes.
			auto* grid = ui.Child(root, ImHashStr("AssetTiles", 0, Utils::AssetDesignControl(root, editor, "ContentColumns")));
			const ImGuiID tileId = ImHashStr("##FolderTile", 0, Test::AssetDesignInteraction::AuthoredScope(grid->GetID("AssetGrid"), "Assets/A###Same"));
			const ImRect tile = ui.Locate(draw, grid, tileId);
			REQUIRE(ui.Click(draw, tile.GetCenter()));
			REQUIRE(ui.Click(draw, tile.GetCenter()));
			visibleLabel = "A###Same";
			const ImGuiID projectScope = Test::AssetDesignInteraction::AuthoredScope(root->ID, FileSystem::PathToUtf8(editor.GetProject().GetProjectFile()));
			const ImGuiID crumbId = ImHashStr("##Breadcrumb", 0, Test::AssetDesignInteraction::AuthoredScope(projectScope, "Assets/A###Same"));
			const ImRect crumb = ui.Locate(draw, root, crumbId);
			CHECK(Utils::AssetDesignTextInRow(*root->DrawList, glyphs, crumb));
			ui.Click(draw, root, ImHashStr("##Breadcrumb", 0, Test::AssetDesignInteraction::AuthoredScope(projectScope, "Assets")));
			auto* tree = ui.Child(root, ImHashStr("FolderTree", 0, Utils::AssetDesignControl(root, editor, "ContentColumns")));
			const ImGuiID treeRoot = ImHashStr("##Folder", 0, Test::AssetDesignInteraction::AuthoredScope(tree->ID, "Assets"));
			const ImGuiID treeRow = ImHashStr("##Folder", 0, Test::AssetDesignInteraction::AuthoredScope(treeRoot, "Assets/B###Same"));
			ui.Click(draw, tree, treeRow);
			visibleLabel = "B###Same";
			const ImRect selected = ui.Locate(draw, root, ImHashStr("##Breadcrumb", 0, Test::AssetDesignInteraction::AuthoredScope(projectScope, "Assets/B###Same")));
			CHECK(Utils::AssetDesignTextInRow(*root->DrawList, glyphs, selected));
		}

		TEST_CASE("EditorAssetDesign: compact bottom docks show a complete first row")
		{
			Test::PanelInteractionFixture fixture("CompactAssetGrid", false);
			auto& context = fixture.GetContext();
			Test::AssetDesignInteraction ui(false);
			Utils::ApplyEditorStyle();
			ImVec2 size(1040.0f, 230.0f);
			SUBCASE("900 pixel workspace")
			{
			}
			SUBCASE("720 pixel workspace")
			{
				size = ImVec2(800.0f, 210.0f);
			}
			ContentBrowserPanel panel;
			const auto draw = [&panel, &context, &size]() -> Status
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(size);
				ImGui::Begin("Compact assets");
				const Status result = panel.Draw(context);
				ImGui::End();
				return result;
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Compact assets");
			auto* grid = ui.Child(root, ImHashStr("AssetTiles", 0, Utils::AssetDesignControl(root, context.Editor, "ContentColumns")));
			const ImGuiID folderScope = Test::AssetDesignInteraction::AuthoredScope(grid->GetID("AssetGrid"), "Assets/Audio");
			const ImRect firstTile = ui.Locate(draw, grid, ImHashStr("##FolderTile", 0, folderScope));
			CHECK(firstTile.Min.y >= grid->InnerClipRect.Min.y);
			CHECK(firstTile.Max.y <= grid->InnerClipRect.Max.y);
			CHECK(firstTile.GetHeight() >= 2.0f * ImGui::GetTextLineHeightWithSpacing());
			CHECK(grid->Scroll.y == 0.0f);
		}

		TEST_CASE("EditorAssetDesign: file browsing filters extensions and returns a canonical file without typing")
		{
			Test::TempDirectory directory("AssetFilePicker");
			REQUIRE(FileSystem::CreateDirectories(directory / "Models"));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Models/Ship.GLB", AsBytes("model")));
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Models/Other.png", AsBytes("image")));
			Test::AssetDesignInteraction ui(false);
			Utils::ApplyEditorStyle();
			ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
			SUBCASE("standard scale")
			{
			}
			SUBCASE("large scale")
			{
				Utils::ApplyEditorStyle(1.5f);
				ImGui::GetIO().DisplaySize = ImVec2(1600.0f, 900.0f);
			}
			FolderPicker picker;
			REQUIRE(picker.OpenFile("Choose a model", directory.GetPath(), ".glb"));
			Result<std::optional<std::filesystem::path>> result;
			const auto draw = [&picker, &result]() -> Status
			{
				result = picker.Draw();
				return result ? Status{} : std::unexpected(result.error());
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Models")));
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Ship.GLB")));
			// These rows share a position and the clicks fall inside the double-click interval.
			// Entering a folder then selecting its first file must leave confirmation available.
			REQUIRE(picker.IsOpen());
			REQUIRE(result);
			CHECK_FALSE(result->has_value());
			// Neither an invalid mode request nor a failed navigation may lose the selection.
			CHECK_FALSE(picker.OpenFile("Wrong filter", directory.GetPath(), "glb"));
			CHECK_FALSE(picker.Open("Missing folder", directory / "Absent"));
			ui.Click(draw, window, "Open file");
			REQUIRE(result);
			REQUIRE(result->has_value());
			std::error_code error;
			CHECK(**result == std::filesystem::canonical(directory / "Models/Ship.GLB", error));
			CHECK_FALSE(error);
			CHECK_FALSE(picker.IsOpen());
			CHECK(window->Size.x <= ImGui::GetIO().DisplaySize.x);
			CHECK(window->Size.y <= ImGui::GetIO().DisplaySize.y);
		}

		TEST_CASE("EditorAssetDesign: double clicking the same file accepts its canonical path")
		{
			Test::TempDirectory directory("AssetPickerDoubleClick");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Source.png", AsBytes("image")));
			Test::AssetDesignInteraction ui(false);
			FolderPicker picker;
			REQUIRE(picker.OpenFile("Choose an image", directory.GetPath()));
			std::optional<std::filesystem::path> accepted;
			const auto draw = [&picker, &accepted]() -> Status
			{
				ENGINE_TRY_ASSIGN(const auto result, picker.Draw());
				if (result)
					accepted = *result;
				return {};
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			const ImRect row = ui.Locate(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Source.png")));
			REQUIRE(ui.Click(draw, row.GetCenter()));
			REQUIRE(picker.IsOpen());
			CHECK_FALSE(accepted.has_value());
			REQUIRE(ui.Click(draw, row.GetCenter()));
			CHECK_FALSE(picker.IsOpen());
			REQUIRE(accepted.has_value());
			std::error_code error;
			CHECK(*accepted == std::filesystem::canonical(directory / "Source.png", error));
			CHECK_FALSE(error);
		}

		TEST_CASE("EditorAssetDesign: a file removed after selection remains a recoverable picker error")
		{
			Test::TempDirectory directory("AssetPickerRemoved");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Source.png", AsBytes("image")));
			Test::AssetDesignInteraction ui(false);
			FolderPicker picker;
			REQUIRE(picker.OpenFile("Choose an image", directory.GetPath()));
			Result<std::optional<std::filesystem::path>> result;
			const auto draw = [&picker, &result]() -> Status
			{
				result = picker.Draw();
				return {};
			};
			REQUIRE(ui.Frame(draw));
			auto* window = ui.Window("###FolderPicker");
			auto* entries = ui.Child(window, window->GetID("Entries"));
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Source.png")));
			REQUIRE(FileSystem::Remove(directory / "Source.png"));
			ui.Click(draw, window, "Open file");
			REQUIRE_FALSE(result);
			CHECK(result.error().GetCode() == ErrorCode::NotFound);
			CHECK(picker.IsOpen());
			ui.Click(draw, window, "Cancel");
			CHECK_FALSE(picker.IsOpen());
		}

		TEST_CASE("EditorAssetDesign: browsing an import queues one undoable human action at the safe point")
		{
			Test::PanelInteractionFixture fixture("AssetBrowseImport", false);
			auto& context = fixture.GetContext();
			const auto source = context.Editor.GetProject().GetRoot() / "Browsed.material";
			const auto text = MaterialToText(MaterialData{}, context.Editor.GetTypeRegistry());
			REQUIRE(text);
			REQUIRE(FileSystem::WriteFileAtomic(source, AsBytes(*text)));
			ContentBrowserPanel panel;
			Test::AssetDesignInteraction ui;
			Utils::ApplyEditorStyle();
			const auto draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			ui.Click(draw, root, Utils::AssetDesignControl(root, context.Editor, "Import"));
			ui.Click(draw, ui.Popup(), "Browse files...");
			REQUIRE(ui.Frame(draw));
			auto* picker = ui.Window("###FolderPicker");
			auto* entries = ui.Child(picker, picker->GetID("Entries"));
			ui.Click(draw, entries, ImHashStr("##Entry", 0, Test::AssetDesignInteraction::AuthoredScope(entries->ID, "Browsed.material")));
			ui.Click(draw, picker, "Open file");
			ui.Click(draw, ui.Popup(), "Import file");
			CHECK_FALSE(context.Editor.GetAssets().Resolve("Assets/Browsed.material"));
			CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
			context.Actions.Pump();
			context.Editor.GetAssets().WaitIdle();
			context.Actions.Pump();
			REQUIRE(ui.Frame(draw));
			const auto asset = context.Editor.GetAssets().Resolve("Assets/Browsed.material");
			REQUIRE(asset);
			const auto history = context.Editor.GetHistory().GetEntries(10);
			REQUIRE(history.size() == 1);
			CHECK(history.front().Origin == CommandOrigin::User);
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK_FALSE(context.Editor.GetAssets().Resolve("Assets/Browsed.material"));
			REQUIRE(fixture.GetAutomation().Call("edit.redo", Json::object()));
			CHECK(context.Editor.GetAssets().Resolve("Assets/Browsed.material") == asset);
		}

		TEST_CASE("EditorAssetDesign: searching by name or type preserves clickable asset selection")
		{
			Test::PanelInteractionFixture fixture("AssetSearch", false);
			auto& context = fixture.GetContext();
			REQUIRE(fixture.GetAutomation().Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Preview.material" } }));
			const auto asset = context.Editor.GetAssets().Resolve("Assets/Preview.material");
			REQUIRE(asset);
			Test::AssetDesignInteraction ui;
			Utils::ApplyEditorStyle();
			const char* query = "PREVIEW";
			SUBCASE("name is case insensitive")
			{
			}
			SUBCASE("type is case insensitive")
			{
				query = "MATERIAL";
			}
			ContentBrowserPanel panel;
			const auto draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			ui.Type(draw, root, Utils::AssetDesignControl(root, context.Editor, "##SearchAssets"), query);
			auto* grid = ui.Child(root, ImHashStr("AssetTiles", 0, Utils::AssetDesignControl(root, context.Editor, "ContentColumns")));
			const ImGuiID assetScope = ImHashStr(asset->ToString().c_str(), 0, grid->GetID("AssetGrid"));
			const size_t history = context.Editor.GetHistory().GetUndoCount();
			ui.Click(draw, grid, ImHashStr("##Asset", 0, assetScope));
			CHECK(context.Editor.GetUiState().GetSelectedAsset() == *asset);
			CHECK(context.Editor.GetHistory().GetUndoCount() == history);
		}

		TEST_CASE("EditorAssetDesign: read only projects consume drops without queuing writes")
		{
			Test::PanelInteractionFixture fixture("AssetReadOnly", false);
			auto& context = fixture.GetContext();
			const auto projectFile = context.Editor.GetProject().GetProjectFile();
			const auto source = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "Ignored.material";
			const auto text = MaterialToText(MaterialData{}, context.Editor.GetTypeRegistry());
			REQUIRE(text);
			REQUIRE(FileSystem::WriteFileAtomic(source, AsBytes(*text)));
			REQUIRE(context.Editor.CloseProject());
			ProjectOpenOptions options;
			options.ReadOnly = true;
			options.ReadOnlyCacheDirectory = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "ReadOnlyCache";
			auto project = ProjectManager::OpenProject(projectFile, options, context.Editor.GetTypeRegistry());
			REQUIRE(project);
			REQUIRE(context.Editor.OpenProject(std::move(*project)));
			std::vector<std::filesystem::path> drops{ source };
			context.TakeContentDrops = [&drops]()
			{
				return std::exchange(drops, {});
			};
			ContentBrowserPanel panel;
			Test::AssetDesignInteraction ui;
			const auto draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			CHECK(drops.empty());
			context.Actions.Pump();
			CHECK_FALSE(context.Editor.GetAssets().Resolve("Assets/Ignored.material"));
			CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
			CHECK_FALSE(FileSystem::Exists(projectFile.parent_path() / "Assets/Ignored.material"));
		}
	}

}
