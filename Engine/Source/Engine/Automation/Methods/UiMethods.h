#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

// The ui domain (M22, C22.1, R-UI-AUTO; Docs/Decisions/0033-m22-contract.md decision 8): ui.tree, the laid-out UI of a scene,
// shared by the Editor and the Runtime (§13.5 "Runtime subset"), so an agent sees where every element landed in scene pixels
// without a screenshot. Conventions as in MethodRegistry.h. The interaction members of UiTreeEntry were reserved by W22.1;
// C22.2 (decision 31; stream A) fills them: the widget kind and Interactable from the scene's components, and, for the play
// scene, Focused, Hovered and Pressed from the session's interaction state (UiSystem::GetInteraction); the edit scene has
// no interaction, so they are false there.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry struct "UiPixelRect": a rectangle in scene pixels, x right and y down from the scene image's top-left corner.
	struct UiPixelRect
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;
	};

	// Registry struct "UiTreeEntry": one canvas or element of the JSON tree, in draw order.
	struct UiTreeEntry
	{
		std::string Id{};
		std::string Name{};
		std::string Path{};
		std::string Canvas{}; // the canvas's id (a canvas's own)
		std::string Parent{}; // the parent element's id, or the canvas's for a top-level element; empty for a canvas
		uint32_t Depth = 0;   // 0 for a canvas, 1 for a top-level element, its parent's plus one below it
		// The UI components it holds (UiCanvas, UiRect, UiImage, UiText, UiLayout, UiLayoutElement), registry order.
		std::vector<std::string> Components{};
		UiPixelRect Rect{}; // laid out (UiLayoutNode::Min and Max); a canvas's is the scene image
		// A canvas's own UiCanvas SortOrder (UiCanvasLayout); 0 for an element.
		int32_t SortOrder = 0;
		// The scale of the entry's canvas (UiCanvasLayout::Scale), a canvas's own and an element's its canvas's (M25's friction
		// F11, Docs/Decisions/0036-m25-tetris-pixel-art.md): the scene pixels per canvas unit at the resolution laid out at, what
		// turns a pixel measurement of the entry into the canvas units UiRect takes (a World canvas's: its canvas pixels per unit).
		float Scale = 0.0f;
		bool Clipped = false; // Clip holds the clip its clipping ancestors impose
		UiPixelRect Clip{};
		bool Visible = false; // UiLayoutNode::Visible; a canvas is visible
		std::string Text{};   // its UiText's Text, cut to 256 bytes at a UTF-8 boundary with "…" when longer; empty without one
		// The interaction (decision 8, filled by C22.2's stream A, decision 31): Interactable for a widget whose Interactable
		// is set; Focused, Hovered and Pressed from the play session's state (false in the edit scene); Widget the kind
		// (UiWidgetTypeToString: "Button", "Toggle", "Slider", "ScrollView"; W22.3 "TextInput"), empty for none.
		bool Interactable = false;
		bool Focused = false;
		bool Hovered = false;
		bool Pressed = false;
		std::string Widget{};
		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 53; registered by stream W after Widget): true for a World canvas
		// and its elements, whose Rect and Clip are in the canvas's own pixels (a World canvas's Rect is (0, 0, WorldWidth,
		// WorldHeight)); false for a Screen canvas's, in scene pixels. Stream T: a UiTextInput's element reports its input's
		// Text as Text and "TextInput" as Widget, and the outline's short form UiTextInput("<text>").
		bool WorldCanvas = false;
		// M25's friction F02 (Docs/Decisions/0036-m25-tetris-pixel-art.md; registered after WorldCanvas): the box of the text
		// the element draws (its UiText's; a text input's Text in its TextElement's), where the Ui pass lays it out in its
		// rectangle (PlanUiPass): from its lines' leftmost pen start to their rightmost advance end, and from the block's top to
		// its bottom, in the pixels of Rect; all 0 for an element that draws no text (no UiText, an empty one, or a hidden
		// element). TextOverflows: that box reaches beyond Rect by more than UiTextOverflowTolerance on a side.
		UiPixelRect TextBounds{};
		bool TextOverflows = false;
		// M25's friction F10 (registered after TextOverflows): whether the entity is effectively active now (Entity::IsActive: it
		// and every ancestor) in the scene read. UiTreeParams::IncludeInactive lists the inactive ones, laid out as if they were
		// active; otherwise only the play session's current layout can hold one, deactivated after the session laid it out.
		bool Active = true;
	};

	// M25's F02: how far a text's box may reach beyond its rectangle before ui.tree reports it as overflowing, in pixels: the
	// rounding of the alignment's arithmetic, never a visible overhang.
	inline constexpr float UiTextOverflowTolerance = 0.01f;

	// ui.tree {canvas?, depth?, format?, width?, height?, target?, includeInactive?}: the UI of the target scene laid out in scene
	// pixels: every canvas, or the one `canvas` names (an EntityRef to an entity with a UiCanvas), with its elements at most
	// `depth` levels below it (0: unlimited). The play scene reads the session's current layout (UiSystem::GetLayout) at its
	// scene resolution unless `width` and `height` ask for another; the edit scene is laid out at `width` x `height`, by default
	// its game view's scene resolution at the project's window size (GetDeterministicViewSize, the size of every deterministic
	// session's view, through ComputeGameViewOutputMapping: a PixelPerfectCamera's reference resolution under one, M22's
	// landing). Only effectively active canvases and elements are laid out and listed, unless `includeInactive` (M25's friction
	// F10, Docs/Decisions/0036-m25-tetris-pixel-art.md): then a copy of the scene as it is now (the serializer's, the one
	// scene-copy path), in which every entity is active, is laid out at that scene resolution in its place (the play scene's
	// too, rather than the session's layout), so an inactive canvas or element is listed where it would land and its text
	// measured as if it were active, and marked inactive (UiTreeEntry::Active false); the target scene is only read.
	// Text: a header line
	// "<file name>  rev <n>  <width>x<height> scene pixels  <count> elements", then per canvas a line "<name> <8 hex>
	// UiCanvas(sort <n>, scale <s>)", followed by "inactive" when it is, and per element a line with box-drawing indentation, its
	// name, its first 8 hex digits, its short component forms ("UiImage(<sheet path>#<frame>)", "UiText(\"<text>\")",
	// "UiLayout(<kind> <direction>)"; C22.2's widgets "UiButton", "UiToggle(on|off)", "UiSlider(<value as read>)" and
	// "UiScrollView(<x>,<y>)", the scroll position as read), its rectangle "(x,y wxh)", and "clipped" or "hidden" when it is,
	// then (M25's F10) "inactive" when it is, then (M25's F02) "overflows" when its text's box reaches beyond its rectangle
	// (UiTreeEntry::TextOverflows), then (C22.2) "disabled" for a widget that is not interactable and "focused", "hovered" and
	// "pressed" by the play session's state.
	struct UiTreeParams
	{
		std::string Canvas{};
		uint32_t Depth = 0;
		SceneTreeFormat Format = SceneTreeFormat::Text;
		// The scene resolution to lay out at, both or neither, each 1 to 16384; 0: the default above.
		uint32_t Width = 0;
		uint32_t Height = 0;
		SceneTarget Target = SceneTarget::Edit;
		bool IncludeInactive = false; // M25's F10: list the inactive canvases and elements too, laid out as if active
	};

	struct UiTreeResult
	{
		SceneSummary Scene{};
		uint32_t Width = 0; // the scene resolution laid out at
		uint32_t Height = 0;
		std::string Text{};                  // format "text"
		std::vector<UiTreeEntry> Elements{}; // format "json": canvases in draw order, each followed by its elements in draw order
	};

	namespace Automation {

		// ui.tree. Errors: those of the target (ResolveTargetScene) and of `canvas` (ResolveEntity at /canvas); InvalidArgument
		// at /canvas for an entity without a UiCanvas, and for only one of width and height; with includeInactive, those of the
		// scene's copy (SceneSerializer::ToJson and FromJson).
		[[nodiscard]] Result<UiTreeResult> UiTree(AutomationMethodContext& context, const UiTreeParams& params);

	}

	// Registers UiPixelRect, UiTreeEntry and the params and result structs above (after RegisterSceneMethodTypes, whose
	// SceneSummary and SceneTreeFormat they use).
	void RegisterUiMethodTypes(TypeRegistry& registry);

	// Registers ui.tree: available in the Runtime, AllowedInBatch, a tool (§13.8 ui_tree), and it takes the scene target
	// (SupportsSceneTarget, ADR 0031 decision 16).
	void RegisterUiMethods(MethodRegistry& methods);

}
