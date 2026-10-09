#pragma once

#include "Editor/EditorPanelContext.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/Automation/EditorAutomationControls.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Support/AutomationTestClient.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <functional>
#include <string_view>

namespace Engine::Test {

	class PanelInteractionViewport final : public EditorViewportHost
	{
	public:
		EditorViewportImage GetImage(ViewportView) const override { return {}; }
		void SetRectangle(ViewportView, const EditorViewportRect& rectangle) override { CHECK(rectangle.Size == glm::vec2(0.0f)); }
		Status RequestPick(const EditorViewportClick&) override { return MakeError(ErrorCode::Unsupported, "picking is outside this panel test"); }
		Status ConfigureSceneSnapshot(RenderSnapshot&, const EditorViewportOptions&, std::span<const UUID>) override { return MakeError(ErrorCode::Unsupported, "rendering is outside this panel test"); }
		void SetGameInputFocused(bool focused) override { CHECK_FALSE(focused); }
	};

	class PanelInteractionUi
	{
	public:
		explicit PanelInteractionUi(bool wrapWindow = true)
			: m_Context(ImGui::CreateContext()), m_WrapWindow(wrapWindow)
		{
			auto& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1000.0f, 1400.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		}
		~PanelInteractionUi() { ImGui::DestroyContext(m_Context); }
		PanelInteractionUi(const PanelInteractionUi&) = delete;
		PanelInteractionUi& operator=(const PanelInteractionUi&) = delete;

		[[nodiscard]] Status Frame(const std::function<Status()>& draw)
		{
			ImGui::NewFrame();
			if (m_WrapWindow)
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1000.0f, 1400.0f));
				ImGui::Begin("Panel interaction");
			}
			const Status result = draw();
			if (m_WrapWindow)
				ImGui::End();
			ImGui::Render();
			return result;
		}
		[[nodiscard]] Status Click(const std::function<Status()>& draw, ImVec2 position)
		{
			ImGui::GetIO().AddMousePosEvent(position.x, position.y);
			ENGINE_TRY(Frame(draw));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			ENGINE_TRY(Frame(draw));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			return Frame(draw);
		}
		[[nodiscard]] static ImVec2 LastItemCenter()
		{
			const auto minimum = ImGui::GetItemRectMin();
			const auto maximum = ImGui::GetItemRectMax();
			return ImVec2((minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f);
		}
	private:
		ImGuiContext* m_Context = nullptr;
		bool m_WrapWindow = true;
	};

	class PanelInteractionFixture
	{
	public:
		explicit PanelInteractionFixture(std::string_view label, bool openScene = true)
			: m_Automation(label, openScene), m_Actions(m_Automation.GetEditor(), m_Automation.GetClient().GetServer()), m_Controls(m_Automation.GetEditor(), m_Automation.GetClient().GetServer()), m_Edits(m_Automation.GetEditor()), m_Thumbnails(m_Automation.GetEditor(), [this](const ThumbnailRequest& request)
		{
			++m_Renders;
			return CreateImage(request.Size, request.Size, nvrhi::Format::RGBA8_UNORM);
		}),
			  m_Gizmos(m_Automation.GetEditor()),
			  m_Context{ m_Automation.GetEditor(), m_Automation.GetClient().GetServer(), m_Actions, m_Controls, m_Viewports, m_Edits, m_Thumbnails, m_Gizmos }
		{
		}

		[[nodiscard]] AutomationFixture& GetAutomation() { return m_Automation; }
		[[nodiscard]] EditorPanelContext& GetContext() { return m_Context; }
		[[nodiscard]] uint32_t GetRenderCount() const { return m_Renders; }
	private:
		AutomationFixture m_Automation;
		EditorActions m_Actions;
		EditorAutomationControls m_Controls;
		PanelInteractionViewport m_Viewports{};
		ReflectedEditController m_Edits;
		uint32_t m_Renders = 0;
		ThumbnailCache m_Thumbnails;
		GizmoController m_Gizmos;
		EditorPanelContext m_Context;
	};

}
