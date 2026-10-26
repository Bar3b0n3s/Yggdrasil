#include "EditorPCH.h"
#include "Editor/EditorLayer.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Panels/AutomationPanel.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/DiagnosticsPanel.h"
#include "Editor/Panels/GameViewportPanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/NativePanel.h"
#include "Editor/Panels/ProjectSettingsPanel.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#include "Editor/Panels/SceneViewportPanel.h"
#include "Editor/Panels/StatsPanel.h"
#include "Editor/Panels/UndoHistoryPanel.h"
#include "Editor/ProjectLauncher.h"
#include "Editor/Ui/EditorStyle.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	static bool WorkspaceSearchMatches(std::string_view text, std::string_view search)
	{
		return std::search(text.begin(), text.end(), search.begin(), search.end(), [](unsigned char left, unsigned char right)
		{
			return std::tolower(left) == std::tolower(right);
		}) != text.end()
			|| search.empty();
	}

	static bool HasWorkspaceLayout()
	{
		for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
		{
			const auto panel = static_cast<EditorPanel>(index);
			// ImGui 1.92.6+ hashes only the suffix after ###, preserving the original panel and tab IDs.
			if (ImGui::FindWindowSettingsByID(ImHashStr(Utils::EditorWindowTitle(panel))) != nullptr)
				return true;
		}
		return false;
	}

	namespace Utils {

		// The open-panel set, persisted in imgui.ini next to the dock layout (Dear ImGui never saves a window's open flag).
		// Callbacks run inside Dear ImGui's settings load and save; the layer applies a loaded set before its next frame.
		struct WorkspacePanelSettings
		{
			const EditorUiState* Ui = nullptr;                // borrowed for WriteAll while the handler is registered
			std::optional<std::vector<EditorPanel>> Loaded{}; // from the last loaded ini; nullopt: the ini holds no set
			bool Pending = false;                             // an ini was cleared or loaded since the set was applied
		};

		constexpr const char* WorkspacePanelSettingsType = "EnginePanels";

		static void ClearWorkspacePanels(ImGuiContext* /*context*/, ImGuiSettingsHandler* handler)
		{
			auto& settings = *static_cast<WorkspacePanelSettings*>(handler->UserData);
			settings.Loaded.reset();
			settings.Pending = true;
		}

		static void* OpenWorkspacePanels(ImGuiContext* /*context*/, ImGuiSettingsHandler* handler, const char* name)
		{
			return std::string_view(name) == "Open" ? handler->UserData : nullptr;
		}

		static void ReadWorkspacePanels(ImGuiContext* /*context*/, ImGuiSettingsHandler* /*handler*/, void* entry, const char* line)
		{
			constexpr std::string_view Key = "Panels=";
			const std::string_view text(line);
			if (!text.starts_with(Key))
				return;
			auto& settings = *static_cast<WorkspacePanelSettings*>(entry);
			std::vector<EditorPanel> panels;
			std::string_view names = text.substr(Key.size());
			while (!names.empty())
			{
				const size_t comma = names.find(',');
				const std::string_view name = names.substr(0, comma);
				names = comma == std::string_view::npos ? std::string_view{} : names.substr(comma + 1);
				// Unknown names (a newer editor's panels) are ignored rather than failing the whole layout.
				for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
				{
					if (EditorPanelToString(static_cast<EditorPanel>(index)) == name)
						panels.push_back(static_cast<EditorPanel>(index));
				}
			}
			settings.Loaded = std::move(panels);
		}

		static void ApplyWorkspacePanels(ImGuiContext* /*context*/, ImGuiSettingsHandler* handler)
		{
			static_cast<WorkspacePanelSettings*>(handler->UserData)->Pending = true;
		}

		static void WriteWorkspacePanels(ImGuiContext* /*context*/, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer)
		{
			const auto& settings = *static_cast<const WorkspacePanelSettings*>(handler->UserData);
			buffer->appendf("[%s][Open]\nPanels=", handler->TypeName);
			bool first = true;
			for (const EditorPanel panel : settings.Ui->GetOpenPanels())
			{
				// The launcher's visibility follows the project state, never the saved layout.
				if (panel == EditorPanel::ProjectLauncher)
					continue;
				const std::string_view name = EditorPanelToString(panel);
				buffer->appendf("%s%.*s", first ? "" : ",", static_cast<int>(name.size()), name.data());
				first = false;
			}
			buffer->append("\n\n");
		}

		static std::vector<EditorPanel> SavedWorkspacePanels(const EditorUiState& ui)
		{
			std::vector<EditorPanel> panels(ui.GetOpenPanels().begin(), ui.GetOpenPanels().end());
			std::erase(panels, EditorPanel::ProjectLauncher);
			return panels;
		}

	}

	static void WorkspaceIdentity(std::string_view text, float width)
	{
		const ImVec2 start = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##Identity", ImVec2(std::max(width, 1.0f), ImGui::GetFrameHeight()));
		const ImVec2 end = ImGui::GetItemRectMax();
		const ImVec2 textStart(start.x, start.y + ImGui::GetStyle().FramePadding.y);
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), textStart, end, end.x,
			text.data(), text.data() + text.size(), nullptr);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), text.data());
	}

	struct EditorLayer::State
	{
		EditorPanelContext& Context; // injected service bundle outlives the layer
		SceneHierarchyPanel Hierarchy{};
		InspectorPanel Inspector{};
		SceneViewportPanel SceneViewport{};
		GameViewportPanel GameViewport{};
		ContentBrowserPanel ContentBrowser{};
		ConsolePanel Console{};
		DiagnosticsPanel Diagnostics{};
		ProjectSettingsPanel Settings{};
		StatsPanel Stats{};
		AutomationPanel Automation{};
		UndoHistoryPanel History{};
		NativePanel Native{};
		ProjectLauncher Launcher{};
		std::vector<uint64_t> Tickets{};
		std::string ErrorMessage{};
		std::string NewSceneName = "Untitled";
		std::string NewSceneFolder = "Assets/Scenes";
		std::string SceneSearch{};
		std::string SelectedScene{};
		std::string SceneDialogError{};
		std::filesystem::path SceneDialogProject{};
		std::vector<VfsPath> Scenes{};
		std::vector<std::string> SceneFolders{};
		std::optional<uint64_t> SceneTicket{};
		bool SceneDialogComplete = false;
		bool ShowNewScene = false;
		bool CreateScene = true;
		bool DiscardForNewScene = false;
		bool SaveForNewScene = false;
		bool KeepOpen = false;                     // the scene dialog's "Open in a new tab", kept between dialogs
		OpenSceneId ActiveScene = NoOpenScene;     // the active scene the panels last followed (FollowActiveScene)
		std::optional<OpenSceneId> CloseRequest{}; // a dirty tab's close button, which opens the close prompt
		OpenSceneId ClosingScene = NoOpenScene;    // the open scene the close prompt asks about
		bool LayoutInitialized = false;
		std::string LayoutIniFile{};
		bool ResetLayout = false;
		bool SelectDefaultTabs = false;
		bool CancelGesture = false;
		bool FrameRequested = false;
		bool HadProject = false;
		std::optional<EditorPanel> FocusRequest{}; // RevealPanel: the window that takes focus on the next frame
		uint64_t RequestedAfter = 0;
		Utils::WorkspacePanelSettings PanelSettings{};
		ImGuiContext* SettingsContext = nullptr;  // the Dear ImGui context holding the panel settings handler
		std::vector<EditorPanel> WrittenPanels{}; // the persisted set the ini last saw; a change marks the ini dirty

		// Registers the panel settings handler in the current Dear ImGui context, once per context. Called before the first
		// frame when a context exists (so the first ini load restores the panels) and again from each UI frame.
		void RegisterPanelSettings()
		{
			ImGuiContext* current = ImGui::GetCurrentContext();
			if (current == nullptr || current == SettingsContext)
				return;
			// The newest layer owns the handler; an older layer's destructor then leaves it alone.
			ImGui::RemoveSettingsHandler(Utils::WorkspacePanelSettingsType);
			ImGuiSettingsHandler handler;
			handler.TypeName = Utils::WorkspacePanelSettingsType;
			handler.TypeHash = ImHashStr(Utils::WorkspacePanelSettingsType);
			handler.ClearAllFn = &Utils::ClearWorkspacePanels;
			handler.ReadInitFn = &Utils::ClearWorkspacePanels;
			handler.ReadOpenFn = &Utils::OpenWorkspacePanels;
			handler.ReadLineFn = &Utils::ReadWorkspacePanels;
			handler.ApplyAllFn = &Utils::ApplyWorkspacePanels;
			handler.WriteAllFn = &Utils::WriteWorkspacePanels;
			handler.UserData = &PanelSettings;
			ImGui::AddSettingsHandler(&handler);
			SettingsContext = current;
			WrittenPanels = Utils::SavedWorkspacePanels(Context.Editor.GetUiState());
		}

		void RefreshScenes()
		{
			Scenes.clear();
			SceneFolders = { "Assets/Scenes", "" };
			SceneDialogError.clear();
			const auto root = VfsPath::Parse("project://");
			ENGINE_VERIFY(root.has_value(), "Constant project root path");
			const auto entries = Context.Editor.GetVfs().List(*root, true);
			if (!entries)
			{
				SceneDialogError = entries.error().GetMessageText();
				return;
			}
			for (const auto& entry : *entries)
			{
				if (entry.Info.IsDirectory && entry.Path.GetPath() != "Assets/Scenes")
					SceneFolders.emplace_back(entry.Path.GetPath());
				else if (!entry.Info.IsDirectory && entry.Path.GetExtension() == ".scene")
					Scenes.push_back(entry.Path);
			}
			std::ranges::sort(Scenes, [](const VfsPath& left, const VfsPath& right)
			{
				return left.GetStem() != right.GetStem() ? left.GetStem() < right.GetStem() : left < right;
			});
		}

		void ReportFailure(const Error& error)
		{
			const std::string message = error.GetMessageText();
			if (ErrorMessage != message)
				ENGINE_ERROR("Editor action: {}", error.ToString());
			ErrorMessage = message;
		}

		// Submits a user action; `cancelGesture` also ends the inspector's gesture at the next safe point.
		void Queue(std::string_view method, const Json& params, bool cancelGesture = false)
		{
			auto submitted = Context.Actions.Submit(method, params);
			if (submitted)
			{
				Tickets.push_back(*submitted);
				CancelGesture |= cancelGesture;
			}
			else
				ReportFailure(submitted.error());
		}

		// The panels show the active scene (Docs/Decisions/0031-m20-contract.md decision 20). When another tab becomes active,
		// or the active scene is replaced, the inspector's gesture belonged to the scene it left, so it ends unwritten.
		void FollowActiveScene()
		{
			const OpenSceneId active = Context.Editor.GetActiveSceneId();
			if (active == ActiveScene)
				return;
			Inspector.CancelEdit(Context);
			ActiveScene = active;
		}

		// The scene tabs (decision 20): one per open scene in tab order, each showing its scene's name with " *" while it has
		// unsaved changes, the active one highlighted. A click on another tab submits scene.activate, a close button
		// scene.close, both naming the tab's scene by its open-scene id (the scene target); a dirty tab's close button opens
		// the close prompt instead (DrawClosePrompt). Drawn as items of their own rather than a Dear ImGui tab bar, whose new
		// tabs first show a frame after they are submitted and whose labels end at "##": a frame drawn right after scene.new
		// shows the new tab, and every scene name shows in full.
		void DrawSceneTabs(bool interactive)
		{
			const EditorContext& editor = Context.Editor;
			struct SceneTab
			{
				OpenSceneInfo Scene{};
				std::string Label{};
				float Width = 0.0f;
			};
			std::vector<SceneTab> tabs;
			for (const OpenSceneId id : editor.GetOpenScenes())
			{
				Result<OpenSceneInfo> scene = editor.DescribeOpenScene(id);
				ENGINE_VERIFY(scene.has_value(), "GetOpenScenes lists open scenes only");
				std::string label = scene->Name + (scene->Dirty ? " *" : "");
				tabs.push_back({ std::move(*scene), std::move(label) });
			}
			if (tabs.empty())
			{
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("No scene open");
				return;
			}
			const ImGuiStyle& style = ImGui::GetStyle();
			const float closeSize = ImGui::GetFontSize();
			const float spacing = style.ItemInnerSpacing.x;
			const float chrome = style.FramePadding.x * 2.0f + spacing + closeSize;
			float total = 0.0f;
			for (SceneTab& tab : tabs)
			{
				tab.Width = chrome + ImGui::CalcTextSize(tab.Label.data(), tab.Label.data() + tab.Label.size()).x;
				total += tab.Width;
			}
			// Tabs that do not fit share the width; their names end in an ellipsis and show in full in the tooltip.
			const float available = ImGui::GetContentRegionAvail().x - spacing * static_cast<float>(tabs.size() - 1);
			if (total > available)
			{
				const float share = std::max(chrome + ImGui::CalcTextSize("...").x, available / static_cast<float>(tabs.size()));
				for (SceneTab& tab : tabs)
					tab.Width = std::min(tab.Width, share);
			}
			ImGui::BeginDisabled(!interactive);
			for (size_t index = 0; index < tabs.size(); ++index)
			{
				const SceneTab& tab = tabs[index];
				const OpenSceneId id = tab.Scene.Id;
				if (index != 0)
					ImGui::SameLine(0.0f, spacing);
				ImGui::SetNextItemAllowOverlap();
				// Keyboard navigation reaches the tabs like any button, and shows its focus on them.
				const bool clicked = ImGui::InvisibleButton(std::format("##SceneTab{}", id).c_str(), ImVec2(tab.Width, ImGui::GetFrameHeight()),
					ImGuiButtonFlags_EnableNav);
				const bool hovered = ImGui::IsItemHovered();
				const ImRect bounds(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
				ImDrawList& draw = *ImGui::GetWindowDrawList();
				ImGuiCol color = tab.Scene.Active ? ImGuiCol_TabSelected : ImGuiCol_Tab;
				if (hovered || ImGui::IsItemActive())
					color = ImGuiCol_TabHovered;
				ImGui::TabItemBackground(&draw, bounds, ImGuiTabItemFlags_None, ImGui::GetColorU32(color));
				if (tab.Scene.Active && style.TabBarOverlineSize > 0.0f)
					draw.AddLine(ImVec2(bounds.Min.x, bounds.Min.y + 1.0f), ImVec2(bounds.Max.x, bounds.Min.y + 1.0f),
						ImGui::GetColorU32(ImGuiCol_TabSelectedOverline), style.TabBarOverlineSize);
				ImGui::RenderNavCursor(bounds, ImGui::GetItemID());
				const ImVec2 closePosition(bounds.Max.x - style.FramePadding.x - closeSize, bounds.Min.y + style.FramePadding.y);
				const float textEnd = closePosition.x - spacing;
				ImGui::RenderTextEllipsis(&draw, ImVec2(bounds.Min.x + style.FramePadding.x, bounds.Min.y + style.FramePadding.y),
					ImVec2(textEnd, bounds.Max.y), textEnd, tab.Label.data(), tab.Label.data() + tab.Label.size(), nullptr);
				if (hovered)
				{
					const std::string where = tab.Scene.Path ? tab.Scene.Path->ToString() : "Not saved to a file yet";
					ImGui::SetTooltip("%s\n%s%s", tab.Scene.Name.c_str(), where.c_str(), tab.Scene.ChangedOnDisk ? "\nChanged on disk" : "");
				}
				const bool closed = ImGui::CloseButton(ImGui::GetID(std::format("##CloseSceneTab{}", id).c_str()), closePosition);
				if (closed && tab.Scene.Dirty)
					CloseRequest = id;
				else if (closed)
					Queue("scene.close", Json{ { "scene", id } }, true);
				else if (clicked && !tab.Scene.Active)
					Queue("scene.activate", Json{ { "scene", id } }, true);
			}
			ImGui::EndDisabled();
		}

		// The prompt of a dirty tab's close button: save and close (the scene's file; a scene never saved can only be
		// discarded), discard its changes and close, or cancel. Closes itself when the scene is no longer open.
		void DrawClosePrompt(const ImGuiViewport& viewport)
		{
			const EditorContext& editor = Context.Editor;
			if (CloseRequest)
			{
				ClosingScene = *std::exchange(CloseRequest, std::nullopt);
				ImGui::OpenPopup("Close scene");
			}
			ImGui::SetNextWindowPos(viewport.GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			if (!ImGui::BeginPopupModal("Close scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
				return;
			const Result<OpenSceneInfo> scene = editor.DescribeOpenScene(ClosingScene);
			if (!scene)
			{
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}
			const Json target{ { "scene", ClosingScene } };
			const std::string question = scene->Dirty ? std::format("{} has unsaved changes.", scene->Name) : std::format("Close {}?", scene->Name);
			ImGui::TextUnformatted(question.c_str());
			if (scene->Dirty && !scene->Path)
				ImGui::TextWrapped("It was never saved to a file, so its changes can only be discarded.");
			ImGui::Separator();
			if (scene->Dirty)
			{
				ImGui::BeginDisabled(editor.IsReadOnly() || !scene->Path);
				if (ImGui::Button("Save and close"))
				{
					Json params = target;
					params["save"] = true;
					Queue("scene.close", params, true);
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Discard changes"))
				{
					Json params = target;
					params["discardChanges"] = true;
					Queue("scene.close", params, true);
					ImGui::CloseCurrentPopup();
				}
			}
			else if (ImGui::Button("Close scene"))
			{
				Queue("scene.close", target, true);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	};

	EditorLayer::EditorLayer(EditorPanelContext& context)
		: m_State(CreateScope<State>(context))
	{
		m_State->HadProject = context.Editor.HasProject();
		// The panels show the scene active now; only a later change of the active scene ends a gesture (FollowActiveScene).
		m_State->ActiveScene = context.Editor.GetActiveSceneId();
		if (context.Editor.GetUiState().GetOpenPanels().empty())
			context.Editor.GetUiState().ResetLayout(context.Editor.HasProject());
		m_State->PanelSettings.Ui = &context.Editor.GetUiState();
		m_State->RegisterPanelSettings();
	}

	EditorLayer::~EditorLayer()
	{
		ImGuiContext* current = ImGui::GetCurrentContext();
		if (current == nullptr || current != m_State->SettingsContext)
			return;
		const ImGuiSettingsHandler* handler = ImGui::FindSettingsHandler(Utils::WorkspacePanelSettingsType);
		if (handler == nullptr || handler->UserData != &m_State->PanelSettings)
			return;
		// Write the panels now, while this layer can still describe them. A save after the handler is gone drops them, which
		// the owner prevents by stopping the ini first (EditorApp::OnShutdown).
		if (current->SettingsLoaded && ImGui::GetIO().IniFilename != nullptr)
			ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
		ImGui::RemoveSettingsHandler(Utils::WorkspacePanelSettingsType);
	}

	Status EditorLayer::OnSafePoint(double nowSeconds)
	{
		if (!std::isfinite(nowSeconds))
			return MakeError(ErrorCode::InvalidArgument, "editor safe-point time must be finite");
		EditorPanelContext& context = m_State->Context;
		if (m_State->CancelGesture)
		{
			m_State->Inspector.CancelEdit(context);
			m_State->CancelGesture = false;
		}
		// Before a queued edit commits (an agent may have activated another scene since the last frame) and again after the
		// queued actions ran (the scene tabs' own activations), so the panels draw the active scene.
		m_State->FollowActiveScene();
		ENGINE_TRY(m_State->Inspector.ApplyQueuedEdit(context));
		context.Actions.Pump();
		m_State->FollowActiveScene();
		// Destroyed entities and replaced runtime scenes leave the selection before panels draw it.
		context.Editor.PruneSelection();
		for (auto ticket = m_State->Tickets.begin(); ticket != m_State->Tickets.end();)
		{
			auto result = context.Actions.TakeResult(*ticket);
			if (!result || result->has_value())
			{
				if (m_State->SceneTicket == *ticket)
				{
					m_State->SceneDialogComplete = result && (**result).has_value();
					if (!result)
						m_State->SceneDialogError = result.error().GetMessageText();
					else if (!**result)
						m_State->SceneDialogError = (**result).error().GetMessageText();
					m_State->SceneTicket.reset();
				}
				if (!result)
					m_State->ReportFailure(result.error());
				else if (!**result)
					m_State->ReportFailure((**result).error());
				ticket = m_State->Tickets.erase(ticket);
			}
			else
				++ticket;
		}
		const bool hasProject = context.Editor.HasProject();
		if (hasProject != m_State->HadProject)
		{
			m_State->Inspector.CancelEdit(context);
			ENGINE_TRY(context.Editor.GetUiState().SetPanelOpen(EditorPanel::ProjectLauncher, !hasProject));
			m_State->HadProject = hasProject;
		}
		if (context.Thumbnails.GetProjectGeneration() != 0)
		{
			const Status thumbnails = context.Thumbnails.Pump();
			// Pump drops work for deleted assets and follows version changes itself; Conflict only reports a binding or
			// cache root that is no longer current, which the host's project-close reset replaces.
			if (!thumbnails && thumbnails.error().GetCode() != ErrorCode::Conflict)
				return thumbnails;
		}
		return {};
	}

	Status EditorLayer::OnImGuiRender()
	{
		ImGuizmo::BeginFrame();
		EditorPanelContext& context = m_State->Context;
		EditorContext& editor = context.Editor;
		EditorUiState& ui = editor.GetUiState();
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		const PlaySession* session = editor.GetPlay().GetSession();
		const bool agentTime = session != nullptr && session->IsLockstep() && session->GetLockstepOwner() != NoClient;
		const float scale = Utils::EditorUiScale();
		m_State->RegisterPanelSettings();
		if (std::exchange(m_State->PanelSettings.Pending, false))
		{
			// A freshly loaded imgui.ini decides which panels are open; one without a saved set uses the defaults.
			if (m_State->PanelSettings.Loaded)
				ui.SetOpenPanels(*m_State->PanelSettings.Loaded, editor.HasProject());
			else
				ui.ResetLayout(editor.HasProject());
			m_State->WrittenPanels = Utils::SavedWorkspacePanels(ui);
		}
		// A test run owns the editor: its lease rejects these actions, so their controls are disabled too.
		const bool testRun = editor.GetPlay().IsTestRunActive();
		const bool canSave = editor.HasScene() && !editor.IsReadOnly() && !testRun;
		const bool canUndo = !editor.IsReadOnly() && !testRun && editor.GetHistory().CanUndo();
		const bool canRedo = !editor.IsReadOnly() && !testRun && editor.GetHistory().CanRedo();
		auto queue = [this](std::string_view method, const Json& params, bool cancelGesture = false)
		{
			m_State->Queue(method, params, cancelGesture);
		};
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New scene", nullptr, false, editor.HasProject() && !editor.IsReadOnly() && session == nullptr))
				{
					m_State->ShowNewScene = true;
					m_State->CreateScene = true;
				}
				if (ImGui::MenuItem("Open scene", nullptr, false, editor.HasProject() && session == nullptr))
				{
					m_State->ShowNewScene = true;
					m_State->CreateScene = false;
				}
				if (ImGui::MenuItem("Save scene", "Ctrl+S", false, canSave))
					queue("scene.save", Json::object(), true);
				if (ImGui::MenuItem("Project launcher", nullptr, false, !editor.HasProject()))
				{
					const Status opened = ui.SetPanelOpen(EditorPanel::ProjectLauncher, true);
					ENGINE_VERIFY(opened.has_value(), "Known panel");
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo))
					queue("edit.undo", Json::object(), true);
				if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo))
					queue("edit.redo", Json::object(), true);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Window"))
			{
				for (uint8_t index = 0; index <= static_cast<uint8_t>(EditorPanel::ProjectLauncher); ++index)
				{
					const EditorPanel panel = static_cast<EditorPanel>(index);
					const auto panels = ui.GetOpenPanels();
					bool open = std::find(panels.begin(), panels.end(), panel) != panels.end();
					const std::string title(Utils::EditorWindowTitle(panel));
					const std::string label = title.substr(0, title.find("###"));
					if (ImGui::MenuItem(label.c_str(), nullptr, &open))
					{
						const Status changed = ui.SetPanelOpen(panel, open);
						ENGINE_VERIFY(changed.has_value(), "Known panel");
					}
				}
				if (ImGui::MenuItem("Reset layout"))
				{
					ui.ResetLayout(editor.HasProject());
					m_State->ResetLayout = true;
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}

		const ImGuiWindowFlags barFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
			| ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus;
		const float toolbarPadding = ImGui::GetStyle().FramePadding.y + 2.0f * scale;
		const bool stackedToolbar = viewport->Size.x < 880.0f * scale;
		const float toolbarHeight = ImGui::GetFrameHeight() * (stackedToolbar ? 2.0f : 1.0f)
			+ toolbarPadding * 2.0f + (stackedToolbar ? ImGui::GetStyle().ItemSpacing.y : 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, toolbarPadding));
		if (ImGui::BeginViewportSideBar("##EditorToolbar", viewport, ImGuiDir_Up, toolbarHeight, barFlags))
		{
			const std::string projectName = editor.HasProject() ? editor.GetProject().GetSettings().Name : "Welcome";
			const std::string sceneName = editor.HasScene() ? editor.GetScene().GetName() : "No scene open";
			const std::string identity = projectName + "  /  " + sceneName + (editor.IsSceneDirty() ? " *" : "");
			float playbackWidth = 0.0f;
			for (const char* label : { "Play", "Simulate", "Resume", "Step", "Stop" })
				playbackWidth += ImGui::CalcTextSize(label).x + 2.0f * ImGui::GetStyle().FramePadding.x + ImGui::GetStyle().ItemSpacing.x;
			const float identityWidth = stackedToolbar ? ImGui::GetContentRegionAvail().x
													   : std::max(1.0f, ImGui::GetContentRegionAvail().x - playbackWidth - 110.0f * scale);
			WorkspaceIdentity(identity, identityWidth);
			if (ImGui::IsItemHovered() && editor.GetScenePath())
				ImGui::SetTooltip("%s\n%s", identity.c_str(), editor.GetScenePath()->ToString().c_str());
			if (!stackedToolbar)
				ImGui::SameLine();
			ImGui::BeginDisabled(!editor.HasScene() || agentTime || testRun);
			ImGui::BeginDisabled(session != nullptr);
			if (Utils::EditorToolbarButton("Play", "Run the scene with scripts and audio", session && session->GetMode() == PlayMode::Play))
				queue("play.start", Json{ { "mode", "Play" } }, true);
			ImGui::SameLine();
			if (Utils::EditorToolbarButton("Simulate", "Preview physics in the Scene view", session && session->GetMode() == PlayMode::Simulate))
				queue("play.start", Json{ { "mode", "Simulate" } }, true);
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(session == nullptr);
			const bool paused = session != nullptr && session->IsPaused();
			if (Utils::EditorToolbarButton(paused ? "Resume###PauseResume" : "Pause###PauseResume", paused ? "Continue the preview" : "Pause the preview", paused))
				queue(paused ? "play.resume" : "play.pause", Json::object());
			ImGui::SameLine();
			ImGui::BeginDisabled(!paused);
			if (Utils::EditorToolbarButton("Step", "Advance the paused scene by one simulation tick"))
				queue("play.step", Json{ { "ticks", 1 } }, true);
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (Utils::EditorToolbarButton("Stop", "Stop the preview and return to the edit scene"))
				queue("play.stop", Json::object(), true);
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", session == nullptr ? "Editing" : paused ? "Paused"
					: session->GetMode() == PlayMode::Play                    ? "Playing"
																			  : "Simulating");
		}
		ImGui::End();
		ImGui::PopStyleVar();
		// The scene tabs, under the toolbar and above the dock space, so above the scene viewport and every panel that follows
		// the active scene. A test run's lease refuses activating and closing scenes, so the tabs wait for it.
		const float tabsPadding = 3.0f * scale;
		const float tabsHeight = editor.HasProject() ? ImGui::GetFrameHeight() + tabsPadding * 2.0f : 0.0f;
		if (editor.HasProject())
		{
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, tabsPadding));
			if (ImGui::BeginViewportSideBar("##EditorSceneTabs", viewport, ImGuiDir_Up, tabsHeight, barFlags))
				m_State->DrawSceneTabs(!testRun);
			ImGui::End();
			ImGui::PopStyleVar();
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * scale, ImGui::GetStyle().FramePadding.y));
		if (ImGui::BeginViewportSideBar("##EditorStatus", viewport, ImGuiDir_Down, ImGui::GetFrameHeight(), barFlags))
		{
			ImGui::TextDisabled("%s", agentTime ? "Agent controls time" : editor.IsReadOnly() ? "Read-only project"
					: !editor.HasProject()                                                    ? "Open a project to begin"
					: !editor.HasScene()                                                      ? "Open or create a scene"
					: session != nullptr                                                      ? "Preview changes end on Stop"
					: editor.IsSceneDirty()                                                   ? "Unsaved changes"
																							  : "All changes saved");
			if (editor.HasScene())
			{
				ImGui::SameLine(0.0f, 24.0f * scale);
				ImGui::TextDisabled("%zu entities", session != nullptr ? session->GetScene().GetEntityCount() : editor.GetScene().GetEntityCount());
				ImGui::SameLine(0.0f, 16.0f * scale);
				ImGui::TextDisabled("%zu selected", editor.GetSelection().size());
			}
			if (editor.IsReadOnly() && agentTime)
			{
				ImGui::SameLine(0.0f, 24.0f * scale);
				ImGui::TextDisabled("Read-only project");
			}
		}
		ImGui::End();
		ImGui::PopStyleVar();

		bool firstLayout = false;
		const std::string_view iniFile = ImGui::GetIO().IniFilename != nullptr ? ImGui::GetIO().IniFilename : "";
		if (!m_State->LayoutInitialized || m_State->LayoutIniFile != iniFile)
		{
			firstLayout = !HasWorkspaceLayout();
			m_State->LayoutInitialized = true;
			m_State->LayoutIniFile = iniFile;
		}
		// Use the same host and DockSpace IDs as DockSpaceOverViewport, with this frame's reserved bars.
		const ImRect work = static_cast<ImGuiViewportP*>(viewport)->GetBuildWorkRect();
		const std::string hostName = std::format("WindowOverViewport_{:08X}", viewport->ID);
		const ImGuiID dock = ImHashStr("DockSpace", 0, ImHashStr(hostName.c_str()));
		if (firstLayout || m_State->ResetLayout)
		{
			ImGui::DockBuilderRemoveNode(dock);
			ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodePos(dock, work.Min);
			ImGui::DockBuilderSetNodeSize(dock, work.GetSize());
			ImGuiID center = dock;
			const float leftWidth = std::min(230.0f * scale, work.GetWidth() * 0.22f);
			const float rightWidth = std::min(360.0f * scale, work.GetWidth() * 0.30f);
			const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftWidth / work.GetWidth(), nullptr, &center);
			const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightWidth / (work.GetWidth() - leftWidth), nullptr, &center);
			// The scene tabs' row comes out of the scene viewport's column: the bottom panels keep the share they had without it.
			const float bottomHeight = std::min(270.0f * scale, (work.GetHeight() + tabsHeight) * 0.32f);
			const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, bottomHeight / work.GetHeight(), nullptr, &center);
			ImGui::DockBuilderDockWindow(Utils::EditorWindowTitle(EditorPanel::SceneHierarchy), left);
			ImGui::DockBuilderDockWindow(Utils::EditorWindowTitle(EditorPanel::Inspector), right);
			ImGui::DockBuilderDockWindow(Utils::EditorWindowTitle(EditorPanel::SceneViewport), center);
			ImGui::DockBuilderDockWindow(Utils::EditorWindowTitle(EditorPanel::GameViewport), center);
			for (const EditorPanel panel : { EditorPanel::ContentBrowser, EditorPanel::Console, EditorPanel::Diagnostics,
					 EditorPanel::Stats, EditorPanel::Automation, EditorPanel::UndoHistory, EditorPanel::Native })
				ImGui::DockBuilderDockWindow(Utils::EditorWindowTitle(panel), bottom);
			ImGui::DockBuilderDockWindow(Utils::EditorWindowTitle(EditorPanel::ProjectSettings), right);
			ImGui::DockBuilderFinish(dock);
			m_State->SelectDefaultTabs = true;
			m_State->ResetLayout = false;
		}
		ImGui::SetNextWindowPos(work.Min);
		ImGui::SetNextWindowSize(work.GetSize());
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::Begin(hostName.c_str(), nullptr, barFlags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus);
		ImGui::PopStyleVar(3);
		ImGui::DockSpace(dock);
		ImGui::End();

		const char* sceneDialog = m_State->CreateScene ? "New scene" : "Open scene";
		if (m_State->ShowNewScene && editor.HasProject())
		{
			ImGui::OpenPopup(sceneDialog);
			m_State->ShowNewScene = false;
			m_State->DiscardForNewScene = false;
			m_State->SaveForNewScene = false;
			m_State->SceneDialogComplete = false;
			m_State->SceneDialogProject = editor.GetProject().GetProjectFile();
			m_State->SelectedScene.clear();
			m_State->SceneSearch.clear();
			m_State->RefreshScenes();
		}
		ImGui::SetNextWindowSize(ImVec2(std::min(570.0f * scale, viewport->Size.x - 32.0f * scale), 0.0f), ImGuiCond_Appearing);
		ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (ImGui::BeginPopupModal(sceneDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			const bool sameProject = editor.HasProject() && editor.GetProject().GetProjectFile() == m_State->SceneDialogProject;
			if (!sameProject || m_State->SceneDialogComplete)
				ImGui::CloseCurrentPopup();
			ImGui::BeginDisabled(!sameProject || m_State->SceneTicket.has_value() || session != nullptr);
			std::string scenePath = m_State->SelectedScene;
			bool valid = !scenePath.empty();
			if (m_State->CreateScene)
			{
				Utils::EditorSectionHeading("Start with an empty scene");
				ImGui::TextWrapped("Give your scene a name. Add entities from the Hierarchy after creating it.");
				ImGui::Spacing();
				ImGui::TextUnformatted("Scene name");
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::IsWindowAppearing())
					ImGui::SetKeyboardFocusHere();
				ImGui::InputTextWithHint("##SceneName", "My scene", &m_State->NewSceneName);
				ImGui::TextUnformatted("Save in");
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::BeginCombo("##SceneFolder", "", ImGuiComboFlags_CustomPreview))
				{
					for (const auto& folder : m_State->SceneFolders)
					{
						ImGui::PushOverrideID(ImHashData(folder.data(), folder.size(), ImGui::GetCurrentWindow()->IDStack.back()));
						const ImVec2 row = ImGui::GetCursorScreenPos();
						if (ImGui::Selectable("##Folder", folder == m_State->NewSceneFolder))
							m_State->NewSceneFolder = folder;
						const ImVec2 end = ImGui::GetItemRectMax();
						const std::string_view label = folder.empty() ? "Project root" : std::string_view(folder);
						ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), row, end, end.x, label.data(), label.data() + label.size(), nullptr);
						ImGui::PopID();
					}
					ImGui::EndCombo();
				}
				if (ImGui::BeginComboPreview())
				{
					ImGui::TextUnformatted(m_State->NewSceneFolder.empty() ? "Project root" : m_State->NewSceneFolder.c_str());
					ImGui::EndComboPreview();
				}
				const std::string fileName = m_State->NewSceneName.ends_with(".scene") ? m_State->NewSceneName : m_State->NewSceneName + ".scene";
				scenePath = m_State->NewSceneFolder.empty() ? fileName : m_State->NewSceneFolder + "/" + fileName;
				const auto path = VfsPath::Create("project", scenePath);
				valid = !m_State->NewSceneName.empty() && m_State->NewSceneName.find_first_of("/\\") == std::string::npos
					&& VfsPath::ValidateRelativePath(m_State->NewSceneName).has_value() && path.has_value() && !path->GetStem().empty();
				if (!valid && !m_State->NewSceneName.empty())
					ImGui::TextWrapped("Use a file name without folders or reserved characters.");
				else
					ImGui::TextWrapped("%s", scenePath.c_str());
				if (valid && std::ranges::any_of(m_State->Scenes, [&scenePath](const VfsPath& pathValue)
				{
					return pathValue.GetPath() == scenePath;
				}))
				{
					valid = false;
					ImGui::TextWrapped("A scene with this name already exists. Choose another name or open the existing scene.");
				}
			}
			else
			{
				Utils::EditorSectionHeading("Scenes in this project");
				const float refreshWidth = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
				ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - refreshWidth - ImGui::GetStyle().ItemSpacing.x));
				ImGui::InputTextWithHint("##SceneSearch", "Search scenes", &m_State->SceneSearch);
				ImGui::SameLine();
				if (ImGui::Button("Refresh"))
					m_State->RefreshScenes();
				if (ImGui::BeginChild("##SceneList", ImVec2(0, std::min(280.0f * scale, viewport->Size.y * 0.35f)), ImGuiChildFlags_Borders))
				{
					bool any = false;
					for (const auto& path : m_State->Scenes)
					{
						if (!WorkspaceSearchMatches(path.GetPath(), m_State->SceneSearch))
							continue;
						any = true;
						const std::string identity = path.ToString();
						ImGui::PushOverrideID(ImHashData(identity.data(), identity.size(), ImGui::GetCurrentWindow()->IDStack.back()));
						const ImVec2 row = ImGui::GetCursorScreenPos();
						const float height = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
						if (ImGui::Selectable("##Scene", path.GetPath() == m_State->SelectedScene, ImGuiSelectableFlags_None, ImVec2(0, height)))
							m_State->SelectedScene = std::string(path.GetPath());
						const ImVec2 end = ImGui::GetItemRectMax();
						const std::string name(path.GetStem());
						const std::string folder = path.GetParent().IsRoot() ? "Project root" : std::string(path.GetParent().GetPath());
						ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(row.x, row.y + ImGui::GetStyle().FramePadding.y), end, end.x, name.data(), name.data() + name.size(), nullptr);
						ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
						ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(row.x, row.y + ImGui::GetStyle().FramePadding.y + ImGui::GetTextLineHeightWithSpacing()), end, end.x, folder.data(), folder.data() + folder.size(), nullptr);
						ImGui::PopStyleColor();
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", path.ToString().c_str());
						ImGui::PopID();
					}
					if (!any)
						Utils::EditorEmptyState(m_State->Scenes.empty() ? "No scenes yet" : "No matching scenes", m_State->Scenes.empty() ? "Create a scene from File > New scene." : "Try a different name or folder.");
				}
				ImGui::EndChild();
				scenePath = m_State->SelectedScene;
				valid = !scenePath.empty();
			}
			// A scene that is already open in a tab is shown rather than opened again (scene.open refuses its path).
			OpenSceneId openTab = NoOpenScene;
			if (!m_State->CreateScene && valid)
			{
				if (const auto path = VfsPath::Create("project", scenePath))
					openTab = editor.FindOpenScene(*path);
			}
			// Open in a new tab (scene.new and scene.open keepOpen): every open scene stays open, so none needs saving first.
			const bool canKeepOpen = editor.GetOpenScenes().size() < MaxOpenScenes;
			const bool keepOpen = editor.HasScene() && openTab == NoOpenScene && canKeepOpen && m_State->KeepOpen;
			if (editor.HasScene() && openTab == NoOpenScene)
			{
				ImGui::Spacing();
				bool shown = keepOpen;
				ImGui::BeginDisabled(!canKeepOpen);
				if (ImGui::Checkbox("Open in a new tab", &shown))
					m_State->KeepOpen = shown;
				ImGui::EndDisabled();
				if (!canKeepOpen && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("The editor keeps up to %zu scenes open. Close a scene tab first.", MaxOpenScenes);
			}
			if (openTab != NoOpenScene)
			{
				ImGui::Separator();
				ImGui::TextWrapped("This scene is already open in a tab.");
			}
			else if (editor.IsSceneDirty() && !keepOpen)
			{
				ImGui::Separator();
				ImGui::TextWrapped("The current scene has unsaved changes.");
				ImGui::BeginDisabled(editor.IsReadOnly());
				if (ImGui::RadioButton("Save changes first", m_State->SaveForNewScene))
				{
					m_State->SaveForNewScene = true;
					m_State->DiscardForNewScene = false;
				}
				ImGui::EndDisabled();
				if (ImGui::RadioButton("Discard changes", m_State->DiscardForNewScene))
				{
					m_State->SaveForNewScene = false;
					m_State->DiscardForNewScene = true;
				}
				valid &= m_State->SaveForNewScene || m_State->DiscardForNewScene;
			}
			if (!m_State->SceneDialogError.empty())
				ImGui::TextWrapped("%s", m_State->SceneDialogError.c_str());
			if (session != nullptr)
				ImGui::TextWrapped("Stop the preview before changing scenes.");
			ImGui::Separator();
			const auto submitScene = [this, &context](std::string_view method, const Json& params)
			{
				const auto submitted = context.Actions.Submit(method, params);
				if (submitted)
				{
					m_State->Tickets.push_back(*submitted);
					m_State->SceneTicket = *submitted;
					m_State->CancelGesture = true;
					m_State->SceneDialogError.clear();
				}
				else
					m_State->SceneDialogError = submitted.error().GetMessageText();
			};
			if (openTab != NoOpenScene)
			{
				if (ImGui::Button("Switch to scene"))
					submitScene("scene.activate", Json{ { "scene", openTab } });
			}
			else
			{
				ImGui::BeginDisabled(!valid || (m_State->CreateScene && editor.IsReadOnly()));
				if (ImGui::Button(m_State->CreateScene ? "Create scene" : "Open scene"))
				{
					Json params{ { "path", scenePath } };
					if (keepOpen)
						params["keepOpen"] = true;
					else
					{
						params["save"] = m_State->SaveForNewScene;
						params["discardChanges"] = m_State->DiscardForNewScene;
					}
					submitScene(m_State->CreateScene ? "scene.new" : "scene.open", params);
				}
				ImGui::EndDisabled();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(m_State->SceneTicket.has_value());
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndDisabled();
			ImGui::EndPopup();
		}
		m_State->DrawClosePrompt(*viewport);
		// These run outside every panel window, so they route globally: they work whichever panel has focus, while an active
		// text field keeps its own Ctrl+Z and Ctrl+Y (InputText claims them through a higher-priority focused route).
		// A modal dialog owns the keyboard, so they wait until it closes.
		if (ImGui::GetTopMostPopupModal() == nullptr)
		{
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal) && canSave)
				queue("scene.save", Json::object(), true);
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal) && canUndo)
				queue("edit.undo", Json::object(), true);
			if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) && canRedo)
				queue("edit.redo", Json::object(), true);
		}
		if (editor.IsSceneChangedOnDisk() || !m_State->ErrorMessage.empty())
		{
			ImGui::Begin("Notifications", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
			if (editor.IsSceneChangedOnDisk())
			{
				ImGui::TextWrapped("Scene changed on disk. Reload discards unsaved scene edits.");
				if (ImGui::Button("Reload scene") && editor.GetScenePath())
					queue("scene.open", Json{ { "path", editor.GetScenePath()->ToString() }, { "reload", true }, { "discardChanges", true } }, true);
			}
			if (!m_State->ErrorMessage.empty())
			{
				ImGui::TextWrapped("%s", m_State->ErrorMessage.c_str());
				if (ImGui::Button("Dismiss"))
					m_State->ErrorMessage.clear();
			}
			ImGui::End();
		}
		Status firstFailure;
		const std::vector<EditorPanel> panels(ui.GetOpenPanels().begin(), ui.GetOpenPanels().end());
		bool gameDrawn = false;
		bool sceneDrawn = false;
		for (const EditorPanel panel : panels)
		{
			bool open = true;
			if (panel == EditorPanel::ProjectLauncher && !editor.HasProject())
			{
				ImGui::SetNextWindowSize(ImVec2(std::min(920.0f * scale, work.GetWidth() * 0.9f), std::min(640.0f * scale, work.GetHeight() * 0.9f)), ImGuiCond_FirstUseEver);
				ImGui::SetNextWindowPos(work.GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
			}
			if (m_State->FocusRequest == panel)
			{
				ImGui::SetNextWindowFocus();
				m_State->FocusRequest.reset();
			}
			// The viewport panels draw into this window and name its flags; the other panels use the defaults.
			ImGuiWindowFlags windowFlags = ImGuiWindowFlags_None;
			if (panel == EditorPanel::SceneViewport)
				windowFlags = SceneViewportPanel::GetWindowFlags();
			else if (panel == EditorPanel::GameViewport)
				windowFlags = GameViewportPanel::GetWindowFlags();
			const bool visible = ImGui::Begin(Utils::EditorWindowTitle(panel), &open, windowFlags);
			Status drawn;
			if (visible)
			{
				switch (panel)
				{
					case EditorPanel::SceneHierarchy: drawn = m_State->Hierarchy.Draw(context); break;
					case EditorPanel::Inspector:      drawn = m_State->Inspector.Draw(context); break;
					case EditorPanel::SceneViewport:
						drawn = m_State->SceneViewport.Draw(context);
						sceneDrawn = true;
						break;
					case EditorPanel::GameViewport:
						drawn = m_State->GameViewport.Draw(context);
						gameDrawn = true;
						break;
					case EditorPanel::ContentBrowser:  drawn = m_State->ContentBrowser.Draw(context); break;
					case EditorPanel::Console:         drawn = m_State->Console.Draw(context); break;
					case EditorPanel::Diagnostics:     drawn = m_State->Diagnostics.Draw(context); break;
					case EditorPanel::ProjectSettings: drawn = m_State->Settings.Draw(context); break;
					case EditorPanel::Stats:           drawn = m_State->Stats.Draw(context); break;
					case EditorPanel::Automation:      drawn = m_State->Automation.Draw(context); break;
					case EditorPanel::UndoHistory:     drawn = m_State->History.Draw(context); break;
					case EditorPanel::Native:          drawn = m_State->Native.Draw(context); break;
					case EditorPanel::ProjectLauncher:
						if (!editor.HasProject())
							drawn = m_State->Launcher.Draw(context);
						break;
				}
			}
			ImGui::End();
			if (!drawn && firstFailure)
				firstFailure = std::unexpected(std::move(drawn).error());
			if (!open)
			{
				ENGINE_TRY(ui.SetPanelOpen(panel, false));
				if (panel == EditorPanel::Inspector)
					m_State->Inspector.CancelEdit(context);
			}
		}
		if (m_State->SelectDefaultTabs)
		{
			// Select after all windows join their tabs; this runs only for a new or explicitly reset dock layout.
			for (const EditorPanel panel : { EditorPanel::SceneViewport, EditorPanel::ContentBrowser })
				if (ImGuiWindow* window = ImGui::FindWindowByName(Utils::EditorWindowTitle(panel)); window && window->DockNode)
				{
					window->DockNode->SelectedTabId = window->TabId;
					if (ImGuiTabBar* tabs = window->DockNode->TabBar)
						tabs->SelectedTabId = tabs->NextSelectedTabId = window->TabId;
				}
			ImGui::SetWindowFocus(Utils::EditorWindowTitle(EditorPanel::ContentBrowser));
			m_State->SelectDefaultTabs = false;
		}
		if (!gameDrawn)
		{
			context.Viewports.SetGameInputFocused(false);
			context.Viewports.SetRectangle(ViewportView::Game, {});
		}
		if (!sceneDrawn)
			context.Viewports.SetRectangle(ViewportView::Scene, {});
		if (std::vector<EditorPanel> persisted = Utils::SavedWorkspacePanels(ui); persisted != m_State->WrittenPanels)
		{
			// Menu toggles, close buttons and resets all reach the ini through Dear ImGui's delayed save.
			ImGui::MarkIniSettingsDirty();
			m_State->WrittenPanels = std::move(persisted);
		}
		if (context.Recovery.GetOffer)
		{
			const auto offer = context.Recovery.GetOffer();
			if (offer)
				ImGui::OpenPopup("Recover unsaved scene");
			if (ImGui::BeginPopupModal("Recover unsaved scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				if (offer)
				{
					// One offer covers every scene with unsaved changes, and Recover recovers them all (decision 15).
					if (offer->SceneNames.size() > 1)
					{
						std::string names;
						for (const std::string& name : offer->SceneNames)
							names += std::format("{}{}", names.empty() ? "" : ", ", name);
						ImGui::TextWrapped("Unsaved versions of %zu scenes are available: %s.", offer->SceneNames.size(), names.c_str());
					}
					else
						ImGui::TextWrapped("An unsaved version of %s is available.", offer->Recovery.SceneName.c_str());
					if (offer->Failure)
						ImGui::TextWrapped("%s", offer->Failure->GetMessageText().c_str());
					if (!context.Recovery.QueueDecision)
						ImGui::TextDisabled("Recovery service is unavailable.");
					ImGui::BeginDisabled(offer->DecisionPending || !context.Recovery.QueueDecision);
					std::optional<EditorRecoveryDecision> decision;
					ImGui::BeginDisabled(editor.IsReadOnly());
					if (ImGui::Button("Recover"))
						decision = EditorRecoveryDecision::Accept;
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (ImGui::Button("Keep current scene"))
						decision = EditorRecoveryDecision::Decline;
					ImGui::EndDisabled();
					if (decision)
					{
						auto queued = context.Recovery.QueueDecision(*offer, *decision);
						if (!queued)
							m_State->ReportFailure(queued.error());
					}
				}
				else
					ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
		}
		return firstFailure;
	}

	void EditorLayer::RequestFrame()
	{
		m_State->FrameRequested = true;
		m_State->RequestedAfter = m_State->Context.Editor.GetUiState().GetCompletedFrame();
	}

	Status EditorLayer::RevealPanel(EditorPanel panel)
	{
		ENGINE_TRY(m_State->Context.Editor.GetUiState().SetPanelOpen(panel, true));
		m_State->FocusRequest = panel;
		return {};
	}

	bool EditorLayer::IsFrameRequested() const
	{
		return m_State->FrameRequested && m_State->Context.Editor.GetUiState().GetCompletedFrame() <= m_State->RequestedAfter;
	}

}
