#pragma once

#include "Engine/Core/Result.h"

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
	};

}
