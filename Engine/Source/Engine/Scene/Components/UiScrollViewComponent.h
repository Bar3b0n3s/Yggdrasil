#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "UiScrollView" (M22, C22.2, R-UI-SCROLL; Docs/Decisions/0033-m22-contract.md decisions 21, 24 and 26): a
	// widget whose rectangle is a window onto Content, an element below it whose rectangle moves by the scroll position.
	//   - Layout (Scene/UiLayout.h): the scroll view clips its descendants to its rectangle, as UiRect::ClipChildren does,
	//     whatever ClipChildren says. Content is laid out as usual, then moved, with its subtree, by -offset canvas units,
	//     where offset is ScrollPosition clamped per axis to [0, max(0, content size - view size)] (the sizes of the two
	//     laid-out rectangles in canvas units; an axis that does not scroll has offset 0): x moves the content left, y up.
	//     The content's own size never follows its children (a rectangle never follows its content, decision 6), so a
	//     list's content element is given its height. A Content that is null or not an element below the scroll view
	//     leaves nothing to scroll (project.validate's UI_WIDGET_PART_INVALID).
	//   - Pointer: the wheel over the scroll view or anything below it scrolls the innermost such scroll view by
	//     ScrollStep per step (a wheel step up scrolls toward the top); a press on the scroll view itself (its nearest
	//     widget, so not on a button inside it) drags the content with the pointer until the release.
	//   - Keyboard and gamepad: when focus moves into the content, the scroll view scrolls the least that shows the focused
	//     element's rectangle (its top-left corner first when it is larger than the view). A Focusable scroll view takes
	//     focus itself, and while focused the directions along its scrolling axes scroll it by ScrollStep (the other two
	//     move focus).
	// Interaction writes ScrollPosition clamped as read, raising OnUiScroll with the new position. Requires a UiRect;
	// excludes UiButton, UiToggle and UiSlider.
	struct UiScrollViewComponent
	{
		bool Interactable = true; // as UiButtonComponent::Interactable (wheel, drag and focus)
		bool Focusable = false;   // as UiButtonComponent::Focusable; off by default, since its content's widgets take focus
		UUID Content{};           // EntityRef: the element below the scroll view that scrolls
		bool Horizontal = false;  // whether x scrolls
		bool Vertical = true;     // whether y scrolls
		// Canvas units the content is moved left (x) and up (y), each 0 to 1,000,000; read clamped to the scroll range.
		glm::vec2 ScrollPosition = glm::vec2(0.0f);
		float ScrollStep = 40.0f; // canvas units per wheel step and per navigation press, 0 to 1,000,000
	};

}
