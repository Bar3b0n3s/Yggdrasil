#pragma once

#include "EditorCore/Viewport/EditorViewportState.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"

namespace Engine {

	class EditorContext;

	// ImGuizmo adapter only. Main thread, active ImGui frame; controller/context outlive this call. W/E/R and local/world
	// mode, Ctrl snaps, ESC/focus loss cancels; keyboard shortcuts disabled while text input is active.
	// View/projection come from the displayed CameraData; ImGuizmo's projection convention is adapted explicitly.
	// Begins on mouse-down, updates CPU preview, ends on release. Conflict discards preview and reports the reason.
	[[nodiscard]] Status DrawGizmoOverlay(EditorContext& context, GizmoController& controller,
		const CameraData& camera, const EditorViewportRect& rectangle, GizmoSettings& settings);

}
