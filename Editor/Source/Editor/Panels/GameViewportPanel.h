#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	struct EditorPanelContext;

	// Primary-camera image at free or selected fixed resolution, letterboxed; input only over focused image.
	// Shows no-camera state and the lockstep owner's 'Agent controls time' banner. Focus loss releases all held input;
	// game render quality follows project settings, never scene-view gizmos/grid.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class GameViewportPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	};

}
