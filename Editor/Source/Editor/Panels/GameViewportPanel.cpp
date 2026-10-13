#include "EditorPCH.h"
#include "Editor/Panels/GameViewportPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/EditorUiState.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Platform/Window.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace Engine {

	int GameViewportPanel::GetWindowFlags()
	{
		return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	}

	Status GameViewportPanel::Draw(EditorPanelContext& context)
	{
		EditorViewportState& state = context.Editor.GetViewportState();
		// Drawn into the caller's window (EditorLayer's Begin with GetWindowFlags); a press on the image never moves it.
		ImGui::GetCurrentWindow()->BgClickFlags = ImGuiWindowBgClickFlags_None;
		Status result;
		const glm::uvec2 resolution = state.GetGameResolution();
		const std::string resolutionLabel = resolution.x == 0 ? "Free resolution" : std::format("{} x {}", resolution.x, resolution.y);
		const float toolbarRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
		ImGui::SetNextItemWidth(std::min(180.0f * Utils::EditorUiScale(), ImGui::GetContentRegionAvail().x));
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
		const bool captured = session != nullptr && context.Viewports.IsGameInputFocused();
		const char* inputHint = !session ? "Camera preview - press Play to interact"
			: captured                   ? "Game input captured - press Esc to release"
										 : "Click the view to send game input";
		if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(inputHint).x <= toolbarRight)
		{
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", inputHint);
		}
		if (session && session->IsLockstep())
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Agent controls time (owner %llu)", static_cast<unsigned long long>(session->GetLockstepOwner()));
		ImGui::Separator();
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const ImVec2 minimum = ImGui::GetCursorScreenPos();
		EditorViewportRect rectangle;
		const EditorViewportImage image = context.Viewports.GetImage(ViewportView::Game);
		bool clicked = false;
		bool imageActive = false;
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
			const ImVec2 size(rectangle.Size.x, rectangle.Size.y);
			if (image.Texture != 0 && image.Width != 0 && image.Height != 0)
			{
				rectangle.ImageSize = { image.Width, image.Height };
				ImGui::GetWindowDrawList()->AddImage(image.Texture, ImVec2(rectangle.Min.x, rectangle.Min.y),
					ImVec2(rectangle.Min.x + size.x, rectangle.Min.y + size.y));
				// The image owns its presses (a held button never turns into a window move) and captures game input.
				ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
				ImGui::InvisibleButton("##GameImage", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
				ImGui::PopItemFlag();
				clicked = ImGui::IsItemActivated();
				imageActive = ImGui::IsItemActive();
				rectangle.Hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
				// While the game owns input its wheel is game input, never a window scroll.
				if (captured)
					ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
			}
			else
			{
				ImGui::Dummy(size);
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
		// Click-to-capture: a press on the image captures game input while playing. It persists while the window keeps
		// focus, the image is held or the game has locked the cursor (whose unbounded positions leave the image), and
		// never depends on per-frame hover. Escape, focus moving to another window and the end of play release it; the
		// host also releases it on native window blur, which a later frame never re-captures without a new click.
		bool focused = captured || (session != nullptr && clicked);
		const Window* window = context.Editor.GetEngine().GetWindow();
		const bool cursorLocked = window != nullptr && window->GetCursorMode() == CursorMode::Locked;
		if (!session || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (!rectangle.Focused && !imageActive && !cursorLocked))
			focused = false;
		context.Viewports.SetGameInputFocused(focused);
		return result;
	}

}
