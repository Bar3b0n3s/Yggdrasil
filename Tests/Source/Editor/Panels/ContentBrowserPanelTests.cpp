#include "TestsPCH.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/PanelInteractionFixture.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/ProjectLauncher.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"

#include <imgui.h>

#include <utility>

namespace Engine {

	class ContentBrowserTestViewport final : public EditorViewportHost
	{
	public:
		EditorViewportImage GetImage(ViewportView) const override { return {}; }
		void SetRectangle(ViewportView, const EditorViewportRect&) override { FAIL("content UI must not modify viewport rectangles"); }
		Status RequestPick(const EditorViewportClick&) override { return MakeError(ErrorCode::Unsupported, "viewport not used by content tests"); }
		Status ConfigureSceneSnapshot(RenderSnapshot&, const EditorViewportOptions&, std::span<const UUID>) override { return MakeError(ErrorCode::Unsupported, "viewport not used by content tests"); }
		void SetGameInputFocused(bool) override { FAIL("content UI must not alter game input"); }
	};

	class ContentBrowserTestUi
	{
	public:
		ContentBrowserTestUi()
			: m_Context(ImGui::CreateContext())
		{
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1000.0f, 700.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		}
		~ContentBrowserTestUi() { ImGui::DestroyContext(m_Context); }
		void Begin()
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(1000.0f, 700.0f));
			ImGui::Begin("Content test");
		}
		void End()
		{
			ImGui::End();
			ImGui::Render();
		}
	private:
		ImGuiContext* m_Context = nullptr;
	};

	TEST_SUITE("Editor")
	{
		TEST_CASE("ContentBrowserPanel: creating a sound effect queues a valid initial layer")
		{
			Test::PanelInteractionFixture fixture("ContentCreateSound", false);
			auto& context = fixture.GetContext();
			ContentBrowserPanel panel;
			Test::PanelInteractionUi ui;
			ImVec2 origin{};
			const auto draw = [&panel, &context, &origin]()
			{
				origin = ImGui::GetCursorScreenPos();
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			const float frame = ImGui::GetFrameHeightWithSpacing();
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, origin.y + 2.0f * frame + 8.0f)));
			const float comboY = origin.y + 3.0f * frame;
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, comboY + 8.0f)));
			// The combo popup lists Folder, Scene, Material, Prefab, SoundEffect in that order.
			const float soundY = comboY + ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y
				+ 4.0f * ImGui::GetTextLineHeightWithSpacing() + 6.0f;
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, soundY)));
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 80.0f, comboY + frame + 8.0f)));
			ImGui::GetIO().AddInputCharactersUTF8("ClickSound");
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Click(draw, ImVec2(origin.x + 22.0f, comboY + 2.0f * frame + 8.0f)));
			CHECK_FALSE(context.Editor.GetAssets().Resolve("Assets/ClickSound.sfx"));
			CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
			context.Actions.Pump();
			context.Editor.GetAssets().WaitIdle();
			context.Actions.Pump();
			const auto asset = context.Editor.GetAssets().Resolve("Assets/ClickSound.sfx");
			REQUIRE(asset);
			const auto properties = fixture.GetAutomation().Call("asset.getProperties", Json{ { "asset", asset->ToString() } });
			REQUIRE(properties);
			const auto history = context.Editor.GetHistory().GetEntries(10);
			REQUIRE(history.size() == 1);
			CHECK(history.front().Origin == CommandOrigin::User);
			REQUIRE(ui.Frame(draw)); // drain the real command result; failures must not be logged
		}

		TEST_CASE("ContentBrowserPanel: registered thumbnails enter the real ImGui draw list without rendering in Draw")
		{
			Test::PanelInteractionFixture fixture("ContentTexture", false);
			auto& context = fixture.GetContext();
			const auto created = fixture.GetAutomation().Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Preview.material" } });
			REQUIRE(created);
			const auto asset = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(asset);
			REQUIRE(context.Thumbnails.BindProject(7));
			uint32_t lookups = 0;
			context.FindThumbnailTexture = [&lookups, asset = *asset](const ThumbnailRequest& request)
			{
				++lookups;
				CHECK(request.ProjectGeneration == 7);
				CHECK(request.Asset == asset);
				CHECK(request.Size == 128);
				return uint64_t{ 912345 };
			};
			ContentBrowserPanel panel;
			Test::PanelInteractionUi ui;
			const auto draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			CHECK(lookups == 0);
			CHECK(fixture.GetRenderCount() == 0);
			REQUIRE(context.Thumbnails.Pump());
			CHECK(fixture.GetRenderCount() == 1);
			REQUIRE(ui.Frame(draw));
			CHECK(lookups == 1);
			bool found = false;
			const ImDrawData* data = ImGui::GetDrawData();
			for (const ImDrawList* list : data->CmdLists)
				for (const ImDrawCmd& command : list->CmdBuffer)
					found |= command.TexRef.GetTexID() == 912345;
			CHECK(found);
			CHECK(fixture.GetRenderCount() == 1);
			context.Thumbnails.Reset();
			REQUIRE(ui.Frame(draw));
			CHECK(lookups == 1);
		}

		TEST_CASE("ContentBrowserPanel: an OS drop queues a human import and trash click shares undo")
		{
			Test::PanelInteractionFixture fixture("ContentImport", false);
			auto& context = fixture.GetContext();
			const auto source = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "Dropped.material";
			const auto text = MaterialToText(MaterialData{}, context.Editor.GetTypeRegistry());
			REQUIRE(text);
			REQUIRE(FileSystem::WriteFileAtomic(source, AsBytes(*text)));
			std::vector<std::filesystem::path> drops{ source };
			context.TakeContentDrops = [&drops]()
			{
				return std::exchange(drops, {});
			};
			ContentBrowserPanel panel;
			Test::PanelInteractionUi ui;
			ImVec2 rename{};
			const auto draw = [&panel, &context, &rename]()
			{
				const auto result = panel.Draw(context);
				rename = Test::PanelInteractionUi::LastItemCenter();
				return result;
			};
			REQUIRE(ui.Frame(draw));
			CHECK(drops.empty());
			const auto importedPath = context.Editor.GetProject().GetRoot() / "Assets/Dropped.material";
			CHECK_FALSE(FileSystem::Exists(importedPath));
			CHECK(context.Editor.GetHistory().GetEntries(10).empty());
			context.Actions.Pump();
			context.Editor.GetAssets().WaitIdle();
			context.Actions.Pump();
			CHECK(FileSystem::Exists(importedPath));
			const auto asset = context.Editor.GetAssets().Resolve("Assets/Dropped.material");
			REQUIRE(asset);
			const auto history = context.Editor.GetHistory().GetEntries(10);
			REQUIRE(history.size() == 1);
			CHECK(history.front().Origin == CommandOrigin::User);
			context.Editor.GetUiState().SetSelectedAsset(*asset);
			REQUIRE(ui.Frame(draw));
			const float trashY = rename.y - 2.0f * ImGui::GetFrameHeightWithSpacing();
			REQUIRE(ui.Click(draw, ImVec2(140.0f, trashY)));
			CHECK(FileSystem::Exists(importedPath));
			context.Actions.Pump();
			CHECK_FALSE(FileSystem::Exists(importedPath));
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			CHECK(FileSystem::Exists(importedPath));
			CHECK(context.Editor.GetAssets().Resolve("Assets/Dropped.material") == asset);
		}

		TEST_CASE("ContentBrowserPanel: audio preview and stop buttons only queue host requests")
		{
			Test::PanelInteractionFixture fixture("ContentPreview", false);
			auto& context = fixture.GetContext();
			const auto created = fixture.GetAutomation().Call("asset.create", Json{ { "type", "SoundEffect" }, { "path", "Assets/Preview.sfx" }, { "values", Json{ { "Layers", Json::array({ Json{ { "Duration", 0.1 } } }) } } } });
			REQUIRE(created);
			const auto asset = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(asset);
			context.Editor.GetUiState().SetSelectedAsset(*asset);
			std::vector<AssetHandle> queued;
			context.QueueAudioPreview = [&queued](AssetHandle handle) -> Status
			{
				queued.push_back(handle);
				return {};
			};
			ContentBrowserPanel panel;
			Test::PanelInteractionUi ui;
			ImVec2 stop{};
			const auto draw = [&panel, &context, &stop]()
			{
				const auto result = panel.Draw(context);
				stop = Test::PanelInteractionUi::LastItemCenter();
				return result;
			};
			REQUIRE(ui.Frame(draw));
			const auto historySize = context.Editor.GetHistory().GetEntries(100).size();
			REQUIRE(ui.Click(draw, ImVec2(48.0f, stop.y)));
			REQUIRE(queued.size() == 1);
			CHECK(queued.front() == *asset);
			REQUIRE(ui.Click(draw, stop));
			REQUIRE(queued.size() == 2);
			CHECK_FALSE(queued.back().IsValid());
			CHECK(context.Editor.GetHistory().GetEntries(100).size() == historySize);
		}

		TEST_CASE("ContentBrowserPanel: drawing queues thumbnails without rendering or changing project state")
		{
			Test::AutomationFixture fixture("ContentDraw", false);
			auto& editor = fixture.GetEditor();
			const auto created = fixture.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Test.material" } });
			REQUIRE(created);
			const auto id = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(id);
			auto& server = fixture.GetClient().GetServer();
			EditorActions actions(editor, server);
			EditorAutomationControls controls(editor, server);
			ContentBrowserTestViewport viewports;
			ReflectedEditController edits(editor);
			GizmoController gizmos(editor);
			uint32_t renders = 0;
			ThumbnailCache thumbnails(editor, [&renders](const ThumbnailRequest& request)
			{
				++renders;
				return CreateImage(request.Size, request.Size, nvrhi::Format::RGBA8_UNORM);
			});
			REQUIRE(thumbnails.BindProject(1));
			EditorPanelContext context{ editor, server, actions, controls, viewports, edits, thumbnails, gizmos };
			ContentBrowserPanel panel;
			const uint64_t revision = editor.GetRevision();
			const size_t history = editor.GetHistory().GetEntries(100).size();
			const auto files = FileSystem::ListDirectory(editor.GetProject().GetRoot(), true);
			REQUIRE(files);
			ContentBrowserTestUi ui;
			ui.Begin();
			const float assetY = ImGui::GetCursorScreenPos().y + 3.0f * ImGui::GetFrameHeightWithSpacing() + 24.0f;
			REQUIRE(panel.Draw(context));
			ui.End();
			CHECK(renders == 0);
			CHECK(editor.GetRevision() == revision);
			CHECK(editor.GetHistory().GetEntries(100).size() == history);
			const auto after = FileSystem::ListDirectory(editor.GetProject().GetRoot(), true);
			REQUIRE(after);
			CHECK(*after == *files);
			REQUIRE(thumbnails.Pump());
			CHECK(renders == 1);
			const ThumbnailRequest request{ 1, *id, editor.GetAssets().GetVersion(*id), 128 };
			REQUIRE(thumbnails.Find(request));
			CHECK(thumbnails.Find(request)->has_value());
			ui.Begin();
			REQUIRE(panel.Draw(context));
			ui.End();
			REQUIRE(thumbnails.Pump());
			CHECK(renders == 1);
			// Click and drag the first grid cell in the fixed-size test window, through public ImGui input events.
			ImGui::GetIO().AddMousePosEvent(250.0f, assetY);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			ui.Begin();
			REQUIRE(panel.Draw(context));
			ui.End();
			ImGui::GetIO().AddMousePosEvent(300.0f, assetY + 10.0f);
			ui.Begin();
			REQUIRE(panel.Draw(context));
			const ImGuiPayload* payload = ImGui::GetDragDropPayload();
			REQUIRE(payload);
			CHECK(payload->IsDataType("ENGINE_ASSET"));
			REQUIRE(payload->DataSize == 17);
			CHECK(std::string_view(static_cast<const char*>(payload->Data), 16) == id->ToString());
			CHECK(static_cast<const char*>(payload->Data)[16] == '\0');
			ui.End();
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			ui.Begin();
			REQUIRE(panel.Draw(context));
			ui.End();
			CHECK(editor.GetHistory().GetEntries(100).size() == history);
		}

		TEST_CASE("ProjectLauncher: drawing recent projects preserves launcher and preferences")
		{
			Test::EditorTestFixture fixture("LauncherDraw");
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			auto& server = client.GetServer();
			EditorActions actions(editor, server);
			EditorAutomationControls controls(editor, server);
			ContentBrowserTestViewport viewports;
			ReflectedEditController edits(editor);
			GizmoController gizmos(editor);
			ThumbnailCache thumbnails(editor, {});
			EditorPanelContext context{ editor, server, actions, controls, viewports, edits, thumbnails, gizmos };
			REQUIRE(ProjectManager::AddRecentProject(editor.GetVfs(), fixture.GetProjectRoot() / "Recent.eproj"));
			const auto before = ProjectManager::ReadRecentProjects(editor.GetVfs());
			REQUIRE(before);
			ProjectLauncher launcher;
			ContentBrowserTestUi ui;
			ui.Begin();
			REQUIRE(launcher.Draw(context));
			ui.End();
			actions.Pump();
			CHECK_FALSE(editor.HasProject());
			const auto after = ProjectManager::ReadRecentProjects(editor.GetVfs());
			REQUIRE(after);
			CHECK(*after == *before);
		}
	}

}
