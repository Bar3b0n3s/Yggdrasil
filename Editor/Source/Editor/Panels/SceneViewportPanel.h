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

	// Displays the host's scene image and records next-frame geometry. Camera orbit/pan/fly/zoom and W/E/R gizmos
	// honor input capture. Picking goes through EditorViewportHost, never a direct GPU wait. Grid/icons/colliders/debug
	// view reflect EditorViewportState. Dropped mesh/prefab/HDR invokes commands: entity, instance, Environment.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class SceneViewportPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
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
