#include "EditorPCH.h"
#include "Editor/Panels/GameViewportPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/EditorUiState.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace Engine {

	Status GameViewportPanel::Draw(EditorPanelContext& context)
	{
		EditorViewportState& state = context.Editor.GetViewportState();
		const std::string title(EditorPanelToString(EditorPanel::GameViewport));
		bool open = true;
		const bool visible = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		if (!visible || !open)
		{
			context.Viewports.SetRectangle(ViewportView::Game, {});
			context.Viewports.SetGameInputFocused(false);
			ImGui::End();
			return open ? Status{} : context.Editor.GetUiState().SetPanelOpen(EditorPanel::GameViewport, false);
		}
		Status result;
		const glm::uvec2 resolution = state.GetGameResolution();
		const std::string resolutionLabel = resolution.x == 0 ? "Free resolution" : std::format("{} x {}", resolution.x, resolution.y);
		ImGui::SetNextItemWidth(170.0f);
		if (ImGui::BeginCombo("##GameResolution", resolutionLabel.c_str()))
		{
			constexpr std::array<glm::uvec2, 5> Resolutions{ { { 0, 0 }, { 640, 360 }, { 1280, 720 }, { 1920, 1080 }, { 1080, 1920 } } };
			for (const glm::uvec2& option : Resolutions)
			{
				const std::string label = option.x == 0 ? "Free resolution" : std::format("{} x {}", option.x, option.y);
				if (ImGui::Selectable(label.c_str(), option == resolution))
					result = state.SetGameResolution(option);
			}
			ImGui::EndCombo();
		}
		const PlaySession* session = context.Editor.GetPlay().GetSession();
		if (session && session->IsLockstep())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Agent controls time (owner %llu)", static_cast<unsigned long long>(session->GetLockstepOwner()));
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const ImVec2 minimum = ImGui::GetCursorScreenPos();
		EditorViewportRect rectangle;
		const EditorViewportImage image = context.Viewports.GetImage(ViewportView::Game);
		if (std::isfinite(available.x) && std::isfinite(available.y) && available.x > 0.0f && available.y > 0.0f)
		{
			rectangle.Min = { minimum.x, minimum.y };
			rectangle.Size = { available.x, available.y };
			// During a resolution change, letterbox the image that actually exists, not the requested next image.
			if (resolution.x != 0 && image.Width != 0 && image.Height != 0)
			{
				const float scale = std::min(available.x / static_cast<float>(image.Width), available.y / static_cast<float>(image.Height));
				rectangle.Size = glm::vec2(image.Width, image.Height) * scale;
				rectangle.Min += (glm::vec2(available.x, available.y) - rectangle.Size) * 0.5f;
			}
			ImGui::SetCursorScreenPos(ImVec2(rectangle.Min.x, rectangle.Min.y));
			if (image.Texture != 0 && image.Width != 0 && image.Height != 0)
			{
				rectangle.ImageSize = { image.Width, image.Height };
				ImGui::Image(image.Texture, ImVec2(rectangle.Size.x, rectangle.Size.y));
				rectangle.Hovered = ImGui::IsItemHovered();
			}
			else
			{
				ImGui::Dummy(ImVec2(rectangle.Size.x, rectangle.Size.y));
			}
			rectangle.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			const Scene* scene = session ? &session->GetScene() : context.Editor.HasScene() ? &context.Editor.GetScene()
																							: nullptr;
			if (!scene || !FindPrimaryCamera(*scene))
				ImGui::GetWindowDrawList()->AddText(ImVec2(rectangle.Min.x + 12.0f, rectangle.Min.y + 12.0f), IM_COL32(220, 220, 220, 255), "No primary camera");
			else if (image.Texture == 0)
				ImGui::GetWindowDrawList()->AddText(ImVec2(rectangle.Min.x + 12.0f, rectangle.Min.y + 12.0f), IM_COL32(220, 220, 220, 255), "Game image unavailable");
		}
		context.Viewports.SetRectangle(ViewportView::Game, rectangle);
		context.Viewports.SetGameInputFocused(session && rectangle.Focused && rectangle.Hovered && !ImGui::GetIO().WantTextInput
			&& !ImGui::IsAnyItemActive() && ImGui::GetDragDropPayload() == nullptr && !context.Gizmos.IsDragging()
			&& !ImGuizmo::IsUsingAny() && !ImGuizmo::IsOver());
		ImGui::End();
		return result;
	}

}
