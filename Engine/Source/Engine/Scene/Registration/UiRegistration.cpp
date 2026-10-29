#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <utility>

// The Ui category (M22, C22.1; Docs/Decisions/0033-m22-contract.md decision 2): canvases, element rectangles, images, text and
// layout containers, laid out and hit-tested in scene pixels (Architecture §5.3, §8.3). Lengths are canvas units (a canvas
// maps them to scene pixels by its scale); colours are linear with straight alpha. ADR 0028's phase-2 ledger puts a phase-2
// milestone's components in a category of its own after Scripting, appended in landing order.

namespace Engine {

	namespace Utils {

		// The largest canvas length a field takes, in canvas units. Scaled to scene pixels (up to 100 per unit by ScaleFactor,
		// up to the scene's height per unit by MatchHeight) and summed down a hierarchy, a rectangle may pass float's exact
		// integer range (2^24) and int32's: such values are inexact but deterministic, and ComputeUiClipRect clamps what it
		// converts to whole pixels (Scene/UiLayout.h).
		constexpr double MaxUiLength = 1000000.0;
		// The canvas scale range (UiCanvas.ScaleFactor), and MatchHeight's reference height.
		constexpr double MinUiScaleFactor = 0.01;
		constexpr double MaxUiScaleFactor = 100.0;
		constexpr double MaxUiReferenceHeight = 100000.0;
		// A sheet holds at most 65,536 frames (MaxSpriteSheetFrames, Asset/SpriteSheetData.h).
		constexpr double MaxUiImageFrame = 65535.0;
		constexpr double MinUiBorderScale = 0.01;
		constexpr double MaxUiBorderScale = 1000.0;
		// The text size range, as the Text component's (pixels per em there, canvas units per em here).
		constexpr double MinUiTextSize = 1.0;
		constexpr double MaxUiTextSize = 100000.0;
		constexpr double MinUiLineSpacing = 0.1;
		constexpr double MaxUiLineSpacing = 10.0;
		constexpr double MaxUiGridColumns = 1024.0;
		constexpr double MaxUiFlexWeight = 1000.0;

		static FieldMeta UiColorRange()
		{
			FieldMeta meta;
			meta.Min = 0.0;
			meta.Max = 1.0;
			return meta;
		}

		// An anchor rectangle runs from AnchorMin to AnchorMax on each axis (equal anchors are a point).
		static void ValidateUiRect(const UiRectComponent& rect, ValidationContext& context)
		{
			if (rect.AnchorMin.x > rect.AnchorMax.x || rect.AnchorMin.y > rect.AnchorMax.y)
				context.Error("AnchorMin", "must not exceed AnchorMax on either axis");
		}

		static void MakeUiRectValid(UiRectComponent& rect, Random& /*random*/)
		{
			if (rect.AnchorMin.x > rect.AnchorMax.x)
				std::swap(rect.AnchorMin.x, rect.AnchorMax.x);
			if (rect.AnchorMin.y > rect.AnchorMax.y)
				std::swap(rect.AnchorMin.y, rect.AnchorMax.y);
		}

	}

