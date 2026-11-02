#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/UiLayout.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

// UI interaction (M22, C22.2, R-UI-INPUT and R-UI-DET; Docs/Decisions/0033-m22-contract.md decisions 22 to 24 and 29): the
// data the UiInput phase reads and keeps, the events it raises, and the pure functions it hit-tests and navigates with. The
// state machine itself is UiSystem::ProcessInput (Scene/UiSystem.h); the widgets' own behaviours are Scene/UiWidgets.h's.
//
// Everything here runs in the fixed step from the tick's latched step view (UiInputFrame, built by the session from
// PlayInput's step view and the project's Ui settings), against the session's current layout (UiSystem::GetLayout), in
// scene pixels: only IEEE-exact float operations (+, -, *, /, comparisons, floor) in canonical order, so a deterministic
// session interacts identically in every configuration (ADR 0028's "UI determinism": focus and navigation run only from
// latched step input). Scene is on the simulation path's lint scope (§4.12). Main thread.
//
// Pointer targets (decision 22). An element of a layout table is a pointer target when it is Visible (UiLayoutNode) and
// holds a widget component (UiButton, UiToggle, UiSlider, UiScrollView), or a UiImage or UiText whose RaycastTarget is set.
// A point is over a target when it lies in the target's rectangle (Min <= p < Max on both axes) and, when the target is
// Clipped, in its clip (the pixel rectangle [X0, X1) x [Y0, Y1) of UiClipRect, compared with the point as is). The
// topmost target over a point is the last such element in the table's draw order (its canvases in draw order, each
// canvas's elements in canonical order). Its widget is the nearest element at or above it, within its canvas (through
// UiLayoutNode::Parent), that holds a widget component; a target without one blocks the pointer but reaches no widget.
//
// Focus (decision 23). An element is focusable when it is in the layout table, not Hidden, with a rectangle of positive
// width and height (it may be clipped or outside the scene image: a list item scrolled out of its scroll view stays
// focusable, and focusing it scrolls it into view), and holds a widget component whose Interactable and Focusable are
// both set. Navigation moves focus between the focusable elements of every canvas of the table, and only from an element
// that has focus: with nothing focused, a navigation command (a direction, the stick's, Next or Previous) focuses the
// table's default focus, the DefaultFocus of the last canvas in draw order whose DefaultFocus is focusable, and does
// nothing when no canvas names one. So a game whose play uses the arrows and Space keeps them while nothing has focus,
// and its menus opt in through a canvas's DefaultFocus, Ui.SetFocus or a pointer press on a focusable widget.

namespace Engine {

	class Scene;

	// The widget an element holds (one at most: the four components exclude each other).
	enum class UiWidgetType : uint8_t
	{
		None,
		Button,
		Toggle,
		Slider,
		ScrollView
	};

	// "Button", "Toggle", "Slider", "ScrollView"; empty for None (ui.tree's widget kind).
	[[nodiscard]] std::string_view UiWidgetTypeToString(UiWidgetType type);

	// The widget `element` holds in `scene`; None for an entity without one or that the scene does not hold.
	[[nodiscard]] UiWidgetType GetUiWidgetType(const Scene& scene, UUID element);

	// Whether `element` holds a widget whose Interactable is set.
	[[nodiscard]] bool IsUiInteractable(const Scene& scene, UUID element);

	// Whether `element` is focusable in `table` (see the file comment).
	[[nodiscard]] bool IsUiFocusable(const Scene& scene, const UiLayoutTable& table, UUID element);

	// A navigation direction, in scene pixels: Up is -y, Down +y, Left -x, Right +x.
	enum class UiDirection : uint8_t
	{
		Up,
		Down,
		Left,
		Right
	};

	// The navigation commands of one step: the settings' bindings that were pressed in the step view (decision 30).
	enum class UiNavigation : uint8_t
	{
		None = 0,
		Up = 1 << 0,
		Down = 1 << 1,
		Left = 1 << 2,
		Right = 1 << 3,
		Next = 1 << 4,     // Ui.Next pressed without a Shift key down
		Previous = 1 << 5, // Ui.Next pressed with a Shift key down
		Submit = 1 << 6,
		Cancel = 1 << 7
	};

	template<>
	inline constexpr bool EnableFlagOperators<UiNavigation> = true;

	// What the UiInput phase reads from the tick's latched step view (decision 24). The session builds it: the pointer is
	// the mouse in view coordinates mapped to scene pixels (MapViewPointToScenePoint of the session's output mapping), the
	// button is Mouse.Left, Scroll the step's scroll delta, Navigation the Ui settings' bindings pressed in the step view
	// and Stick the left stick of the connected gamepad whose stick has the largest magnitude (the first such gamepad on a
	// tie); Navigation and Stick are empty and zero while Ui.Navigation is off.
	struct UiInputFrame
	{
		glm::vec2 Pointer = glm::vec2(0.0f); // scene pixels
		bool PointerDown = false;
		bool PointerPressed = false;
		bool PointerReleased = false;
		glm::vec2 Scroll = glm::vec2(0.0f); // wheel steps, x right-positive, y up-positive (InputState's convention)
		UiNavigation Navigation = UiNavigation::None;
		glm::vec2 Stick = glm::vec2(0.0f); // engine convention: x right-positive, y up-positive, each -1 to 1
	};

