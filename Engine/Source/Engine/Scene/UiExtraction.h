#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scene/UiLayout.h"

// The UI's render extraction (M22, C22.1, R-UI-RENDER and R-UI-AUTO; Docs/Decisions/0033-m22-contract.md decisions 7 and
// 9): from a laid-out scene to the snapshot's UI draw list (RenderSnapshot::Ui, Renderer/UiDrawList.h). ExtractRenderSnapshot
// (Scene/RenderExtraction.h) lays the scene out at the snapshot's scene resolution (ComputeUiLayout of the scene resolution
// of the snapshot's Output settings, the view's size by default) and appends the draw items with AppendUiDrawItems; the
// capture annotations append theirs with AppendUiRenderAnnotations from AppendRenderAnnotations (Scene/RenderAnnotations.h).
// Pure functions of the scene's components (no asset access: sheets and fonts are named by handle, the Ui pass resolves
// them), in the table's order, so a deterministic scene gives an identical draw list in every configuration. Main thread.

namespace Engine {

	class Scene;

	// Appends to `list.Items` the draw items of `table`'s visible elements (UiLayoutNode::Visible), in the table's order (its
	// canvases in draw order, each canvas's elements in canonical order): for each element its UiImage, then its UiText when
	// its Text is not empty, each with the element's rectangle, clip and entity, the canvas's scale applied to the lengths
	// (UiText::Size to FontSize, UiImage::BorderScale to scene pixels per border texel) and the enums mapped to the draw
	// list's (TextAlignment Left, Center and Right to Start, Center and End; UiTextVerticalAlignment Top, Middle and Bottom
	// likewise). `table` is a layout of `scene` (ComputeUiLayout); an element the scene no longer holds is skipped. Pure.
	// C22.2's theme (Docs/Decisions/0033-m22-contract.md decision 25; stream C):
	//   - the image of a widget's element (UiButton, UiToggle, UiSlider, UiScrollView) has its Color multiplied, component by
	//     component, by its canvas's UiTheme tint of the widget's state: Disabled when not Interactable, else, from
	//     `interaction` (the session's state; null outside a play session, every widget then Normal), Pressed when it is
	//     the state's Pressed, Focused when Focused, Hovered when Hovered, else Normal;
	//   - a text whose Font is null takes its canvas's Font (the canvas the element belongs to; still null when the canvas
	//     names none, which the Ui pass draws with the default font).
	void AppendUiDrawItems(const Scene& scene, const UiLayoutTable& table, UiDrawList& list, const UiInteractionState* interaction = nullptr);

	// The UI's capture annotations (viewport.screenshot's annotations, §8.13): with snapshot.Annotations.Bounds, an Outline in
	// yellow (1, 1, 0, 1) around every element that has a draw item in snapshot.Ui; with a label mode other than None, a
	// Label in white of the elements it selects among them (All: every one; Selected: those in snapshot.SelectedEntities;
	// Explicit: those in Annotations.LabelEntities), the entity's name and the first six hex digits of its id, as the entity
	// labels read. Appended to snapshot.Ui.Annotations in the items' order, one annotation per element and kind. Pure.
	void AppendUiRenderAnnotations(const Scene& scene, RenderSnapshot& snapshot);

}
