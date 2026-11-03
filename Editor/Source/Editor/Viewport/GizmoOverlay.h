#pragma once

#include "EditorCore/Viewport/EditorViewportState.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <cstdint>

namespace Engine {

	class EditorContext;

	// ImGuizmo adapter only. Main thread, active ImGui frame; controller/context outlive this call. The gizmo is drawn for
	// the displayed selection whenever the view has a usable image, focused or not; manipulation needs the pointer over
	// the image (or an active drag). W/E/R and local/world (X) apply while the view is focused and hovered; Ctrl snaps;
	// ESC, and focus loss during a drag, cancel; keyboard shortcuts are disabled while text input is active.
	// View/projection come from the displayed CameraData; ImGuizmo's projection convention is adapted explicitly. In the scene
	// view's 2D mode (EditorViewportOptions::Mode2D, M21) the handles translate in X and Y, rotate about Z and scale in X and Y.
	// Begins on mouse-down, updates CPU preview, ends on release. Conflict discards preview and reports the reason.
	// `ownsPointer` reports, after this frame's manipulation, whether ImGuizmo hovers a handle or is dragging: the caller
	// then leaves the press to ImGuizmo instead of submitting its own image item (ImGuizmo activates only over no item).
	[[nodiscard]] Status DrawGizmoOverlay(EditorContext& context, GizmoController& controller,
		const CameraData& camera, const EditorViewportRect& rectangle, GizmoSettings& settings, bool& ownsPointer);

	// The ImGuizmo::OPERATION bits DrawGizmoOverlay manipulates `operation` with: every axis in 3D; in the scene view's 2D mode
	// (M21) translation in X and Y, rotation about Z and scale in X and Y. Pure.
	[[nodiscard]] uint32_t GetGizmoOperationMask(GizmoOperation operation, bool mode2D);

}
