#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Scene/UiLayout.h"

#include <glm/glm.hpp>

// The play session's UI (M22, C22.1, R-UI-CORE; Docs/Decisions/0033-m22-contract.md decisions 3 and 4): the two layouts of
// the session's scene that the step order keeps (ADR 0028's phase-2 ledger, PlaySessionPhase):
//   - the step's layout, made by the UiLayout phase at the end of every fixed step (after every phase that can change UI data);
//   - the frame's layout, made by the FrameUiLayout phase after FrameTransformUpdate and before RenderExtraction, so UI that
//     OnUpdate or OnLateUpdate wrote is laid out as the frame draws it.
// The current layout (GetLayout) is the later of the two: the frame's when a frame phase ran since the last step's layout,
// the step's own when none did. W22.2's UiInput hit-tests it, and ui.tree reads it for the play scene, so a hit test depends
// only on the state, never on how many frames ran between two ticks. The session also runs LayoutStep, outside the phase
// list, at the end of PlaySession::Activate and right after a Scene.Load or Scene.Reload replaces its scene (each after the
// scene's startup callbacks), so the current layout always describes the session's current scene: never a replaced scene's,
// and empty only before Activate. Both layouts are at the session's scene resolution
// (the view's size in W22.1; the later lander of M21 and M22 makes it the scene resolution M21's PixelPerfectCamera sets).
// Layouts are derived data: they never reach the state hash (W22.2's ui hash block holds the interaction state only).
// Owned by the PlaySession (GetUi); runs in Play and Simulate alike. Main thread; not copyable.

namespace Engine {

	class Scene;

	class UiSystem
	{
	public:
		UiSystem() = default;
		~UiSystem() = default;

		UiSystem(const UiSystem&) = delete;
		UiSystem& operator=(const UiSystem&) = delete;

		// The UiLayout phase, the session's start and its scene replacements (see the file comment): ComputeUiLayout(scene,
		// sceneSize) becomes the step's layout and the current one.
		void LayoutStep(const Scene& scene, glm::uvec2 sceneSize);
		// The FrameUiLayout phase: ComputeUiLayout(scene, sceneSize) becomes the frame's layout and the current one.
		void LayoutFrame(const Scene& scene, glm::uvec2 sceneSize);

		// The current layout (see the file comment); an empty table before the first layout (a session's: its Activate).
		[[nodiscard]] const UiLayoutTable& GetLayout() const;
		[[nodiscard]] const UiLayoutTable& GetStepLayout() const { return m_StepLayout; }
		[[nodiscard]] const UiLayoutTable& GetFrameLayout() const { return m_FrameLayout; }
		// True when the frame's layout is the current one (a FrameUiLayout ran after the last UiLayout).
		[[nodiscard]] bool IsFrameLayoutCurrent() const { return m_FrameLayoutCurrent; }
	private:
		UiLayoutTable m_StepLayout{};
		UiLayoutTable m_FrameLayout{};
		bool m_FrameLayoutCurrent = false;
	};

}
