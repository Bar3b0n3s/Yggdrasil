#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"
#include "Engine/Scene/UiTextInput.h"

#include <utility>

// The Ui category (M22, C22.1; Docs/Decisions/0033-m22-contract.md decision 2): canvases, element rectangles, images, text and
// layout containers, laid out and hit-tested in scene pixels (Architecture §5.3, §8.3). Lengths are canvas units (a canvas
// maps them to scene pixels by its scale); colours are linear with straight alpha. ADR 0028's phase-2 ledger puts a phase-2
// milestone's components in a category of its own after Scripting, appended in landing order. C22.2 (decision 21) appends
// the four widgets (UiButton, UiToggle, UiSlider, UiScrollView) after UiLayoutElement; the fields it adds to UiCanvas (Font,
// Theme and DefaultFocus after PixelSnap, with the UiTheme struct: stream C) and to UiImage and UiText (RaycastTarget:
// stream A) are registered by their streams, and W22.2's integration regenerates FeatureTest's UI scenes with them. The
// components are unlanded, so they stay at version 1. C22.3 (decisions 45 and 49) appends UiTextInput, a fifth widget,
// after UiScrollView; the fields it adds to UiCanvas (RenderMode, WorldWidth, WorldHeight and PixelsPerUnit after
// DefaultFocus, with the UiCanvasRenderMode enum: stream W) are registered by their stream, with FeatureTest's
// regeneration.

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

		// C22.2 (decision 21): a slider's range is not empty. Its Value may lie outside it: readers clamp it
		// (UiSliderComponent.h), so a script that moves the range and then the value never fails in between.
		static void ValidateUiSlider(const UiSliderComponent& slider, ValidationContext& context)
		{
			if (!(slider.Min < slider.Max))
				context.Error("Min", "must be below Max");
		}

		static void MakeUiSliderValid(UiSliderComponent& slider, Random& /*random*/)
		{
			if (slider.Min > slider.Max)
				std::swap(slider.Min, slider.Max);
			if (slider.Min < slider.Max)
				return;
			// Equal ends: widen the range by one unit on the side that stays inside the fields' range.
			if (slider.Max - 1.0f >= static_cast<float>(-MaxUiLength))
				slider.Min = slider.Max - 1.0f;
			else
				slider.Max = slider.Min + 1.0f;
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

		// C22.2 (decision 25): a canvas's theme.
		registry
			.Struct<UiTheme>("UiTheme",
				"The state tints of a canvas's widgets: each multiplies the colour of a widget element's own UiImage in that state.")
			.ColorField("Normal", &UiTheme::Normal, "The tint of an interactable widget that is neither hovered, pressed nor focused.",
				Utils::UiColorRange())
			.ColorField("Hovered", &UiTheme::Hovered, "The tint of a widget under the pointer.", Utils::UiColorRange())
			.ColorField("Pressed", &UiTheme::Pressed, "The tint of a widget the pointer presses.", Utils::UiColorRange())
			.ColorField("Focused", &UiTheme::Focused, "The tint of the widget that has keyboard and gamepad focus.", Utils::UiColorRange())
			.ColorField("Disabled", &UiTheme::Disabled, "The tint of a widget whose Interactable is false.", Utils::UiColorRange());

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
				"Rounds every element's corners to whole scene pixels, so images and clips land on pixel corners.")
			.Field("Font", &UiCanvasComponent::Font, "The font of the canvas's texts whose own Font is null; null uses the default font.")
			.Field("Theme", &UiCanvasComponent::Theme, "The state tints of the canvas's widgets (buttons, toggles, sliders and scroll views).")
			.Field("DefaultFocus", &UiCanvasComponent::DefaultFocus,
				"The UI widget keyboard and gamepad navigation focuses first while nothing has focus; null: navigation starts only from a pointer "
				"press or Ui.SetFocus.");

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
			.Field("Filter", &UiImageComponent::Filter, "How the frame's texture is sampled.")
			.Field("RaycastTarget", &UiImageComponent::RaycastTarget,
				"Whether the image catches the pointer for its nearest widget; false lets the pointer pass through it.");

		RegisterComponent<UiTextComponent>(registry, "UiText", "Draws text in a UI element's rectangle, wrapped and aligned in it.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Field("Text", &UiTextComponent::Text, "The UTF-8 text; a line feed starts a new line. Scripts read localized strings with Loc.Get.")
			.Field("Font", &UiTextComponent::Font, "The font; null uses its canvas's Font, or the default font when the canvas names none.")
			.Field("Size", &UiTextComponent::Size, "The em size in canvas units.", { .Min = Utils::MinUiTextSize, .Max = Utils::MaxUiTextSize, .Unit = "units" })
			.ColorField("Color", &UiTextComponent::Color, "The linear text colour with opacity in the fourth component.", Utils::UiColorRange())
			.Field("Alignment", &UiTextComponent::Alignment, "How each line is aligned within the rectangle's width.")
			.Field("VerticalAlignment", &UiTextComponent::VerticalAlignment, "Where the text block sits in the rectangle's height.")
			.Field("Wrap", &UiTextComponent::Wrap, "Whether lines break at spaces to fit the rectangle's width.")
			.Field("LineSpacing", &UiTextComponent::LineSpacing, "Multiplies the font's line height.",
				{ .Min = Utils::MinUiLineSpacing, .Max = Utils::MaxUiLineSpacing })
			.Field("RaycastTarget", &UiTextComponent::RaycastTarget,
				"Whether the text catches the pointer for its nearest widget; false lets the pointer pass through it.");

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

		// C22.2 (decision 21): the widgets, one per element (each excludes the other three, declared by both sides).
		constexpr std::string_view InteractableDescription =
			"Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event.";
		constexpr std::string_view FocusableDescription =
			"Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it.";

		RegisterComponent<UiButtonComponent>(registry, "UiButton", "A UI button: a pointer click or Submit while focused raises OnUiClick.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Excludes<UiToggleComponent>()
			.Excludes<UiSliderComponent>()
			.Excludes<UiScrollViewComponent>()
			.Excludes<UiTextInputComponent>()
			.Field("Interactable", &UiButtonComponent::Interactable, InteractableDescription)
			.Field("Focusable", &UiButtonComponent::Focusable, FocusableDescription);

		RegisterComponent<UiToggleComponent>(registry, "UiToggle",
			"A UI toggle: a pointer click or Submit while focused flips IsOn and raises OnUiValueChanged; Graphic shows while it is on.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Excludes<UiButtonComponent>()
			.Excludes<UiSliderComponent>()
			.Excludes<UiScrollViewComponent>()
			.Excludes<UiTextInputComponent>()
			.Field("Interactable", &UiToggleComponent::Interactable, InteractableDescription)
			.Field("Focusable", &UiToggleComponent::Focusable, FocusableDescription)
			.Field("IsOn", &UiToggleComponent::IsOn, "Whether the toggle is on.")
			.Field("Graphic", &UiToggleComponent::Graphic,
				"A UI element below the toggle (its check mark) that is drawn and hit only while the toggle is on; null for none.");

		RegisterComponent<UiSliderComponent>(registry, "UiSlider",
			"A UI slider: the pointer drags its value between Min and Max and the directions along it step it, raising OnUiValueChanged.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Excludes<UiButtonComponent>()
			.Excludes<UiToggleComponent>()
			.Excludes<UiScrollViewComponent>()
			.Excludes<UiTextInputComponent>()
			.Field("Interactable", &UiSliderComponent::Interactable, InteractableDescription)
			.Field("Focusable", &UiSliderComponent::Focusable, FocusableDescription)
			.Field("Value", &UiSliderComponent::Value, "The value, read clamped to Min and Max (and rounded with WholeNumbers).",
				{ .Min = -Utils::MaxUiLength, .Max = Utils::MaxUiLength })
			.Field("Min", &UiSliderComponent::Min, "The value at the start edge (left, or bottom when Vertical); below Max.",
				{ .Min = -Utils::MaxUiLength, .Max = Utils::MaxUiLength })
			.Field("Max", &UiSliderComponent::Max, "The value at the end edge (right, or top when Vertical).", { .Min = -Utils::MaxUiLength, .Max = Utils::MaxUiLength })
			.Field("WholeNumbers", &UiSliderComponent::WholeNumbers, "Whether the value is rounded to whole numbers.")
			.Field("Step", &UiSliderComponent::Step,
				"The change per navigation press along the slider; 0 is a tenth of the range. With WholeNumbers it is rounded to a whole number of at least 1.",
				{ .Min = 0.0, .Max = Utils::MaxUiLength })
			.Field("Direction", &UiSliderComponent::Direction, "Horizontal puts Min at the left edge; Vertical puts it at the bottom edge.")
			.Field("Fill", &UiSliderComponent::Fill, "A UI element below the slider stretched from the Min edge to the value; null for none.")
			.Field("Handle", &UiSliderComponent::Handle, "A UI element below the slider placed at the value; null for none.")
			.Validate(&Utils::ValidateUiSlider)
			.Generate(&Utils::MakeUiSliderValid);

		RegisterComponent<UiScrollViewComponent>(registry, "UiScrollView",
			"A UI scroll view: clips its descendants and moves its Content by the scroll position, which the wheel, a drag and focus change.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Excludes<UiButtonComponent>()
			.Excludes<UiToggleComponent>()
			.Excludes<UiSliderComponent>()
			.Excludes<UiTextInputComponent>()
			.Field("Interactable", &UiScrollViewComponent::Interactable, InteractableDescription)
			.Field("Focusable", &UiScrollViewComponent::Focusable, FocusableDescription)
			.Field("Content", &UiScrollViewComponent::Content, "The UI element below the scroll view that scrolls; null scrolls nothing.")
			.Field("Horizontal", &UiScrollViewComponent::Horizontal, "Whether the content scrolls along x.")
			.Field("Vertical", &UiScrollViewComponent::Vertical, "Whether the content scrolls along y.")
			.Field("ScrollPosition", &UiScrollViewComponent::ScrollPosition,
				"Canvas units the content is moved left (x) and up (y), read clamped to the content's overflow.",
				{ .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" })
			.Field("ScrollStep", &UiScrollViewComponent::ScrollStep, "Canvas units per wheel step and per navigation press.",
				{ .Min = 0.0, .Max = Utils::MaxUiLength, .Unit = "units" });

		// C22.3 (decision 45): the text input, a fifth widget (it and the other four exclude each other, declared by both).
		RegisterComponent<UiTextInputComponent>(registry, "UiTextInput",
			"A UI text field: while focused, typed characters insert at its caret and the editing keys edit it, raising OnUiTextChanged; Enter raises OnUiSubmit.")
			.Category("Ui")
			.Version(1)
			.Requires<UiRectComponent>()
			.Excludes<UiButtonComponent>()
			.Excludes<UiToggleComponent>()
			.Excludes<UiSliderComponent>()
			.Excludes<UiScrollViewComponent>()
			.Field("Interactable", &UiTextInputComponent::Interactable, InteractableDescription)
			.Field("Focusable", &UiTextInputComponent::Focusable, FocusableDescription)
			.Field("Text", &UiTextInputComponent::Text,
				"The field's UTF-8 text: typing inserts at the caret; a script's write raises no OnUiTextChanged.")
			.Field("MaxLength", &UiTextInputComponent::MaxLength, "The most codepoints typing reaches; characters typed past it are dropped.",
				{ .Min = 1.0, .Max = static_cast<double>(MaxUiTextInputLength) })
			.Field("Caret", &UiTextInputComponent::Caret,
				"The caret, in codepoints from the start of Text, read clamped to Text's length; typing and the editing keys move it.",
				{ .Min = 0.0 })
			.Field("TextElement", &UiTextInputComponent::TextElement,
				"A UI element below the input whose UiText shows the input's Text, with the caret while focused; null shows nothing.")
			.Field("Placeholder", &UiTextInputComponent::Placeholder,
				"A UI element below the input shown only while its Text is empty (a hint); null for none.");
	}

}
