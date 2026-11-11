#pragma once

#include "EditorCore/Ui/UiAuthoring.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/UiRectComponent.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Engine {

	struct EditorPanelContext;

	// The UI authoring overlay (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decision 34), drawn over the scene
	// view's UI preview only (UiPreview.h), over the scene the scene view shows (the play scene while the editor shows it,
	// M20 decisions 13 and 23), laid out at the preview image's scene resolution (ComputeUiLayout, the layout the image was
	// extracted with: the image's size, or a PixelPerfectCamera's reference resolution, since M22's landing):
	//   - every laid-out element's outline (Hidden elements left out), and the selected element's rectangle with its eight
	//     handles when one element is selected in the shown scene;
	//   - a click (a press and release without a drag) selects the topmost element at the point, drawn or not
	//     (PickUiElement), or clears the selection over no element, with edit.select;
	//   - a press on a handle of the selected element, or on its body followed by a drag, drags it (FindUiRectHandle,
	//     DragUiRect; the pointer's delta in panel pixels turned into canvas units by the preview's and the element's canvas's
	//     scales), drawn as it goes, and submits one entity.update of its UiRect (Position and Size) at the release, with the
	//     revision the drag started at (ifRevision), so a drag is one undo step and a scene changed meanwhile refuses it.
	//     Escape, a read-only editor or the element leaving the layout cancels the drag without a write.
	// Every action goes through EditorActions, so the overlay needs no parity exemption; ui.tree reports the rectangles it
	// shows. No snapping, guides, alignment, rotation or scale (Backlog §6). Main thread inside ImGui; it keeps only UI state
	// and ids across frames, never an entity or component pointer.
	class UiAuthoringOverlay
	{
	public:
		// Draws the overlay for the preview image placed at `frame` (panel pixels) in the current window, the game view of
		// `imageSize` view (frame) pixels: the UI is laid out at that view's scene resolution and placed, picked and dragged
		// through its output mapping (ComputeGameViewOutputMapping: the identity, or a PixelPerfectCamera's upscale and
		// letterbox, where scene pixel p is frame pixel p * Scale + Offset). Takes the pointer over the frame as one ImGui
		// item (the last item when it returns, so a drop target can follow). `active` reports whether that item holds a
		// press. Errors: a failed action of an earlier frame, once.
		[[nodiscard]] Status Draw(EditorPanelContext& context, const UiPreviewFrame& frame, glm::uvec2 imageSize, bool& active);
	private:
		struct Drag
		{
			UUID Entity{};
			UiRectHandle Handle = UiRectHandle::Move;
			UiRectComponent Start{};
			glm::vec2 StartSize = glm::vec2(0.0f);  // laid-out size in canvas units
			glm::vec2 StartMin = glm::vec2(0.0f);   // laid-out top-left corner in scene pixels
			float CanvasScale = 1.0f;               // scene pixels per canvas unit
			glm::vec2 PressPoint = glm::vec2(0.0f); // panel pixels
			glm::vec2 Pointer = glm::vec2(0.0f);    // the pointer's last valid position, panel pixels
			uint64_t Revision = 0;
			bool Started = false;
		};
		[[nodiscard]] Status Queue(EditorPanelContext& context, std::string_view method, const Json& params);
		[[nodiscard]] Status CollectActions(EditorPanelContext& context);
	private:
		std::optional<Drag> m_Drag{};
		std::optional<glm::vec2> m_Click{}; // a press that selects at its release, panel pixels
		std::vector<uint64_t> m_Tickets{};
	};

}
