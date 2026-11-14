#pragma once

#include "Editor/Tools/TerrainBrush.h"
#include "Editor/Ui/UiAuthoringOverlay.h"
#include "Editor/Viewport/TilemapBrush.h"
#include "EditorCore/Viewport/EditorViewportState.h"
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
	struct EditorViewportImage;

	// Displays the host's scene image and records next-frame geometry. The image is one ImGui item owning left, right and
	// middle presses: Alt+LMB orbit, MMB pan and RMB+WASD/QE fly run while their button holds it, and the first press on
	// an unfocused view focuses it in that frame; the wheel zooms the hovered view. The gizmo shows for the displayed
	// selection whenever the image is shown and takes presses over its handles; W/E/R/X and F apply to the focused,
	// hovered view. All of it honors input capture (text input, popups, drags, other active items). Picking goes through
	// EditorViewportHost, never a direct GPU wait, and only on an image rendered with picking; without a scene the view
	// shows its placeholder. Grid/icons/colliders/debug view reflect EditorViewportState. Dropped mesh/prefab/HDR invokes
	// commands: entity, instance, Environment.
	// M21 (Docs/Decisions/0032-m21-contract.md decision 14): the toolbar's 2D toggle queues viewport.setOptions {mode2D}, the
	// method's own action. In the 2D mode the view pans (MMB, RMB or Alt+LMB) and zooms (the wheel) with
	// EditorCamera::Navigate2D, never orbits or flies, and its Orthographic overlay toggle is off limits (the 2D camera is
	// orthographic). A dropped sprite sheet or texture creates a sprite where the cursor's ray meets z = 0 (SpriteDrop.h; the
	// camera's target when it meets no point of it), through entity.create. With one entity holding a TilemapRenderer whose
	// Tilemap is set selected, the toolbar shows the tile brush's controls and an armed brush takes the image's left and
	// right strokes, each one tilemap.paint (Editor/Viewport/TilemapBrush.h; M21, decision 26). M26 (Docs/Decisions/
	// 0037-m26-contract.md decision 23): with one edit-scene entity holding a Terrain selected, the toolbar shows the terrain
	// brush's controls and an armed brush takes the image's left strokes, each one terrain.edit (Editor/Tools/TerrainBrush.h).
	// M22 (Docs/Decisions/0033-m22-contract.md decisions 33 and 34): the Overlays popup's "UI preview" (EditorViewportOptions::
	// UiPreview) shows the game frame letterboxed in the view (UiPreview.h) under the UI authoring overlay (UiAuthoringOverlay.h),
	// whose item takes the image's presses; the camera's controls, the gizmo, Frame and mesh picking are off meanwhile.
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
		// `rectangle` and `image` are the displayed image's, which a sprite's drop position is read through.
		[[nodiscard]] Status AcceptAssetDrop(EditorPanelContext& context, const EditorViewportRect& rectangle, const EditorViewportImage& image);
	private:
		GizmoSettings m_GizmoSettings{};
		TilemapBrush m_TilemapBrush{};
		TerrainBrush m_TerrainBrush{}; // M26
		uint64_t m_ClickSequence = 0;
		std::vector<uint64_t> m_ActionTickets{};
		std::vector<UUID> m_DragSelection{};
		bool m_DragInPlay = false;
		std::string m_LastError{};
		UiAuthoringOverlay m_UiOverlay{};
	};

}
