#include "TestsPCH.h"
#include "Editor/Panels/ContentBrowserPanel.h"

#include "Editor/AssetDesignInteraction.h"
#include "Editor/EditorPanelContext.h"
#include "Editor/PanelInteractionFixture.h"
#include "Editor/SupportingPanelTestUi.h"
#include "Editor/Ui/EditorStyle.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <string>
#include <utility>
#include <vector>

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

	// Forwards to the project mount it wraps and counts its listings, which the content browser must not repeat per frame.
	class ContentListingCounter final : public IMount
	{
	public:
		explicit ContentListingCounter(Scope<IMount> inner)
			: m_Inner(std::move(inner))
		{
		}
		[[nodiscard]] Result<Buffer> ReadFile(const VfsPath& path) const override { return m_Inner->ReadFile(path); }
		[[nodiscard]] Result<Scope<IFileStream>> Open(const VfsPath& path) const override { return m_Inner->Open(path); }
		[[nodiscard]] Status WriteFileAtomic(const VfsPath& path, std::span<const std::byte> data) override { return m_Inner->WriteFileAtomic(path, data); }
		[[nodiscard]] Status WriteNewFile(const VfsPath& path, std::span<const std::byte> data) override { return m_Inner->WriteNewFile(path, data); }
		[[nodiscard]] Result<FileInfo> GetInfo(const VfsPath& path) const override { return m_Inner->GetInfo(path); }
		[[nodiscard]] Result<std::vector<VfsEntry>> List(const VfsPath& directory, bool recursive) const override
		{
			if (directory.GetPath() == "Assets" && recursive)
				++m_Listings;
			return m_Inner->List(directory, recursive);
		}
		[[nodiscard]] Status CreateDirectories(const VfsPath& directory) override { return m_Inner->CreateDirectories(directory); }
		[[nodiscard]] Status Remove(const VfsPath& path) override { return m_Inner->Remove(path); }
		[[nodiscard]] Status RemoveEmptyDirectory(const VfsPath& directory) override { return m_Inner->RemoveEmptyDirectory(directory); }
		[[nodiscard]] Status Move(const VfsPath& from, const VfsPath& to) override { return m_Inner->Move(from, to); }
		[[nodiscard]] MountAccess GetAccess() const override { return m_Inner->GetAccess(); }
		[[nodiscard]] uint32_t GetListings() const { return m_Listings.load(); }
	private:
		Scope<IMount> m_Inner;
		mutable std::atomic<uint32_t> m_Listings{ 0 };
	};

	// Draws `panel` with ImGui's text log on, keeping everything the frame wrote as text in `text`.
	static std::function<Status()> DrawCapturingText(ContentBrowserPanel& panel, EditorPanelContext& context, std::string& text)
	{
		return [&panel, &context, &text]() -> Status
		{
			ImGui::LogToBuffer();
			const Status drawn = panel.Draw(context);
			text = ImGui::GetCurrentContext()->LogBuffer.c_str();
			ImGui::LogFinish();
			return drawn;
		};
	}

	static void DoubleClickContentItem(Test::AssetDesignInteraction& ui, const std::function<Status()>& draw, ImGuiWindow* window, ImGuiID id)
	{
		const ImVec2 center = ui.Locate(draw, window, id).GetCenter();
		ImGui::GetIO().AddMousePosEvent(center.x, center.y);
		REQUIRE(ui.Frame(draw));
		for (int click = 0; click < 2; ++click)
		{
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			REQUIRE(ui.Frame(draw));
		}
	}

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
			// The first safe point starts the created material's first load; the next renders it once it is published.
			REQUIRE(context.Thumbnails.Pump());
			CHECK(fixture.GetRenderCount() == 0);
			context.Editor.GetAssets().WaitIdle();
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
			// The panel is on screen when the files arrive, so they import into the folder it shows.
			std::vector<std::filesystem::path> visible;
			std::swap(visible, drops);
			REQUIRE(ui.Frame(draw));
			std::swap(visible, drops);
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
			// The first safe point starts the created material's first load; the next renders it once it is published.
			REQUIRE(thumbnails.Pump());
			CHECK(renders == 0);
			editor.GetAssets().WaitIdle();
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

		TEST_CASE("ContentBrowserPanel: drawing lists the project folders again only after a change")
		{
			Test::PanelInteractionFixture fixture("ContentListing", false);
			auto& context = fixture.GetContext();
			VirtualFileSystem& vfs = context.Editor.GetVfs();
			auto inner = vfs.Unmount("project");
			REQUIRE(inner);
			auto counter = CreateScope<ContentListingCounter>(std::move(*inner));
			const ContentListingCounter& listings = *counter;
			REQUIRE(vfs.Mount("project", std::move(counter)));
			ContentBrowserPanel panel;
			Test::AssetDesignInteraction ui;
			// Only listings made while the panel draws count; the asset manager's own scans do not.
			uint32_t drawListings = 0;
			const auto draw = [&panel, &context, &listings, &drawListings]()
			{
				const uint32_t before = listings.GetListings();
				const Status drawn = panel.Draw(context);
				drawListings += listings.GetListings() - before;
				return drawn;
			};
			REQUIRE(ui.Frame(draw));
			CHECK(drawListings == 1);
			for (int frame = 0; frame < 8; ++frame)
				REQUIRE(ui.Frame(draw));
			CHECK(drawListings == 1); // nothing changed: no per-frame walk of the project
			REQUIRE(fixture.GetAutomation().Call("asset.create", Json{ { "type", "Folder" }, { "path", "Assets/Fresh" } }));
			REQUIRE(ui.Frame(draw));
			CHECK(drawListings == 2); // an executed command may have changed the folders
			for (int frame = 0; frame < 4; ++frame)
				REQUIRE(ui.Frame(draw));
			CHECK(drawListings == 2);
			auto* root = ui.Window("Panel interaction");
			auto* grid = ui.Child(root, ImHashStr("AssetTiles", 0, ContentControlID(root, context.Editor, "ContentColumns")));
			// The new folder is shown from the refreshed listing.
			CHECK(ui.Locate(draw, grid, ImHashStr("##FolderTile", 0, Test::AssetDesignInteraction::AuthoredScope(grid->GetID("AssetGrid"), "Assets/Fresh"))).GetWidth() > 0.0f);
			const uint32_t beforeUndo = drawListings;
			REQUIRE(fixture.GetAutomation().Call("edit.undo", Json::object()));
			REQUIRE(ui.Frame(draw));
			CHECK(drawListings == beforeUndo + 1); // undo is a change too
		}

		TEST_CASE("ContentBrowserPanel: assets renamed or moved outside the editor show their new paths after the rescan")
		{
			Test::AutomationFixture fixture("ContentExternalMove", false);
			auto& editor = fixture.GetEditor();
			REQUIRE(fixture.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Wood.material" } }));
			auto& server = fixture.GetClient().GetServer();
			EditorActions actions(editor, server);
			EditorAutomationControls controls(editor, server);
			ContentBrowserTestViewport viewports;
			ReflectedEditController edits(editor);
			GizmoController gizmos(editor);
			ThumbnailCache thumbnails(editor, {});
			EditorPanelContext context{ editor, server, actions, controls, viewports, edits, thumbnails, gizmos };
			ContentBrowserPanel panel;
			std::string text;
			const auto draw = DrawCapturingText(panel, context, text);
			Test::AssetDesignInteraction ui;
			REQUIRE(ui.Frame(draw));
			const auto asset = editor.GetAssets().Resolve("Assets/Wood.material");
			REQUIRE(asset);
			auto* root = ui.Window("Panel interaction");
			auto* grid = ui.Child(root, ImHashStr("AssetTiles", 0, ContentControlID(root, editor, "ContentColumns")));
			// Hovering the tile shows its name and path in the tooltip.
			const auto hoverAsset = [&ui, &draw, grid, &asset]()
			{
				const ImVec2 center = ui.Locate(draw, grid, ImHashStr("##Asset", 0, ImHashStr(asset->ToString().c_str(), 0, grid->GetID("AssetGrid")))).GetCenter();
				ImGui::GetIO().AddMousePosEvent(center.x, center.y);
				REQUIRE(ui.Frame(draw));
				REQUIRE(ui.Frame(draw));
			};
			hoverAsset();
			CHECK(text.contains("Assets/Wood.material"));
			// The source and its .meta renamed in one go (a file manager, a checkout): the rescan registers the same handle at
			// the new path, so the record count stays the same, no command runs and no cooked bytes change.
			const std::filesystem::path assets = editor.GetProject().GetRoot() / "Assets";
			const size_t records = editor.GetAssets().GetRegistry().GetRecordCount();
			const auto move = [&editor, &assets](std::string_view from, std::string_view to)
			{
				REQUIRE(FileSystem::Move(assets / from, assets / to));
				REQUIRE(FileSystem::Move(assets / (std::string(from) + ".meta"), assets / (std::string(to) + ".meta")));
				REQUIRE(editor.GetAssets().Refresh());
				editor.GetAssets().WaitIdle();
			};
			move("Wood.material", "Oak.material");
			REQUIRE(editor.GetAssets().Resolve("Assets/Oak.material") == asset);
			REQUIRE(editor.GetAssets().GetRegistry().GetRecordCount() == records);
			hoverAsset();
			CHECK(text.contains("Assets/Oak.material"));
			CHECK_FALSE(text.contains("Assets/Wood.material"));
			// Moved into a new folder: the folder appears in the grid of its parent.
			REQUIRE(FileSystem::CreateDirectories(assets / "Moved"));
			move("Oak.material", "Moved/Oak.material");
			REQUIRE(editor.GetAssets().Resolve("Assets/Moved/Oak.material") == asset);
			REQUIRE(ui.Frame(draw));
			CHECK(ui.Locate(draw, grid, ImHashStr("##FolderTile", 0, Test::AssetDesignInteraction::AuthoredScope(grid->GetID("AssetGrid"), "Assets/Moved"))).GetWidth() > 0.0f);
		}

		TEST_CASE("ContentBrowserPanel: a thumbnail failure stays on its tile and never replaces an operation error")
		{
			Test::AutomationFixture fixture("ContentThumbnailError", false);
			auto& editor = fixture.GetEditor();
			REQUIRE(fixture.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Broken.material" } }));
			auto& server = fixture.GetClient().GetServer();
			EditorActions actions(editor, server);
			EditorAutomationControls controls(editor, server);
			ContentBrowserTestViewport viewports;
			ReflectedEditController edits(editor);
			GizmoController gizmos(editor);
			ThumbnailCache thumbnails(editor, [](const ThumbnailRequest&) -> Result<Image>
			{
				return MakeError(ErrorCode::Io, "preview renderer exploded");
			});
			REQUIRE(thumbnails.BindProject(1));
			EditorPanelContext context{ editor, server, actions, controls, viewports, edits, thumbnails, gizmos };
			std::vector<std::filesystem::path> drops;
			context.TakeContentDrops = [&drops]()
			{
				return std::exchange(drops, {});
			};
			ContentBrowserPanel panel;
			std::string text;
			const auto draw = DrawCapturingText(panel, context, text);
			Test::AssetDesignInteraction ui;
			REQUIRE(ui.Frame(draw));
			// The failure is reported once, by the pump, and remembered.
			Status pumped;
			for (int frame = 0; frame < 4 && pumped; ++frame)
			{
				pumped = thumbnails.Pump();
				editor.GetAssets().WaitIdle();
			}
			CHECK_FALSE(pumped);
			REQUIRE(ui.Frame(draw));
			CHECK_FALSE(text.contains("exploded"));
			// A failing operation: importing a file that does not exist.
			drops = { fixture.GetEditorFixture().GetDirectory() / "Missing.material" };
			Test::ExpectLog failedImport(LogLevel::Error, "Content browser:");
			REQUIRE(ui.Frame(draw));
			actions.Pump();
			REQUIRE(ui.Frame(draw));
			for (int frame = 0; frame < 3; ++frame)
				REQUIRE(ui.Frame(draw));
			CHECK(text.contains("Missing.material"));
			CHECK_FALSE(text.contains("exploded"));
			// The failure is on its tile instead.
			auto* root = ui.Window("Panel interaction");
			auto* grid = ui.Child(root, ImHashStr("AssetTiles", 0, ContentControlID(root, editor, "ContentColumns")));
			const auto asset = editor.GetAssets().Resolve("Assets/Broken.material");
			REQUIRE(asset);
			const ImVec2 center = ui.Locate(draw, grid, ImHashStr("##Asset", 0, ImHashStr(asset->ToString().c_str(), 0, grid->GetID("AssetGrid")))).GetCenter();
			ImGui::GetIO().AddMousePosEvent(center.x, center.y);
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			CHECK(text.contains("Preview unavailable: preview renderer exploded"));
		}

		TEST_CASE("ContentBrowserPanel: files dropped while the panel was hidden import only after their destination is confirmed")
		{
			Test::PanelInteractionFixture fixture("ContentHiddenDrop", false);
			auto& context = fixture.GetContext();
			REQUIRE(FileSystem::Exists(context.Editor.GetProject().GetRoot() / "Assets/Audio")); // a folder of every new project
			const auto source = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "Hidden.material";
			const auto text = MaterialToText(MaterialData{}, context.Editor.GetTypeRegistry());
			REQUIRE(text);
			REQUIRE(FileSystem::WriteFileAtomic(source, AsBytes(*text)));
			std::vector<std::filesystem::path> drops;
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
			// Another tab is selected: frames pass without drawing the panel while the files arrive.
			drops = { source };
			for (int frame = 0; frame < 3; ++frame)
				REQUIRE(ui.Frame([]() -> Status
				{
					return {};
				}));
			REQUIRE(ui.Frame(draw));
			CHECK(drops.empty());
			context.Actions.Pump();
			CHECK_FALSE(context.Editor.GetAssets().Resolve("Assets/Hidden.material"));
			CHECK_FALSE(context.Editor.GetAssets().Resolve("Assets/Audio/Hidden.material"));
			auto* prompt = ui.Popup();
			auto* destinations = ui.Child(prompt, prompt->GetID("DropDestinations"));
			ui.Click(draw, destinations, ImHashStr("##Destination", 0, Test::AssetDesignInteraction::AuthoredScope(destinations->ID, "Assets/Audio")));
			ui.Click(draw, prompt, "Import files");
			context.Actions.Pump();
			context.Editor.GetAssets().WaitIdle();
			context.Actions.Pump();
			CHECK(context.Editor.GetAssets().Resolve("Assets/Audio/Hidden.material"));
			CHECK_FALSE(context.Editor.GetAssets().Resolve("Assets/Hidden.material"));
		}

		TEST_CASE("ContentBrowserPanel: files dropped on a read-only project are reported as not imported")
		{
			Test::PanelInteractionFixture fixture("ContentReadOnlyDrop", false);
			auto& context = fixture.GetContext();
			const auto root = context.Editor.GetProject().GetRoot();
			REQUIRE(context.Editor.CloseProject());
			auto loaded = ProjectManager::OpenProject(root, { .ReadOnly = true, .ReadOnlyCacheDirectory = fixture.GetAutomation().GetEditorFixture().GetDirectory() / "ReadOnlyCache" }, context.Editor.GetTypeRegistry());
			REQUIRE(loaded);
			REQUIRE(context.Editor.OpenProject(std::move(*loaded)));
			std::vector<std::filesystem::path> drops{ fixture.GetAutomation().GetEditorFixture().GetDirectory() / "Any.material" };
			context.TakeContentDrops = [&drops]()
			{
				return std::exchange(drops, {});
			};
			ContentBrowserPanel panel;
			std::string text;
			const auto draw = DrawCapturingText(panel, context, text);
			Test::AssetDesignInteraction ui;
			Test::ExpectLog warned(LogLevel::Warn, "read-only");
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			CHECK(drops.empty());
			CHECK(text.contains("read-only, so 1 dropped file(s) were not imported"));
			context.Actions.Pump();
			CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
		}

		TEST_CASE("ContentBrowserPanel: opening a scene over unsaved changes asks to save or discard first")
		{
			Test::PanelInteractionFixture fixture("ContentDirtyScene", true);
			auto& context = fixture.GetContext();
			const auto created = fixture.GetAutomation().Call("asset.create", Json{ { "type", "Scene" }, { "path", "Assets/Level2.scene" } });
			REQUIRE(created);
			const auto level = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(level);
			const auto another = fixture.GetAutomation().Call("asset.create", Json{ { "type", "Scene" }, { "path", "Assets/Level3.scene" } });
			REQUIRE(another);
			const auto next = JsonReader((*another)["asset"]["id"]).ReadUUID();
			REQUIRE(next);
			const auto opened = context.Editor.GetScenePath();
			REQUIRE(opened);
			REQUIRE(fixture.GetAutomation().Call("entity.create", Json{ { "name", "Unsaved" } }));
			REQUIRE(context.Editor.IsSceneDirty());
			ContentBrowserPanel panel;
			Test::AssetDesignInteraction ui;
			const auto draw = [&panel, &context]()
			{
				return panel.Draw(context);
			};
			REQUIRE(ui.Frame(draw));
			auto* root = ui.Window("Panel interaction");
			auto* grid = ui.Child(root, ImHashStr("AssetTiles", 0, ContentControlID(root, context.Editor, "ContentColumns")));
			DoubleClickContentItem(ui, draw, grid, ImHashStr("##Asset", 0, ImHashStr(level->ToString().c_str(), 0, grid->GetID("AssetGrid"))));
			context.Actions.Pump();
			CHECK(context.Editor.GetScenePath() == opened); // nothing was submitted without the choice
			CHECK(context.Editor.IsSceneDirty());
			ui.Click(draw, ui.Popup(), "Discard and open");
			context.Actions.Pump();
			REQUIRE(context.Editor.GetScenePath());
			CHECK(context.Editor.GetScenePath()->GetPath() == "Assets/Level2.scene");
			CHECK_FALSE(context.Editor.IsSceneDirty());
			// Without unsaved changes another scene opens at once.
			REQUIRE(ui.Frame(draw));
			DoubleClickContentItem(ui, draw, grid, ImHashStr("##Asset", 0, ImHashStr(next->ToString().c_str(), 0, grid->GetID("AssetGrid"))));
			context.Actions.Pump();
			REQUIRE(context.Editor.GetScenePath());
			CHECK(context.Editor.GetScenePath()->GetPath() == "Assets/Level3.scene");
		}

		TEST_CASE("ContentBrowserPanel: authored folder labels retain distinct browsing and undoable move targets")
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

		TEST_CASE("ContentBrowserPanel: compact bottom docks show a complete first row")
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

		TEST_CASE("ContentBrowserPanel: browsing an import queues one undoable human action at the safe point")
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

		TEST_CASE("ContentBrowserPanel: searching by name or type preserves clickable asset selection")
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

		TEST_CASE("ContentBrowserPanel: read only projects consume drops without queuing writes")
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
