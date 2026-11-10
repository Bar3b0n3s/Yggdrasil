#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Engine {

	struct EditorPanelContext;

	// The editor's animation preview controls (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 32): a window of its
	// own, "Animation Preview", shown while the active edit scene's selection is one entity holding an Animator, or while a
	// preview of the active scene runs, and never during play (the session poses its Animators itself). Its controls (preview,
	// play and pause, the time slider over the longest clip, stop) submit animation.preview, the preview's one action
	// (EditorCore/Automation/AnimationMethods.h), so the UI previews exactly as automation does; the preview itself is the
	// EditorContext's (EditorCore/Animation/AnimationPreview.h). EditorLayer only calls Draw. Main thread inside ImGui; keeps
	// only presentation state, no entity, component or asset pointer across frames.
	class AnimationPreviewPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		uint64_t m_Ticket = 0;                // the submitted animation.preview, 0 when none is pending
		std::string m_Error{};                // the last request's failure, until the next request
		float m_SliderTime = 0.0f;            // the time slider's value
		bool m_Dragging = false;              // the slider was held on the last frame
		std::optional<float> m_PendingTime{}; // a dragged time not sent yet
	};

}
