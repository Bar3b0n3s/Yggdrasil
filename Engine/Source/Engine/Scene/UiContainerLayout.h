#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

// The UiLayout containers (M22, C22.1, R-UI-LAYOUT; Docs/Decisions/0033-m22-contract.md decision 5): the pure placement of a
// container's children in canvas units, which ComputeUiLayout (Scene/UiLayout.h, stream A) calls for every element with a
// UiLayout and stream D implements. A container's children are the elements whose parent element it is (an entity without
// a UiRect in between is transparent to the layout, UiRectComponent.h), effectively enabled and not ignoring the layout, in
// canonical order. Only IEEE-exact float operations (+, -, *, /, comparisons, floor): the placement is identical in every
// configuration. No rule divides by 0, and every placed size is at least 0.

namespace Engine {

	// One child taking part in its container, in canvas units: its preferred size (its UiRect Size, each component at least 0)
	// and its UiLayoutElement's values (Grow 0, Shrink 1 and MinSize 0 without one).
	struct UiContainerChild
	{
		glm::vec2 Size = glm::vec2(0.0f);
		glm::vec2 MinSize = glm::vec2(0.0f);
		float Grow = 0.0f;
		float Shrink = 1.0f;
	};

	// A child's placed rectangle: its top-left corner relative to the container's rectangle's top-left corner, and its size, in
	// canvas units.
	struct UiContainerPlacement
	{
		glm::vec2 Position = glm::vec2(0.0f);
		glm::vec2 Size = glm::vec2(0.0f);

		bool operator==(const UiContainerPlacement&) const = default;
	};

	// Places `children` (in their order) in a container of `layout` whose rectangle is `containerSize` canvas units; one
	// placement per child, in order. The inner rectangle is the container's less Padding (a negative inner size counts as 0).
	// Main is Direction's axis, cross the other; a child's main and cross sizes are its Size's, raised to MinSize (Stack, Flex).
	//   - Stack: one line, whose cross size is the inner cross size: children one after another along main, Spacing's main
	//     component between two; the run is placed in the inner main size by MainAlignment: offset by none (Start), half
	//     (Center) or all (End) of the space the run leaves, a negative space counting as 0, so an overflowing run starts at the
	//     inner rectangle's start whatever the alignment.
	//   - Flex: lines of children; with Wrap a child that would end beyond the inner main size starts a new line (a line holds at
	//     least one child). In each line the free space (inner main size - the line's sizes - its spacings) is shared out by
	//     Grow when positive (each child gains free * Grow / the line's Grow total; nothing when that total is 0), or taken back
	//     when negative by Shrink times size (each child loses -free * Shrink * size / the line's total of Shrink times size,
	//     never going below its MinSize; what a clamped child keeps is not taken from the others, so such a line may overflow;
	//     when that total is 0, because every child's Shrink or size is 0, no child shrinks and the line overflows); the line
	//     is then placed by MainAlignment as a Stack's run. Lines follow each other across, Spacing's cross component between
	//     two; a line's cross size is its tallest child's (the inner cross size for a single line).
	//   - Grid: cells of CellSize, Columns per row (Horizontal) or column (Vertical), or with Columns 0 as many as fit the inner
	//     main size: floor((inner + spacing) / (cell + spacing)) on the main axis, clamped to 1 to 1024 (Columns' own range), and
	//     1024 when cell + spacing is 0 (every count fits; the quotient is never computed); each child takes its cell's size
	//     whatever its own; the cell block is placed in the inner rectangle by MainAlignment (main) and Start (cross).
	//   - CrossAlignment places each child in its line's cross size (Grid: its cell's): Start, Center, End, or Stretch to the
	//     whole of it (Stack and Flex: never below the child's MinSize). A Stack's or Flex's child larger than its line across
	//     keeps the same formula: Center overflows both sides equally and End overflows the start
	//     (Docs/Decisions/0033-m22-contract.md decision 5).
	// Pure.
	[[nodiscard]] std::vector<UiContainerPlacement> LayoutUiContainer(const UiLayoutComponent& layout, glm::vec2 containerSize,
		std::span<const UiContainerChild> children);

}
