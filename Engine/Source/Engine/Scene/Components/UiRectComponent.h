#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "UiRect" (M22, C22.1, R-UI-CORE; Docs/Decisions/0033-m22-contract.md decisions 2 and 4): a UI element's
	// rectangle, placed in its parent's rectangle (the nearest ancestor with a UiRect below its canvas, or the canvas's), in
	// canvas units with x right and y down. With the parent rectangle P (Min, Size):
	//   anchor rectangle A = [P.Min + AnchorMin * P.Size, P.Min + AnchorMax * P.Size]
	//   size              = (A.Max - A.Min) + Size, each component clamped to at least 0
	//   pivot point       = A.Min + Pivot * (A.Max - A.Min) + Position
	//   rectangle         = [pivot point - Pivot * size, pivot point + (1 - Pivot) * size]
	// so with equal anchors Size is the element's size and Position moves its pivot from the anchor point, and with stretched
	// anchors Size adds to the stretched span (negative values inset it). An entity without a UiRect between an element and
	// its parent element is transparent to the layout. A UiLayout container on the parent element places the element instead
	// (UiLayoutComponent.h), unless its UiLayoutElement ignores the layout. Elements draw in canonical entity order: a parent
	// before its children, siblings in their order (painter's order). A UiRect never shares an entity with a UiCanvas (the
	// two components exclude each other).
	struct UiRectComponent
	{
		glm::vec2 AnchorMin = glm::vec2(0.5f); // each component 0 to 1, at most AnchorMax's
		glm::vec2 AnchorMax = glm::vec2(0.5f); // each component 0 to 1
		glm::vec2 Pivot = glm::vec2(0.5f);     // normalized to the element: (0, 0) its top-left corner, (1, 1) its bottom-right
		glm::vec2 Position = glm::vec2(0.0f);  // canvas units, x right and y down
		glm::vec2 Size = glm::vec2(100.0f);    // canvas units, added to the anchor rectangle's size
		// Clips the element's descendants to its rectangle (the Ui pass's scissor, intersected with every clipping ancestor's);
		// the element itself is not clipped by its own rectangle.
		bool ClipChildren = false;
	};

}