	// The left stick's navigation direction: the axis of larger magnitude when it is at least 0.5 (x on a tie), its sign
	// giving the direction (y up-positive: a positive y is Up); None below 0.5.
	enum class UiStickDirection : uint8_t
	{
		None,
		Up,
		Down,
		Left,
		Right
	};

	// The session's interaction state: what the UiInput phase keeps from one tick to the next and the Ui script module
	// reads, held by UiSystem, outside the scene (it is not serialized). The ui state-hash block holds it with the event
	// queue (decision 29). It is empty (the default) while nothing is focused, hovered, pressed or dragged, the stick is at
	// rest and the pointer is over no target: always in a scene without a canvas (UiSystem::ProcessInput keeps it so), and
	// in a scene with UI until the pointer reaches a target or something takes focus.
	struct UiInteractionState
	{
		UUID Focused{}; // the element with keyboard and gamepad focus
		UUID Hovered{}; // the interactable widget under the pointer
		UUID Pressed{}; // the widget a pointer press began on, while the pointer is held
		UUID Dragged{}; // the slider or scroll view the held pointer drags (it is Pressed)
		// The pointer where the drag began, in scene pixels, and the dragged scroll view's ScrollPosition as read then (zero
		// for a slider, whose value follows the pointer directly); both zero while nothing is dragged.
		glm::vec2 DragOrigin = glm::vec2(0.0f);
		glm::vec2 DragStart = glm::vec2(0.0f);
		UiStickDirection Stick = UiStickDirection::None; // the stick's direction at the last UiInput (its edges navigate)
		bool PointerOverUi = false;                      // the pointer was over a pointer target at the last UiInput

		[[nodiscard]] bool IsEmpty() const { return *this == UiInteractionState{}; }

		bool operator==(const UiInteractionState&) const = default;
	};

	// The UI events (decision 27), each delivered by the UiEvents phase to its callback, bubbling from Target.
	enum class UiEventKind : uint8_t
	{
		Click,        // OnUiClick(self, element): a Button was activated
		ValueChanged, // OnUiValueChanged(self, element, value): a Toggle's IsOn (a boolean) or a Slider's Value (a number) changed
		Focus,        // OnUiFocus(self, element, focused): the element gained (true) or lost (false) focus
		Hover,        // OnUiHover(self, element, hovered): the pointer entered (true) or left (false) the widget
		Cancel,       // OnUiCancel(self, element): Cancel was pressed with the element focused (or at the last canvas)
		Scroll        // OnUiScroll(self, element, position): a ScrollView's ScrollPosition changed
	};

	// "Click", "ValueChanged", "Focus", "Hover", "Cancel", "Scroll".
	[[nodiscard]] std::string_view UiEventKindToString(UiEventKind kind);
	// The callback that receives `kind`: "OnUiClick", "OnUiValueChanged", "OnUiFocus", "OnUiHover", "OnUiCancel", "OnUiScroll".
	[[nodiscard]] std::string_view GetUiEventCallbackName(UiEventKind kind);

	// One UI event. Only the members of its Kind are read.
	struct UiEvent
	{
		UiEventKind Kind = UiEventKind::Click;
		UUID Target{};
		bool Flag = false;                  // Focus: gained; Hover: entered; ValueChanged of a Toggle: its new IsOn
		bool HasNumber = false;             // ValueChanged of a Slider: Number holds its new value (else Flag is the value)
		float Number = 0.0f;                // ValueChanged of a Slider
		glm::vec2 Vector = glm::vec2(0.0f); // Scroll: the new ScrollPosition

		bool operator==(const UiEvent&) const = default;
	};

	// The topmost pointer target over a point and its widget (see the file comment).
	struct UiHit
	{
		UUID Target{}; // null when the point is over no pointer target
		UUID Widget{}; // null when the target has no widget at or above it
	};

	// The hit test of the file comment: the topmost pointer target of `table` (a layout of `scene`) over `point` (scene
	// pixels) and its widget. Pure.
	[[nodiscard]] UiHit HitTestUi(const Scene& scene, const UiLayoutTable& table, glm::vec2 point);

	// Directional navigation (decision 23) from `from` toward `direction`: among the focusable elements of `table` other
	// than `from`, with the centre c of each rectangle and v = c - (the centre of from's rectangle), the along-distance a =
	// the component of v in `direction` (v.y for Down, -v.y for Up, v.x for Right, -v.x for Left) and the across-distance
	// b = |the other component of v|, those with a > 0, the one with the smallest a + 2 * b (the first in draw order on a
	// tie). With `from` null, or not an element of `table`: the table's default focus (the file comment), or null when no
	// canvas names one. Null when there is none (focus then stays). Pure.
	[[nodiscard]] UUID FindUiNavigationTarget(const Scene& scene, const UiLayoutTable& table, UUID from, UiDirection direction);

	// Sequential navigation (Next and Previous): the focusable element after `from` in draw order (`forward`), wrapping to
	// the first, or the one before it, wrapping to the last; with `from` null or not focusable, the table's default focus
	// (the file comment) either way, or null when no canvas names one. Null when no element is focusable. Pure.
	[[nodiscard]] UUID FindNextUiFocus(const Scene& scene, const UiLayoutTable& table, UUID from, bool forward);

	// The stick's direction under UiStickDirection's rule. Pure.
	[[nodiscard]] UiStickDirection GetUiStickDirection(glm::vec2 stick);

}
