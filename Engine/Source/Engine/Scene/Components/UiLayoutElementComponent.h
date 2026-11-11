#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "UiLayoutElement" (M22, C22.1, R-UI-LAYOUT; Docs/Decisions/0033-m22-contract.md decisions 2 and 5): how a
	// UI element takes part in its parent element's UiLayout container. Without one an element takes part with Grow 0,
	// Shrink 1 and no minimum size. Ignored when the parent element has no UiLayout.
	struct UiLayoutElementComponent
	{
		// The element keeps its own UiRect placement in the parent element's rectangle and takes no space in the container.
		bool IgnoreLayout = false;
		float Grow = 0.0f;                   // Flex: its share of the main axis's free space, 0 to 1000
		float Shrink = 1.0f;                 // Flex: its share, times its preferred size, of the main axis's overflow, 0 to 1000
		glm::vec2 MinSize = glm::vec2(0.0f); // Stack and Flex: the size it never goes below, canvas units, 0 to 1,000,000
	};

}
