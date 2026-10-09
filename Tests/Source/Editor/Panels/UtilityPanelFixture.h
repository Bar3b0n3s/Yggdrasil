#pragma once

#include "Editor/EditorPanelContext.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Core/FileSystem.h"
#include "Support/AutomationTestClient.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <functional>

namespace Engine {

	namespace Test {

		// Utility panels never use renderer services. Fail if one accidentally acquires that dependency.
		class UtilityPanelUnusedViewport final : public EditorViewportHost
		{
		public:
			[[nodiscard]] EditorViewportImage GetImage(ViewportView) const override
			{
				FAIL("utility panel requested a viewport image");
				return {};
			}
			void SetRectangle(ViewportView, const EditorViewportRect&) override { FAIL("utility panel changed a viewport"); }
			[[nodiscard]] Status RequestPick(const EditorViewportClick&) override { return MakeError(ErrorCode::Unsupported, "utility panel has no viewport"); }
			[[nodiscard]] Status ConfigureSceneSnapshot(RenderSnapshot&, const EditorViewportOptions&, std::span<const UUID>) override { return MakeError(ErrorCode::Unsupported, "utility panel has no viewport"); }
			void SetGameInputFocused(bool) override { FAIL("utility panel changed game input focus"); }
		};

		// Real editor, server, queued human actions and ImGui. Only the renderer and host-owned preference queue are
		// injected boundaries. Frames advance explicitly; no GPU, sleeps, or private ImGui inspection is involved.
		class UtilityPanelFixture
		{
		public:
			UtilityPanelFixture()
				: m_Environment("UtilityPanels"), m_Client(m_Environment.GetEditor(), MakeSpecification()), m_Actions(m_Environment.GetEditor(), m_Client.GetServer()), m_Controls(m_Environment.GetEditor(), m_Client.GetServer()), m_Edits(m_Environment.GetEditor()), m_Thumbnails(m_Environment.GetEditor(), {}), m_Gizmos(m_Environment.GetEditor()), m_Context{ m_Environment.GetEditor(), m_Client.GetServer(), m_Actions, m_Controls, m_Viewports, m_Edits, m_Thumbnails, m_Gizmos }
			{
				m_ImGui = ImGui::CreateContext();
				ImGuiIO& io = ImGui::GetIO();
				io.IniFilename = nullptr;
				io.LogFilename = nullptr;
				io.DisplaySize = ImVec2(1200.0f, 900.0f);
				io.DeltaTime = 1.0f / 60.0f;
				io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
				REQUIRE(io.Fonts->Build());
				m_Context.AutomationPreferences.GetState = [this]()
				{
					return m_Preference;
				};
				m_Context.AutomationPreferences.QueueChange = [this](bool allowed) -> Status
				{
					if (m_Preference.Pending)
						return MakeError(ErrorCode::InvalidState, "preference change already pending");
					m_Preference.Pending = true;
					m_RequestedPreference = allowed;
					return {};
				};
			}

			~UtilityPanelFixture() { ImGui::DestroyContext(m_ImGui); }
			UtilityPanelFixture(const UtilityPanelFixture&) = delete;
			UtilityPanelFixture& operator=(const UtilityPanelFixture&) = delete;

			void OpenProject()
			{
				m_Environment.CreateAndOpenProject();
				m_Environment.CreateAndOpenScene();
			}

			EditorContext& GetEditor() { return m_Environment.GetEditor(); }
			AutomationTestClient& GetClient() { return m_Client; }
			EditorPanelContext& GetContext() { return m_Context; }

			void Pump()
			{
				if (m_Preference.Pending)
				{
					const Status status = m_Controls.SetAllowAiAutomation(m_RequestedPreference);
					m_Preference.Pending = false;
					if (status)
					{
						m_Preference.Allowed = m_RequestedPreference;
						m_Preference.Failure.reset();
					}
					else
						m_Preference.Failure = status.error();
				}
				m_Actions.Pump();
			}

			template<typename Panel>
			std::string Draw(Panel& panel, int focusOffset = -1)
			{
				const auto logPath = m_Environment.GetDirectory() / std::format("Frame{}.txt", m_Frame++);
				ImGui::NewFrame();
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1200.0f, 900.0f));
				ImGui::Begin("Utility panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
				ImGui::LogToFile(32, FileSystem::PathToUtf8(logPath).c_str());
				if (focusOffset >= 0)
					ImGui::SetKeyboardFocusHere(focusOffset);
				const Status drawn = panel.Draw(m_Context);
				ImGui::LogFinish();
				ImGui::End();
				ImGui::Render();
				REQUIRE_MESSAGE(drawn.has_value(), drawn.error().ToString());
				const auto text = FileSystem::ReadText(logPath);
				REQUIRE(text);
				return *text;
			}

			template<typename Panel>
			void ClickFirstItem(Panel& panel)
			{
				ClickAt(panel, 16.0f, 16.0f);
			}

			template<typename Panel>
			void ClickAt(Panel& panel, float x, float y)
			{
				ImGuiIO& io = ImGui::GetIO();
				io.AddMousePosEvent(x, y);
				Draw(panel);
				io.AddMouseButtonEvent(0, true);
				Draw(panel);
				io.AddMouseButtonEvent(0, false);
				Draw(panel);
			}

			template<typename Panel>
			void Press(Panel& panel, ImGuiKey key)
			{
				ImGui::GetIO().AddKeyEvent(key, true);
				Draw(panel);
				ImGui::GetIO().AddKeyEvent(key, false);
				Draw(panel);
			}

			template<typename Panel>
			void ReplaceFocusedText(Panel& panel, std::string_view text)
			{
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
				Press(panel, ImGuiKey_A);
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
				Draw(panel);
				ImGui::GetIO().AddInputCharactersUTF8(std::string(text).c_str());
				Draw(panel);
			}
		private:
			AutomationServerSpecification MakeSpecification() const
			{
				auto specification = MakeTestServerSpecification();
				specification.SessionsDirectory = m_Environment.GetDirectory() / "Sessions";
				return specification;
			}
		private:
			EditorTestFixture m_Environment;
			AutomationTestClient m_Client;
			EditorActions m_Actions;
			EditorAutomationControls m_Controls;
			ReflectedEditController m_Edits;
			ThumbnailCache m_Thumbnails;
			GizmoController m_Gizmos;
			UtilityPanelUnusedViewport m_Viewports;
			EditorPanelContext m_Context;
			EditorAutomationPreferenceState m_Preference{};
			bool m_RequestedPreference = false;
			uint64_t m_Frame = 0;
			ImGuiContext* m_ImGui = nullptr; // owned C API context, destroyed in the destructor
		};

	}

}
