#pragma once

#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	struct EditorPanelContext;

	// Displays the host's scene image and records next-frame geometry. The image is one ImGui item owning left, right and
	// middle presses: Alt+LMB orbit, MMB pan and RMB+WASD/QE fly run while their button holds it, and the first press on
	// an unfocused view focuses it in that frame; the wheel zooms the hovered view. The gizmo shows for the displayed
	// selection whenever the image is shown and takes presses over its handles; W/E/R/X and F apply to the focused,
	// hovered view. All of it honors input capture (text input, popups, drags, other active items). Picking goes through
	// EditorViewportHost, never a direct GPU wait, and only on an image rendered with picking; without a scene the view
	// shows its placeholder. Grid/icons/colliders/debug view reflect EditorViewportState. Dropped mesh/prefab/HDR invokes
	// commands: entity, instance, Environment.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class SceneViewportPanel
	{
	public:
		// The ImGuiWindowFlags the host passes to the Begin of the window Draw renders into.
		[[nodiscard]] static int GetWindowFlags();
		// Draws into the current window (the caller's Begin); never begins or closes a window of its own.
		[[nodiscard]] Status Draw(EditorPanelContext& context);
		// The toolbar's gizmo operation, space and snapping, which W/E/R/X also change.
		[[nodiscard]] const GizmoSettings& GetGizmoSettings() const { return m_GizmoSettings; }
	private:
		[[nodiscard]] Status QueueAction(EditorPanelContext& context, std::string_view method, const Json& params);
		[[nodiscard]] Status CollectActions(EditorPanelContext& context);
		[[nodiscard]] Status FrameSelection(EditorPanelContext& context);
		[[nodiscard]] Status AcceptAssetDrop(EditorPanelContext& context);
	private:
		GizmoSettings m_GizmoSettings{};
		uint64_t m_ClickSequence = 0;
		std::vector<uint64_t> m_ActionTickets{};
		std::vector<UUID> m_DragSelection{};
		bool m_DragInPlay = false;
		std::string m_LastError{};
	};

}
