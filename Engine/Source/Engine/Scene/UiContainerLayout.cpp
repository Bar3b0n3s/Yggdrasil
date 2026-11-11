#include "EnginePCH.h"
#include "Engine/Scene/UiContainerLayout.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The UiLayout containers (UiContainerLayout.h states every rule; Docs/Decisions/0033-m22-contract.md decision 5). The
// container's axes are indices into its vectors (main Direction's, cross the other), so one routine places Horizontal and
// Vertical containers alike. Stack is Flex's line placement with one line and no growing or shrinking.
//
// Every value is computed in float with +, -, *, / and comparisons (and floor for a fitted Grid's count), in the children's
// order, each operation in the order the header's formulas write it, so a container's placements are identical in every
// configuration (the precise floating-point model, §4.12). The values the rules take as at least 0 (a child's sizes, which its
// caller passes at least 0, and the registry's minimum sizes, spacing, cell sizes and flex weights) are read as at least 0, a
// NaN as 0, so the function stays total for any input: no rule divides by 0, and every placed size is at least 0.

namespace Engine {

	namespace {

		// The container's inner rectangle and gaps on its axes.
		struct UiContainerFrame
		{
			glm::length_t Main = 0;
			glm::length_t Cross = 1;
			glm::vec2 Origin = glm::vec2(0.0f); // the inner rectangle's top-left corner, from the container's
			glm::vec2 Size = glm::vec2(0.0f);   // the inner rectangle's size, each component at least 0
			glm::vec2 Spacing = glm::vec2(0.0f);
		};

		// A Stack's or Flex's child on the container's axes: its size raised to its minimum, then grown or shrunk (Flex).
		struct UiContainerItem
		{
			float Main = 0.0f;
			float Cross = 0.0f;
			float MinMain = 0.0f;
			float MinCross = 0.0f;
			float Grow = 0.0f;
			float Shrink = 0.0f;
		};

		// A line of a Stack or Flex: the items [First, First + Count).
		struct UiContainerLine
		{
			size_t First = 0;
			size_t Count = 0;
		};

		// A child's offset and size across its line (Grid: its cell).
		struct UiCrossPlacement
		{
			float Offset = 0.0f;
			float Size = 0.0f;
		};

	}

	namespace Utils {

		// The largest number of cells a Grid line holds (UiLayoutComponent::Columns' range).
		constexpr uint32_t MaxUiGridCellsPerLine = 1024;

		// A value the rules take as at least 0, read as at least 0; a NaN reads as 0.
		static float NonNegative(float value)
		{
			return value > 0.0f ? value : 0.0f;
		}

		// Where a run of `runSize` starts in `available`: none (Start), half (Center) or all (End) of the space it leaves, a
		// negative space counting as 0, so an overflowing run starts at the start.
		static float AlignRun(UiMainAlignment alignment, float available, float runSize)
		{
			const float space = NonNegative(available - runSize);
			switch (alignment)
			{
				case UiMainAlignment::Start:
					return 0.0f;
				case UiMainAlignment::Center:
					return space * 0.5f;
				case UiMainAlignment::End:
					return space;
			}
			return 0.0f;
		}

		// A child of cross size `size` (raised to `minSize`) in a line of cross size `lineSize`: Start, Center and End offset it
		// by none, half or all of the difference (a child larger than its line overflows it accordingly); Stretch makes it the
		// line's size, never below its minimum.
		static UiCrossPlacement AlignCross(UiCrossAlignment alignment, float lineSize, float size, float minSize)
		{
			switch (alignment)
			{
				case UiCrossAlignment::Start:
					return UiCrossPlacement{ .Offset = 0.0f, .Size = size };
				case UiCrossAlignment::Center:
					return UiCrossPlacement{ .Offset = (lineSize - size) * 0.5f, .Size = size };
				case UiCrossAlignment::End:
					return UiCrossPlacement{ .Offset = lineSize - size, .Size = size };
				case UiCrossAlignment::Stretch:
					return UiCrossPlacement{ .Offset = 0.0f, .Size = std::max(lineSize, minSize) };
			}
			return UiCrossPlacement{ .Offset = 0.0f, .Size = size };
		}

		// A line's run: its items' main sizes and the main spacing between two.
		static float MeasureRun(std::span<const UiContainerItem> items, float spacing)
		{
			float run = 0.0f;
			for (size_t index = 0; index < items.size(); ++index)
			{
				if (index > 0)
					run = run + spacing;
				run = run + items[index].Main;
			}
			return run;
		}

