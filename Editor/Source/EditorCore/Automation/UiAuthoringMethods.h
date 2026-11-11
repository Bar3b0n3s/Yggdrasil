#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

// ui.createWidget (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decision 32; stream D): builds a UI widget's
// entities with their components in the edit scene as one undoable command, the action the editor's "Create UI" menu
// submits (EditorActions), so the menu needs no parity exemption. Conventions as in MethodRegistry.h. A file of M22's own:
// ADR 0028's phase-2 ledger has new methods take M20's scene target from their own files.
//
// The kinds and what each creates (canvas units; colours linear RGBA; every UiRect field not named keeps its default, so
// anchors and pivot are centred; the label texts use the canvas's font):
//   - Canvas: an entity with UiCanvas (defaults).
//   - Panel: UiRect stretched over its parent (AnchorMin (0, 0), AnchorMax (1, 1), Size (0, 0)); UiImage Color (0.1, 0.1, 0.1, 0.8).
//   - Image: UiRect Size (100, 100); UiImage (defaults: white).
//   - Text: UiRect Size (200, 50); UiText Text `text` (default "Text"), Size 32, Alignment Center, VerticalAlignment Middle.
//   - Button: UiRect Size (200, 50); UiImage Color (0.9, 0.9, 0.9, 1); UiButton; and a child "Label": UiRect stretched,
//     UiText Text `text` (default "Button"), Size 28, Color (0.05, 0.05, 0.05, 1), Alignment Center, VerticalAlignment Middle.
//   - Toggle: UiRect Size (32, 32); UiImage Color (0.9, 0.9, 0.9, 1); UiToggle Graphic = its child "Check": UiRect stretched
//     with Size (-12, -12), UiImage Color (0.1, 0.1, 0.1, 1); and a child "Label": UiRect AnchorMin and AnchorMax (1, 0.5),
//     Pivot (0, 0.5), Position (8, 0), Size (160, 32), UiText Text `text` (default "Toggle"), Size 24, VerticalAlignment
//     Middle.
//   - Slider: UiRect Size (200, 20); UiImage Color (0.3, 0.3, 0.3, 1); UiSlider Fill = its child "Fill": UiRect AnchorMin
//     (0, 0), AnchorMax (0, 1), Size (0, 0), UiImage Color (0.3, 0.6, 1, 1); Handle = its child "Handle": UiRect AnchorMin and
//     AnchorMax (0, 0.5), Size (20, 32), UiImage Color (0.95, 0.95, 0.95, 1).
//   - ScrollView: UiRect Size (300, 200); UiImage Color (0.1, 0.1, 0.1, 0.8); UiScrollView Content = its child "Content":
//     UiRect AnchorMin (0, 0), AnchorMax (1, 0), Pivot (0.5, 0), Size (0, 400), UiLayout Kind Stack, Direction Vertical,
//     Padding 8 on each side, Spacing (0, 8).
//   - TextInput (C22.3, Docs/Decisions/0033-m22-contract.md decision 53; stream T): UiRect Size (240, 32); UiImage Color
//     (0.15, 0.15, 0.15, 1); UiTextInput (empty Text) whose TextElement is its child "Text": UiRect stretched with Size
//     (-12, -12) (a 6-unit inset), UiText Text empty, Size 20, Color (1, 1, 1, 1), Wrap off, VerticalAlignment Middle;
//     and whose Placeholder is its child "Placeholder": the same rectangle, UiText Text `text` (default "Enter text"),
//     Size 20, Color (0.6, 0.6, 0.6, 1), Wrap off, VerticalAlignment Middle.
// The widget's root is named `name` (default: the kind's name, "Scroll View" for ScrollView, "Text Input" for TextInput) and
// placed below `parent` (an EntityRef), last or at `index`. A kind other than Canvas needs a UiCanvas at or above its parent:
// below a parent without one (a mesh entity the hierarchy's "Create UI child" was opened on), the widget goes below a new
// canvas named "Canvas", created below `parent` (last or at `index`) in the same command. Without `parent`, a Canvas is a
// root and any other kind goes below a new root canvas named "Canvas", created in the same command; `index` is then the
// created root's position among the scene's roots (the Canvas's, or the new canvas's). Children are named as above, in the
// order listed.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry enum "UiWidgetKind": what ui.createWidget builds.
	enum class UiWidgetKind : uint8_t
	{
		Canvas,
		Panel,
		Image,
		Text,
		Button,
		Toggle,
		Slider,
		ScrollView,
		TextInput // C22.3 (decision 53), appended
	};

	// ui.createWidget {kind, parent?, index?, name?, text?, scene?}.
	struct UiCreateWidgetParams
	{
		UiWidgetKind Kind = UiWidgetKind::Canvas; // required
		std::string Parent{};
		uint32_t Index = 0; // only when given (MethodContext::HasParam)
		std::string Name{}; // only when given; not empty
		std::string Text{}; // only when given; Text, Button, Toggle and TextInput (its placeholder) only
	};

	struct UiCreateWidgetResult
	{
		EntitySummary Entity{};              // the widget's root
		std::vector<std::string> Entities{}; // every created entity's id, in creation order (a new canvas first)
		uint32_t UndoIndex = 0;
	};

	namespace Automation {

		// ui.createWidget. Errors: NotFound for an unknown `parent`; InvalidArgument at /name for an empty name, at /text for a
		// kind without a label, at /index past the parent's children (the scene's roots without `parent`); InvalidState when
		// the edit scene is read-only or a play session owns it, as entity.create's.
		[[nodiscard]] Result<UiCreateWidgetResult> UiCreateWidget(EditorMethodContext& context, const UiCreateWidgetParams& params);

	}

	// Registers UiWidgetKind and the params and result structs above.
	void RegisterUiAuthoringMethodTypes(TypeRegistry& registry);

	// Registers ui.createWidget: a tool (§13.8 ui_create_widget), AllowedInBatch, mutating, with a dry run, taking the scene
	// target (SupportsSceneTarget; the scene-target inventory's Target), editor only.
	void RegisterUiAuthoringMethods(MethodRegistry& methods);

}