	void RegisterUiComponents(TypeRegistry& registry)
	{
		registry.Enum<UiScaleMode>("UiScaleMode", "How a UI canvas maps its units to scene pixels.")
			.Entry(UiScaleMode::ConstantPixelSize, "ConstantPixelSize", "ScaleFactor scene pixels per canvas unit, whatever the scene resolution.")
			.Entry(UiScaleMode::MatchHeight, "MatchHeight", "The canvas is ReferenceHeight units high, so it scales with the scene's height.");

		registry.Enum<UiImageType>("UiImageType", "How a UI image fills its element's rectangle.")
			.Entry(UiImageType::Simple, "Simple", "The sprite frame is stretched over the rectangle.")
			.Entry(UiImageType::Sliced, "Sliced", "The frame's 9-slice borders keep their size; the edges and the centre stretch.");

		registry.Enum<UiImageFilter>("UiImageFilter", "How a UI image samples its texture.")
			.Entry(UiImageFilter::Point, "Point", "The nearest texel: pixel art, and a 1:1 image reproduces its texels exactly.")
			.Entry(UiImageFilter::Linear, "Linear", "Bilinear filtering between texels.");

		registry.Enum<UiTextVerticalAlignment>("UiTextVerticalAlignment", "Where a UI text block sits in its element's rectangle.")
			.Entry(UiTextVerticalAlignment::Top, "Top", "The first line starts at the rectangle's top edge.")
			.Entry(UiTextVerticalAlignment::Middle, "Middle", "The block is centred vertically in the rectangle.")
			.Entry(UiTextVerticalAlignment::Bottom, "Bottom", "The last line ends at the rectangle's bottom edge.");

		registry.Enum<UiLayoutKind>("UiLayoutKind", "How a UI layout container places its children.")
			.Entry(UiLayoutKind::Stack, "Stack", "One after another along Direction, each at its own size.")
			.Entry(UiLayoutKind::Grid, "Grid", "In cells of CellSize, Columns cells per row (Horizontal) or per column (Vertical).")
			.Entry(UiLayoutKind::Flex, "Flex", "A stack whose children grow into free space and shrink when they overflow, wrapping with Wrap.");

		registry.Enum<UiLayoutDirection>("UiLayoutDirection", "The main axis of a UI layout container.")
			.Entry(UiLayoutDirection::Horizontal, "Horizontal", "Children follow each other from left to right.")
			.Entry(UiLayoutDirection::Vertical, "Vertical", "Children follow each other from top to bottom.");

		registry.Enum<UiMainAlignment>("UiMainAlignment", "Where a container's children sit along its main axis when they do not fill it.")
			.Entry(UiMainAlignment::Start, "Start", "At the start: the left edge (Horizontal) or the top (Vertical).")
			.Entry(UiMainAlignment::Center, "Center", "Centred along the main axis.")
			.Entry(UiMainAlignment::End, "End", "At the end: the right edge (Horizontal) or the bottom (Vertical).");

		registry.Enum<UiCrossAlignment>("UiCrossAlignment", "Where each child of a container sits across its main axis.")
			.Entry(UiCrossAlignment::Start, "Start", "At the start of its line: the top (Horizontal) or the left edge (Vertical).")
			.Entry(UiCrossAlignment::Center, "Center", "Centred across the main axis.")
			.Entry(UiCrossAlignment::End, "End", "At the end of its line: the bottom (Horizontal) or the right edge (Vertical).")
			.Entry(UiCrossAlignment::Stretch, "Stretch", "Stretched to fill its line's cross size (a Grid child: its cell's).");

		registry.Struct<UiPadding>("UiPadding", "Insets of a UI layout container's rectangle, in canvas units.")
			.Field("Left", &UiPadding::Left, "The inset from the left edge.", { .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" })
			.Field("Top", &UiPadding::Top, "The inset from the top edge.", { .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" })
			.Field("Right", &UiPadding::Right, "The inset from the right edge.", { .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" })
			.Field("Bottom", &UiPadding::Bottom, "The inset from the bottom edge.", { .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" });

		RegisterComponent<UiCanvasComponent>(registry, "UiCanvas",
			"The root of a UI tree, laid out over the whole scene image in scene pixels and drawn after the game's text.")
			.Category("Ui")
			.Version(1)
			.Excludes<UiRectComponent>()
			.Field("SortOrder", &UiCanvasComponent::SortOrder, "Canvases draw in ascending SortOrder, ties in entity order; a later canvas draws over earlier ones.")
			.Field("ScaleMode", &UiCanvasComponent::ScaleMode, "How canvas units map to scene pixels.")
			.Field("ScaleFactor", &UiCanvasComponent::ScaleFactor, "ConstantPixelSize: the scene pixels per canvas unit.",
				{ .Min = Utils::MinUiScaleFactor, .Max = Utils::MaxUiScaleFactor })
			.Field("ReferenceHeight", &UiCanvasComponent::ReferenceHeight,
				"MatchHeight: the canvas's height in canvas units, so one unit is the scene's height / ReferenceHeight scene pixels.",
				{ .Min = 1.0, .Max = Utils::MaxUiReferenceHeight, .Unit = "units" })
			.Field("PixelSnap", &UiCanvasComponent::PixelSnap,
				"Rounds every element's corners to whole scene pixels, so images and clips land on pixel corners.");

		RegisterComponent<UiRectComponent>(registry, "UiRect",
			"A UI element's rectangle in its parent's (or its canvas's), in canvas units, x right and y down.")
			.Category("Ui")
			.Version(1)
			.Excludes<UiCanvasComponent>()
			.Field("AnchorMin", &UiRectComponent::AnchorMin,
				"The anchor rectangle's top-left corner, normalized in the parent's rectangle: (0, 0) its top-left corner, (1, 1) its bottom-right.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("AnchorMax", &UiRectComponent::AnchorMax,
				"The anchor rectangle's bottom-right corner, normalized in the parent's rectangle; equal to AnchorMin for a fixed-size element.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Pivot", &UiRectComponent::Pivot,
				"The point of the element that Position places, normalized to the element: (0, 0) its top-left corner, (1, 1) its bottom-right.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Position", &UiRectComponent::Position,
				"The pivot's offset from its point in the anchor rectangle, in canvas units, x right and y down.",
				{ .Min = -Utils::MaxUiLength, .Max = Utils::MaxUiLength, .Unit = "units" })
			.Field("Size", &UiRectComponent::Size,
				"The element's size in canvas units, added to the anchor rectangle's size (negative values inset stretched anchors); "
				"a container's child takes it as its preferred size.",
				{ .Min = -Utils::MaxUiLength, .Max = Utils::MaxUiLength, .Unit = "units" })
			.Field("ClipChildren", &UiRectComponent::ClipChildren, "Whether the element's descendants are clipped to its rectangle.")
			.Validate(&Utils::ValidateUiRect)
			.Generate(&Utils::MakeUiRectValid);

		RegisterComponent<UiImageComponent>(registry, "UiImage", "Draws a sprite sheet frame, or a plain colour, over a UI element's rectangle.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Field("Sheet", &UiImageComponent::Sheet, "The sprite sheet whose frame is drawn; null draws a plain rectangle of Color.")
			.Field("Frame", &UiImageComponent::Frame, "The index of the frame in the sheet's Frames; a frame the sheet lacks draws nothing.",
				{ .Min = 0.0, .Max = Utils::MaxUiImageFrame })
			.ColorField("Color", &UiImageComponent::Color, "The linear tint multiplied with the frame's texels, with opacity in the fourth component.",
				Utils::UiColorRange())
			.Field("Type", &UiImageComponent::Type, "Whether the frame stretches or keeps its 9-slice borders.")
			.Field("BorderScale", &UiImageComponent::BorderScale, "Sliced: the canvas units each border texel takes.",
				{ .Min = Utils::MinUiBorderScale, .Max = Utils::MaxUiBorderScale })
			.Field("Filter", &UiImageComponent::Filter, "How the frame's texture is sampled.");

		RegisterComponent<UiTextComponent>(registry, "UiText", "Draws text in a UI element's rectangle, wrapped and aligned in it.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Field("Text", &UiTextComponent::Text, "The UTF-8 text; a line feed starts a new line. Scripts read localized strings with Loc.Get.")
			.Field("Font", &UiTextComponent::Font, "The font; null uses the default font.")
			.Field("Size", &UiTextComponent::Size, "The em size in canvas units.", { .Min = Utils::MinUiTextSize, .Max = Utils::MaxUiTextSize, .Unit = "units" })
			.ColorField("Color", &UiTextComponent::Color, "The linear text colour with opacity in the fourth component.", Utils::UiColorRange())
			.Field("Alignment", &UiTextComponent::Alignment, "How each line is aligned within the rectangle's width.")
			.Field("VerticalAlignment", &UiTextComponent::VerticalAlignment, "Where the text block sits in the rectangle's height.")
			.Field("Wrap", &UiTextComponent::Wrap, "Whether lines break at spaces to fit the rectangle's width.")
			.Field("LineSpacing", &UiTextComponent::LineSpacing, "Multiplies the font's line height.",
				{ .Min = Utils::MinUiLineSpacing, .Max = Utils::MaxUiLineSpacing });

		RegisterComponent<UiLayoutComponent>(registry, "UiLayout",
			"A container that places its children's rectangles in a stack, a grid or a flexible row or column.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Field("Kind", &UiLayoutComponent::Kind, "How the children are placed.")
			.Field("Direction", &UiLayoutComponent::Direction, "The main axis the children follow.")
			.Field("Padding", &UiLayoutComponent::Padding, "Insets of the container's rectangle that the children are placed in.")
			.Field("Spacing", &UiLayoutComponent::Spacing, "The gaps between children, lines and cells along x and y, in canvas units.",
				{ .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" })
			.Field("MainAlignment", &UiLayoutComponent::MainAlignment, "Where the children sit along the main axis when they do not fill it.")
			.Field("CrossAlignment", &UiLayoutComponent::CrossAlignment, "Where each child sits across the main axis.")
			.Field("Columns", &UiLayoutComponent::Columns,
				"Grid: the cells per row (Horizontal) or per column (Vertical); 0 fits as many as the container holds.",
				{ .Min = 0.0, .Max = Utils::MaxUiGridColumns })
			.Field("CellSize", &UiLayoutComponent::CellSize, "Grid: the size of each cell in canvas units.",
				{ .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" })
			.Field("Wrap", &UiLayoutComponent::Wrap, "Flex: whether a child that does not fit the line starts a new line.");

		RegisterComponent<UiLayoutElementComponent>(registry, "UiLayoutElement",
			"How a UI element takes part in its parent's layout container: flex weights, a minimum size, or none.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Field("IgnoreLayout", &UiLayoutElementComponent::IgnoreLayout,
				"Whether the element keeps its own placement and takes no space in the container.")
			.Field("Grow", &UiLayoutElementComponent::Grow, "Flex: the element's share of the main axis's free space.", { .Min = 0.0, .Max = Utils::MaxUiFlexWeight })
			.Field("Shrink", &UiLayoutElementComponent::Shrink, "Flex: the element's share, times its preferred size, of the main axis's overflow.",
				{ .Min = 0.0, .Max = Utils::MaxUiFlexWeight })
			.Field("MinSize", &UiLayoutElementComponent::MinSize, "The size the element never goes below in a Stack or Flex container, in canvas units.",
				{ .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" });
	}

}