		// Flex: shares a line's free space out by Grow, or takes its overflow back by Shrink times size, never below MinSize.
		static void FlexLine(std::span<UiContainerItem> items, float innerMain, float spacing)
		{
			const float freeSpace = innerMain - MeasureRun(items, spacing);
			if (freeSpace > 0.0f)
			{
				float growTotal = 0.0f;
				for (const UiContainerItem& item : items)
					growTotal = growTotal + item.Grow;
				if (growTotal > 0.0f)
					for (UiContainerItem& item : items)
						item.Main = item.Main + freeSpace * item.Grow / growTotal;
			}
			else if (freeSpace < 0.0f)
			{
				const float overflow = -freeSpace;
				float shrinkTotal = 0.0f;
				for (const UiContainerItem& item : items)
					shrinkTotal = shrinkTotal + item.Shrink * item.Main;
				// A total of 0 (every Shrink or size 0): nothing shrinks, and the line overflows.
				if (shrinkTotal > 0.0f)
					for (UiContainerItem& item : items)
						item.Main = std::max(item.Main - overflow * item.Shrink * item.Main / shrinkTotal, item.MinMain);
			}
		}

		// Stack and Flex: the children raised to their minimum sizes, split into lines (Flex with Wrap), grown or shrunk (Flex),
		// each line's run placed by MainAlignment and each child across its line by CrossAlignment; the lines follow each other
		// across from the inner rectangle's start.
		static void LayoutLines(const UiLayoutComponent& layout, const UiContainerFrame& frame, std::span<const UiContainerChild> children,
			std::span<UiContainerPlacement> placements)
		{
			const bool flex = layout.Kind == UiLayoutKind::Flex;
			const float innerMain = frame.Size[frame.Main];
			const float mainSpacing = frame.Spacing[frame.Main];

			std::vector<UiContainerItem> items;
			items.reserve(children.size());
			for (const UiContainerChild& child : children)
			{
				UiContainerItem item{ .MinMain = NonNegative(child.MinSize[frame.Main]),
					.MinCross = NonNegative(child.MinSize[frame.Cross]),
					.Grow = NonNegative(child.Grow),
					.Shrink = NonNegative(child.Shrink) };
				item.Main = std::max(NonNegative(child.Size[frame.Main]), item.MinMain);
				item.Cross = std::max(NonNegative(child.Size[frame.Cross]), item.MinCross);
				items.push_back(item);
			}

			// With Wrap, a child that would end beyond the inner main size starts a new line; a line holds at least one child.
			std::vector<UiContainerLine> lines;
			if (flex && layout.Wrap)
			{
				UiContainerLine line{ .First = 0, .Count = 1 };
				float lineEnd = items[0].Main;
				for (size_t index = 1; index < items.size(); ++index)
				{
					const float end = lineEnd + mainSpacing + items[index].Main;
					if (end > innerMain)
					{
						lines.push_back(line);
						line = UiContainerLine{ .First = index, .Count = 1 };
						lineEnd = items[index].Main;
					}
					else
					{
						++line.Count;
						lineEnd = end;
					}
				}
				lines.push_back(line);
			}
			else
				lines.push_back(UiContainerLine{ .First = 0, .Count = items.size() });

			float lineStart = 0.0f;
			for (const UiContainerLine& line : lines)
			{
				const std::span<UiContainerItem> lineItems = std::span<UiContainerItem>(items).subspan(line.First, line.Count);
				if (flex)
					FlexLine(lineItems, innerMain, mainSpacing);

				// A single line spans the inner cross size; each of several lines is as tall as its tallest child.
				float lineCross = frame.Size[frame.Cross];
				if (lines.size() > 1)
				{
					lineCross = 0.0f;
					for (const UiContainerItem& item : lineItems)
						lineCross = std::max(lineCross, item.Cross);
				}

				float cursor = frame.Origin[frame.Main] + AlignRun(layout.MainAlignment, innerMain, MeasureRun(lineItems, mainSpacing));
				for (size_t index = 0; index < lineItems.size(); ++index)
				{
					const UiContainerItem& item = lineItems[index];
					const UiCrossPlacement across = AlignCross(layout.CrossAlignment, lineCross, item.Cross, item.MinCross);
					UiContainerPlacement& placement = placements[line.First + index];
					placement.Position[frame.Main] = cursor;
					placement.Position[frame.Cross] = frame.Origin[frame.Cross] + lineStart + across.Offset;
					placement.Size[frame.Main] = item.Main;
					placement.Size[frame.Cross] = across.Size;
					cursor = cursor + item.Main + mainSpacing;
				}
				lineStart = lineStart + lineCross + frame.Spacing[frame.Cross];
			}
		}

