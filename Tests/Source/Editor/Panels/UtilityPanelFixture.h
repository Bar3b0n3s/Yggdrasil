#pragma once

#include "Editor/EditorPanelContext.h"
#include "Editor/Private/EditorHostPreferences.h"
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

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

		// The server's wall clock: Real reads steady_clock; Stepped advances 100 ms per reading, so every Pump advances a
		// pending test run by exactly one runner step (TestRunFrameBudget is 50 ms) in every configuration.
		enum class UtilityPanelClock
		{
			Real,
			Stepped
		};

		// Real editor, server, queued human actions, the host's preference queue (EditorHostPreferences) and ImGui. Only the
		// renderer is an injected boundary. Frames advance explicitly; no GPU, sleeps, or private ImGui inspection is involved.
		class UtilityPanelFixture
		{
		public:
			explicit UtilityPanelFixture(UtilityPanelClock clock = UtilityPanelClock::Real)
				: m_Clock(clock), m_Environment("UtilityPanels"), m_Client(m_Environment.GetEditor(), MakeSpecification()), m_Actions(m_Environment.GetEditor(), m_Client.GetServer()), m_Controls(m_Environment.GetEditor(), m_Client.GetServer()), m_Edits(m_Environment.GetEditor()), m_Thumbnails(m_Environment.GetEditor(), {}), m_Gizmos(m_Environment.GetEditor()), m_Context{ m_Environment.GetEditor(), m_Client.GetServer(), m_Actions, m_Controls, m_Viewports, m_Edits, m_Thumbnails, m_Gizmos }
			{
				m_ImGui = ImGui::CreateContext();
				ImGuiIO& io = ImGui::GetIO();
				io.IniFilename = nullptr;
				io.LogFilename = nullptr;
				io.DisplaySize = ImVec2(1200.0f, 900.0f);
				io.DeltaTime = 1.0f / 60.0f;
				io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
				REQUIRE(io.Fonts->Build());
				// The production host preference queue (EditorApp's), applied by Pump as at the host's safe point.
				REQUIRE(m_Preferences.Load(m_Environment.GetEditor().GetVfs()));
				m_Context.AutomationPreferences.GetState = [this]()
				{
					return m_Preferences.GetAutomationState();
				};
				m_Context.AutomationPreferences.QueueChange = [this](bool allowed)
				{
					return m_Preferences.QueueAutomationChange(allowed);
				};
				m_Context.ExternalEditor.GetState = [this]()
				{
					return m_Preferences.GetExternalEditorState();
				};
				m_Context.ExternalEditor.QueueChange = [this](std::string executable, std::vector<std::string> arguments)
				{
					return m_Preferences.QueueExternalEditorChange(std::move(executable), std::move(arguments));
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

			EditorHostPreferences& GetPreferences() { return m_Preferences; }

			// The host's safe point: the queued preference changes, then the queued human actions. A failed preference change
			// stays published as its Failure, which the panel shows, as in EditorApp.
			void Pump()
			{
				static_cast<void>(m_Preferences.Pump([this](bool allowed)
				{
					return m_Controls.SetAllowAiAutomation(allowed);
				}));
				static_cast<void>(m_Preferences.PumpExternalEditor(m_Environment.GetEditor().GetVfs()));
				m_Actions.Pump();
			}

			// A queued human action submitted and discarded at once: every ticket a panel queues later is greater.
			uint64_t ProbeActionTicket()
			{
				const Result<uint64_t> probe = m_Actions.Submit("stats.get", Json::object());
				REQUIRE_MESSAGE(probe.has_value(), probe.error().ToString());
				REQUIRE(m_Actions.Discard(*probe).has_value());
				return *probe;
			}

			// The first ticket after `after` that is still queued (submitted, not yet pumped); nullopt when there is none.
			// Never consumes a completion: a known ticket found here is still pending.
			std::optional<uint64_t> FindPendingAction(uint64_t after)
			{
				for (uint64_t ticket = after + 1; ticket <= after + 16; ++ticket)
				{
					const auto result = m_Actions.TakeResult(ticket);
					if (!result.has_value())
						continue;
					REQUIRE_FALSE(result->has_value());
					return ticket;
				}
				return std::nullopt;
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
			AutomationServerSpecification MakeSpecification()
			{
				auto specification = MakeTestServerSpecification();
				specification.SessionsDirectory = m_Environment.GetDirectory() / "Sessions";
				if (m_Clock == UtilityPanelClock::Stepped)
				{
					specification.WallClock = [this]()
					{
						m_Now += std::chrono::milliseconds(100);
						return m_Now;
					};
				}
				return specification;
			}
		private:
			UtilityPanelClock m_Clock = UtilityPanelClock::Real;
			std::chrono::steady_clock::time_point m_Now{}; // the Stepped clock's reading; read only through the server
			EditorTestFixture m_Environment;
			AutomationTestClient m_Client;
			EditorActions m_Actions;
			EditorAutomationControls m_Controls;
			ReflectedEditController m_Edits;
			ThumbnailCache m_Thumbnails;
			GizmoController m_Gizmos;
			UtilityPanelUnusedViewport m_Viewports;
			EditorPanelContext m_Context;
			EditorHostPreferences m_Preferences{};
			uint64_t m_Frame = 0;
			ImGuiContext* m_ImGui = nullptr; // owned C API context, destroyed in the destructor
		};

	}

}
