#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// Registry enum "UiLayoutKind": how a container places its children.
	enum class UiLayoutKind : uint8_t
	{
		Stack, // one after another along Direction, each at its own size
		Grid,  // in cells of CellSize, filling a row (Horizontal) or a column (Vertical) of Columns cells before the next
		Flex   // a stack whose children grow into free space and shrink when they overflow (UiLayoutElement), wrapping with Wrap
	};

	// Registry enum "UiLayoutDirection": the container's main axis.
	enum class UiLayoutDirection : uint8_t
	{
		Horizontal, // left to right
		Vertical    // top to bottom
	};

	// Registry enum "UiMainAlignment": where the children sit along the main axis when they do not fill it.
	enum class UiMainAlignment : uint8_t
	{
		Start,
		Center,
		End
	};

	// Registry enum "UiCrossAlignment": where each child sits across the main axis.
	enum class UiCrossAlignment : uint8_t
	{
		Start,
		Center,
		End,
		Stretch // fills the line's cross size (Grid: its cell's)
	};

	// Registry struct "UiPadding": insets of a container's rectangle, in canvas units, each 0 to 1,000,000.
	struct UiPadding
	{
		float Left = 0.0f;
		float Top = 0.0f;
		float Right = 0.0f;
		float Bottom = 0.0f;

		bool operator==(const UiPadding&) const = default;
	};

	// Registry name "UiLayout" (M22, C22.1, R-UI-LAYOUT; Docs/Decisions/0033-m22-contract.md decisions 2 and 5): a container
	// that places its children's rectangles inside its own rectangle less Padding, in canvas units, replacing their UiRect
	// anchors, pivot and position (Scene/UiContainerLayout.h has every rule). A child's preferred size is its UiRect
	// Size; its UiLayoutElement adds a minimum size, flex weights or opts out. Its children are the elements whose parent
	// element it is (an entity without a UiRect in between is transparent to the layout, so a grouping entity's elements are
	// placed as the container's own), effectively enabled, in canonical order; the container's own size never follows its
	// content.
	struct UiLayoutComponent
	{
		UiLayoutKind Kind = UiLayoutKind::Stack;
		UiLayoutDirection Direction = UiLayoutDirection::Vertical;
		UiPadding Padding{};
		glm::vec2 Spacing = glm::vec2(0.0f); // the gaps between children (and between Flex lines or Grid cells), x and y, 0 to 1,000,000
		UiMainAlignment MainAlignment = UiMainAlignment::Start;
		UiCrossAlignment CrossAlignment = UiCrossAlignment::Start;
		// Grid: cells per row (Horizontal) or per column (Vertical), 1 to 1024; 0 fits as many as the inner size holds (1 to
		// 1024).
		uint32_t Columns = 0;
		glm::vec2 CellSize = glm::vec2(100.0f); // Grid: each cell's size, 0 to 1,000,000
		bool Wrap = false;                      // Flex: a child that does not fit the line's remaining space starts a new line
	};

}
