#include "EditorPCH.h"
#include "Editor/EditorLayer.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Panels/AutomationPanel.h"
#include "Editor/Panels/ConsolePanel.h"
#include "Editor/Panels/ContentBrowserPanel.h"
#include "Editor/Panels/DiagnosticsPanel.h"
#include "Editor/Panels/GameViewportPanel.h"
#include "Editor/Panels/InspectorPanel.h"
#include "Editor/Panels/ProjectSettingsPanel.h"
#include "Editor/Panels/SceneHierarchyPanel.h"
#include "Editor/Panels/SceneViewportPanel.h"
#include "Editor/Panels/StatsPanel.h"
#include "Editor/Panels/UndoHistoryPanel.h"
#include "Editor/ProjectLauncher.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "Engine/Core/Log.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Engine {

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
		ProjectLauncher Launcher{};
		std::vector<uint64_t> Tickets{};
		std::string ErrorMessage{};
		std::string NewScenePath = "Assets/Scenes/Untitled.scene";
		bool ShowNewScene = false;
		bool CreateScene = true;
		bool DiscardForNewScene = false;
		bool ResetLayout = false;
		bool SelectDefaultTabs = false;
		bool CancelGesture = false;
		bool FrameRequested = false;
		bool HadProject = false;
		uint64_t RequestedAfter = 0;

		void ReportFailure(const Error& error)
		{
			const std::string message = error.GetMessageText();
			if (ErrorMessage != message)
				ENGINE_ERROR("Editor action: {}", error.ToString());
			ErrorMessage = message;
		}
	};

	EditorLayer::EditorLayer(EditorPanelContext& context)
		: m_State(CreateScope<State>(context))
	{
		m_State->HadProject = context.Editor.HasProject();
		if (context.Editor.GetUiState().GetOpenPanels().empty())
			context.Editor.GetUiState().ResetLayout(context.Editor.HasProject());
	}

	EditorLayer::~EditorLayer() = default;

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
		ENGINE_TRY(m_State->Inspector.ApplyQueuedEdit(context));
		context.Actions.Pump();
		for (auto ticket = m_State->Tickets.begin(); ticket != m_State->Tickets.end();)
		{
			auto result = context.Actions.TakeResult(*ticket);
			if (!result || result->has_value())
			{
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
			// Loading a queued asset can advance version zero to one. The next Draw queues its current version.
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
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const ImGuiID dock = ImGui::DockSpaceOverViewport(0, viewport);
		if (m_State->ResetLayout || ImGui::DockBuilderGetNode(dock) == nullptr || ImGui::DockBuilderGetNode(dock)->IsLeafNode())
		{
			// Existing split nodes came from the user's ini. Only an explicit reset or the first empty dock builds defaults.
			if (m_State->ResetLayout || ImGui::FindWindowSettingsByID(ImHashStr("SceneViewport")) == nullptr)
			{
				ImGui::DockBuilderRemoveNode(dock);
				ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
				ImGui::DockBuilderSetNodeSize(dock, viewport->WorkSize);
				ImGuiID center = dock;
				const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
				const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
				const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
				ImGui::DockBuilderDockWindow("SceneHierarchy", left);
				ImGui::DockBuilderDockWindow("Inspector", right);
				ImGui::DockBuilderDockWindow("SceneViewport", center);
				ImGui::DockBuilderDockWindow("GameViewport", center);
				for (const char* name : { "ContentBrowser", "Console", "Diagnostics", "Stats", "Automation", "UndoHistory" })
					ImGui::DockBuilderDockWindow(name, bottom);
				ImGui::DockBuilderFinish(dock);
				m_State->SelectDefaultTabs = true;
			}
			m_State->ResetLayout = false;
		}
		auto queue = [this, &context](std::string_view method, const Json& params, bool cancelGesture = false)
		{
			auto submitted = context.Actions.Submit(method, params);
			if (submitted)
			{
				m_State->Tickets.push_back(*submitted);
				m_State->CancelGesture |= cancelGesture;
			}
			else
				m_State->ReportFailure(submitted.error());
		};
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New scene", nullptr, false, editor.HasProject() && !editor.IsReadOnly()))
				{
					m_State->ShowNewScene = true;
					m_State->CreateScene = true;
				}
				if (ImGui::MenuItem("Open scene", nullptr, false, editor.HasProject()))
				{
					m_State->ShowNewScene = true;
					m_State->CreateScene = false;
				}
				if (ImGui::MenuItem("Save scene", "Ctrl+S", false, editor.HasScene() && !editor.IsReadOnly()))
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
				if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !editor.IsReadOnly() && editor.GetHistory().CanUndo()))
					queue("edit.undo", Json::object(), true);
				if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !editor.IsReadOnly() && editor.GetHistory().CanRedo()))
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
					if (ImGui::MenuItem(EditorPanelToString(panel).data(), nullptr, &open))
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
			const PlaySession* session = editor.GetPlay().GetSession();
			const bool agentTime = session != nullptr && session->IsLockstep() && session->GetLockstepOwner() != NoClient;
			ImGui::BeginDisabled(!editor.HasScene() || agentTime);
			if (session == nullptr)
			{
				if (ImGui::Button("Play"))
					queue("play.start", Json{ { "mode", "Play" } }, true);
				if (ImGui::Button("Simulate"))
					queue("play.start", Json{ { "mode", "Simulate" } }, true);
			}
			else
			{
				if (ImGui::Button(session->IsPaused() ? "Resume" : "Pause"))
					queue(session->IsPaused() ? "play.resume" : "play.pause", Json::object());
				ImGui::BeginDisabled(!session->IsPaused());
				if (ImGui::Button("Step"))
					queue("play.step", Json{ { "ticks", 1 } }, true);
				ImGui::EndDisabled();
				if (ImGui::Button("Stop"))
					queue("play.stop", Json::object(), true);
			}
			ImGui::EndDisabled();
			if (agentTime)
				ImGui::TextUnformatted("Agent controls time");
			ImGui::EndMainMenuBar();
		}
		const char* sceneDialog = m_State->CreateScene ? "New scene" : "Open scene";
		if (m_State->ShowNewScene)
		{
			ImGui::OpenPopup(sceneDialog);
			m_State->ShowNewScene = false;
			m_State->DiscardForNewScene = false;
		}
		if (ImGui::BeginPopupModal(sceneDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Project path", &m_State->NewScenePath);
			ImGui::Checkbox("Discard unsaved scene edits", &m_State->DiscardForNewScene);
			ImGui::BeginDisabled(m_State->NewScenePath.empty() || !editor.HasProject() || (m_State->CreateScene && editor.IsReadOnly()));
			if (ImGui::Button(m_State->CreateScene ? "Create" : "Open"))
			{
				queue(m_State->CreateScene ? "scene.new" : "scene.open", Json{ { "path", m_State->NewScenePath }, { "discardChanges", m_State->DiscardForNewScene } }, true);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S) && editor.HasScene() && !editor.IsReadOnly())
			queue("scene.save", Json::object(), true);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z) && !editor.IsReadOnly() && editor.GetHistory().CanUndo())
			queue("edit.undo", Json::object(), true);
		if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y) && !editor.IsReadOnly() && editor.GetHistory().CanRedo())
			queue("edit.redo", Json::object(), true);
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
				ImGui::TextWrapped("%s", m_State->ErrorMessage.c_str());
			ImGui::End();
		}
		Status firstFailure;
		const std::vector<EditorPanel> panels(ui.GetOpenPanels().begin(), ui.GetOpenPanels().end());
		bool gameDrawn = false;
		bool sceneDrawn = false;
		for (const EditorPanel panel : panels)
		{
			bool open = true;
			const bool visible = ImGui::Begin(EditorPanelToString(panel).data(), &open);
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
			for (const char* name : { "SceneViewport", "ContentBrowser" })
				if (ImGuiWindow* window = ImGui::FindWindowByName(name); window && window->DockNode)
				{
					window->DockNode->SelectedTabId = window->TabId;
					if (ImGuiTabBar* tabs = window->DockNode->TabBar)
						tabs->SelectedTabId = tabs->NextSelectedTabId = window->TabId;
				}
			ImGui::SetWindowFocus("ContentBrowser");
			m_State->SelectDefaultTabs = false;
		}
		if (!gameDrawn)
		{
			context.Viewports.SetGameInputFocused(false);
			context.Viewports.SetRectangle(ViewportView::Game, {});
		}
		if (!sceneDrawn)
			context.Viewports.SetRectangle(ViewportView::Scene, {});
		if (context.Recovery.GetOffer)
		{
			const auto offer = context.Recovery.GetOffer();
			if (offer)
				ImGui::OpenPopup("Recover unsaved scene");
			if (ImGui::BeginPopupModal("Recover unsaved scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				if (offer)
				{
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

	bool EditorLayer::IsFrameRequested() const
	{
		return m_State->FrameRequested && m_State->Context.Editor.GetUiState().GetCompletedFrame() <= m_State->RequestedAfter;
	}

}