		// Grid: the number of cells a line holds: Columns (at most 1024), or with Columns 0 as many as fit the inner main size,
		// 1 to 1024, without dividing by 0.
		static uint32_t GetGridCellsPerLine(const UiLayoutComponent& layout, const UiContainerFrame& frame, glm::vec2 cell)
		{
			if (layout.Columns > 0)
				return std::min(layout.Columns, MaxUiGridCellsPerLine);
			const float spacing = frame.Spacing[frame.Main];
			const float step = cell[frame.Main] + spacing;
			// Cells and gaps of no length: every count fits.
			if (!(step > 0.0f))
				return MaxUiGridCellsPerLine;
			const float fitted = std::floor((frame.Size[frame.Main] + spacing) / step);
			if (!(fitted >= 1.0f))
				return 1;
			if (fitted >= static_cast<float>(MaxUiGridCellsPerLine))
				return MaxUiGridCellsPerLine;
			return static_cast<uint32_t>(fitted);
		}

		// Grid: each child takes the size of its cell, filled along main a line at a time; the cell block is placed by
		// MainAlignment along main and at the start across.
		static void LayoutGrid(const UiLayoutComponent& layout, const UiContainerFrame& frame, std::span<UiContainerPlacement> placements)
		{
			const glm::vec2 cell(NonNegative(layout.CellSize.x), NonNegative(layout.CellSize.y));
			const uint32_t perLine = GetGridCellsPerLine(layout, frame, cell);
			const glm::vec2 step = cell + frame.Spacing;

			// The block's main size: the cells of its fullest line and the gaps between them.
			const size_t used = std::min<size_t>(perLine, placements.size());
			const float block = static_cast<float>(used) * cell[frame.Main] + static_cast<float>(used - 1) * frame.Spacing[frame.Main];
			const float start = frame.Origin[frame.Main] + AlignRun(layout.MainAlignment, frame.Size[frame.Main], block);

			// A child takes its cell's size, so CrossAlignment leaves it filling its cell.
			const UiCrossPlacement across = AlignCross(layout.CrossAlignment, cell[frame.Cross], cell[frame.Cross], 0.0f);
			for (size_t index = 0; index < placements.size(); ++index)
			{
				const size_t mainIndex = index % perLine;
				const size_t crossIndex = index / perLine;
				UiContainerPlacement& placement = placements[index];
				placement.Position[frame.Main] = start + static_cast<float>(mainIndex) * step[frame.Main];
				placement.Position[frame.Cross] = frame.Origin[frame.Cross] + static_cast<float>(crossIndex) * step[frame.Cross] + across.Offset;
				placement.Size[frame.Main] = cell[frame.Main];
				placement.Size[frame.Cross] = across.Size;
			}
		}

	}

	std::vector<UiContainerPlacement> LayoutUiContainer(const UiLayoutComponent& layout, glm::vec2 containerSize,
		std::span<const UiContainerChild> children)
	{
		std::vector<UiContainerPlacement> placements(children.size());
		if (children.empty())
			return placements;

		const UiPadding& padding = layout.Padding;
		UiContainerFrame frame{ .Origin = glm::vec2(padding.Left, padding.Top),
			.Size = glm::vec2(Utils::NonNegative(containerSize.x - padding.Left - padding.Right),
				Utils::NonNegative(containerSize.y - padding.Top - padding.Bottom)),
			.Spacing = glm::vec2(Utils::NonNegative(layout.Spacing.x), Utils::NonNegative(layout.Spacing.y)) };
		if (layout.Direction == UiLayoutDirection::Vertical)
		{
			frame.Main = 1;
			frame.Cross = 0;
		}

		if (layout.Kind == UiLayoutKind::Grid)
			Utils::LayoutGrid(layout, frame, placements);
		else
			Utils::LayoutLines(layout, frame, children, placements);
		return placements;
	}

}
