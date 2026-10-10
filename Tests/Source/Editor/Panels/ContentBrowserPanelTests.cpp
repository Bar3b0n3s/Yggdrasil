#include "TestsPCH.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/AssetDesignInteraction.h"
#include "Editor/PanelInteractionFixture.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/ProjectLauncher.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
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

	static ImGuiID ContentControlID(ImGuiWindow* window, EditorContext& editor, const char* label)
	{
		const std::string project = FileSystem::PathToUtf8(editor.GetProject().GetProjectFile());
		return ImHashStr(label, 0, Test::AssetDesignInteraction::AuthoredScope(window->ID, project));
	}

	static void CreateContentAsset(Test::AssetDesignInteraction& ui, const std::function<Status()>& draw,
		EditorContext& editor, int choice, const char* type, const char* name)
	{
		auto* root = ui.Window("Panel interaction");
		ui.Click(draw, root, ContentControlID(root, editor, "Create"));
		auto* create = ui.Popup();
		ui.Click(draw, create, "##AssetType");
		auto* combo = ui.Popup();
		ui.Click(draw, combo, ImHashStr(type, 0, combo->GetID(choice)));
		ui.Type(draw, create, create->GetID("##AssetName"), name);
		ui.Click(draw, create, "Create asset");
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("ContentBrowserPanel: Behaviour Module and Test templates queue undoable script creation")
		{
			for (int choice = 5; choice <= 7; ++choice)
			{
				INFO(choice);
				Test::AutomationFixture fixture("ContentCreateScript", false, std::nullopt, true);
				auto& editor = fixture.GetEditor();
				auto& server = fixture.GetClient().GetServer();
				EditorActions actions(editor, server);
				EditorAutomationControls controls(editor, server);
				ContentBrowserTestViewport viewports;
				ReflectedEditController edits(editor);
				GizmoController gizmos(editor);
				ThumbnailCache thumbnails(editor, {});
				EditorPanelContext context{ editor, server, actions, controls, viewports, edits, thumbnails, gizmos };
				ContentBrowserPanel panel;
				Test::AssetDesignInteraction ui;
				const auto draw = [&panel, &context]()
				{
					return panel.Draw(context);
				};
				REQUIRE(ui.Frame(draw));
				constexpr const char* ScriptTypes[] = { "Behaviour script", "Module script", "Test script" };
				CreateContentAsset(ui, draw, editor, choice, ScriptTypes[choice - 5], "ClickScript");
				const std::string path = choice == 7 ? "Assets/ClickScript.test.luau" : "Assets/ClickScript.luau";
				CHECK_FALSE(context.Editor.GetAssets().Resolve(path));
				CHECK(context.Editor.GetHistory().GetUndoCount() == 0);
				context.Actions.Pump();
				context.Editor.GetAssets().WaitIdle();
				context.Actions.Pump();
				const auto asset = context.Editor.GetAssets().Resolve(path);
				REQUIRE(asset);
				const auto fields = fixture.Call("script.fields", Json{ { "script", asset->ToString() } });
				REQUIRE_MESSAGE(fields, (fields ? "" : fields.error().ToString()));
				CHECK((*fields)["kind"] == Json(choice == 5 ? "Behaviour" : choice == 6 ? "Module"
																						: "TestSuite"));
				const auto history = context.Editor.GetHistory().GetEntries(10);
				REQUIRE(history.size() == 1);
				CHECK(history.front().Origin == CommandOrigin::User);
				REQUIRE(ui.Frame(draw));
				REQUIRE(fixture.Call("edit.undo", Json::object()));
				CHECK_FALSE(context.Editor.GetAssets().Resolve(path));
				REQUIRE(fixture.Call("edit.redo", Json::object()));
				CHECK(context.Editor.GetAssets().Resolve(path) == asset);
			}
		}

		TEST_CASE("ContentBrowserPanel: creating a sound effect queues a valid initial layer")
		{
			Test::PanelInteractionFixture fixture("ContentCreateSound", false);
			auto& context = fixture.GetContext();
			ContentBrowserPanel panel;
			Test::AssetDesignInteraction ui;
			const auto draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			CreateContentAsset(ui, draw, context.Editor, 4, "Sound effect", "ClickSound");
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
			Test::AssetDesignInteraction ui;
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
			Test::AssetDesignInteraction ui;
			const auto draw = [&panel, &context]()
			{
				const auto result = panel.Draw(context);
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
			auto* root = ui.Window("Panel interaction");
			ui.Click(draw, root, ContentControlID(root, context.Editor, "Actions"));
			ui.Click(draw, ui.Popup(), "Move to trash...");
			ui.Click(draw, ui.Popup(), "Move to trash");
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
			Test::AssetDesignInteraction ui;
			const auto draw = [&panel, &context]()
			{
				const auto result = panel.Draw(context);
				return result;
			};
			REQUIRE(ui.Frame(draw));
			const auto historySize = context.Editor.GetHistory().GetEntries(100).size();
			auto* root = ui.Window("Panel interaction");
			ui.Click(draw, root, ContentControlID(root, context.Editor, "Preview"));
			REQUIRE(queued.size() == 1);
			CHECK(queued.front() == *asset);
			ui.Click(draw, root, ContentControlID(root, context.Editor, "Stop"));
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
			Test::AssetDesignInteraction ui;
			const auto draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
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
			REQUIRE(ui.Frame(draw));
			REQUIRE(thumbnails.Pump());
			CHECK(renders == 1);
			auto* root = ui.Window("Panel interaction");
			auto* grid = ui.Child(root, ImHashStr("AssetTiles", 0, ContentControlID(root, editor, "ContentColumns")));
			const ImGuiID tileID = ImHashStr("##Asset", 0, ImHashStr(id->ToString().c_str(), 0, grid->GetID("AssetGrid")));
			const ImRect tile = ui.Locate(draw, grid, tileID);
			const ImVec2 center = tile.GetCenter();
			ImGui::GetIO().AddMousePosEvent(center.x, center.y);
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddMousePosEvent(center.x + ImGui::GetIO().MouseDragThreshold * 2.0f, center.y);
			REQUIRE(ui.Frame(draw));
			const ImGuiPayload* payload = ImGui::GetDragDropPayload();
			REQUIRE(payload);
			CHECK(payload->IsDataType("ENGINE_ASSET"));
			REQUIRE(payload->DataSize == 17);
			CHECK(std::string_view(static_cast<const char*>(payload->Data), 16) == id->ToString());
			CHECK(static_cast<const char*>(payload->Data)[16] == '\0');
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			REQUIRE(ui.Frame(draw));
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
