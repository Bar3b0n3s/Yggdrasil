#pragma once

#include "Engine/Core/Result.h"

namespace Engine {

	struct EditorPanelContext;

	// Primary-camera image at free or selected fixed resolution, letterboxed. Game input is click-to-capture: a press on
	// the image while playing captures it, and it stays captured while the window keeps focus, the image is held or the
	// game has locked the cursor; Escape, focus moving elsewhere, native blur and the end of play release it (releasing
	// the held input of an ordinary session). The host maps captured cursor positions into the game image's pixels.
	// Shows the no-camera state and the lockstep owner's 'Agent controls time' banner; game render quality follows project
	// settings, never scene-view gizmos/grid.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class GameViewportPanel
	{
	public:
		// The ImGuiWindowFlags the host passes to the Begin of the window Draw renders into.
		[[nodiscard]] static int GetWindowFlags();
		// Draws into the current window (the caller's Begin); never begins or closes a window of its own.
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	};

}
