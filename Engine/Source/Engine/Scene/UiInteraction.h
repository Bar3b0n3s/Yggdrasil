#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/UiLayout.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
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
// holds a widget component (UiButton, UiToggle, UiSlider, UiScrollView, and C22.3's UiTextInput), or a UiImage or UiText
// whose RaycastTarget is set.
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
//
// World canvases (C22.3, R-UI-WORLD; decision 50; stream W). A World canvas (UiCanvasComponent::RenderMode) is laid out in
// its own canvas pixels (UiCanvasLayout::SceneSize) and placed in the world by UiCanvasLayout::World, so its elements'
// rectangles are not scene pixels. The pointer reaches it along a ray through the scene's primary camera
// (ComputeUiPointerRay, from the step's latched pointer and the camera's simulated pose): the ray meets the canvas's plane
// at a point of its canvas pixels (MapUiPointerToCanvas), where the canvas's elements are hit-tested by the rule above,
// restricted to that canvas and to its rectangle ((0, 0) to UiCanvasLayout::SceneSize, outside which it draws nothing,
// Renderer/UiPass.h), and to the camera's clip range (UiPointerRay::MinDistance to MaxDistance: beyond the near plane,
// and before the far plane of an orthographic camera, whose projection clips there; a perspective camera's far plane is
// at infinity, Architecture §8.3), where it draws. Scene geometry does not occlude the pointer: a World canvas behind a
// mesh still takes it where the ray meets it (an accepted limit, decision 50 and Backlog §6: a game that needs
// occlusion raycasts with Physics before it acts). Screen canvases draw over the world, so a target of a Screen canvas
// under the pointer wins; otherwise the nearest World canvas along the ray with a target under its point (the later in
// draw order on a tie) (HitTestUiPointer). Directional navigation moves among the focusable elements of one render
// space: every Screen canvas's together, as before C22.3, or one World canvas's; Next, Previous and the default focus
// span every canvas in draw order. The same IEEE-exact operations, with DetMath for the camera's field of view
// (Core/DetMath.h), so a world canvas interacts identically in every configuration.
//
// The scene image (the milestone review). The UiInput phase hit-tests only a pointer inside the scene image ([0, SceneSize)
// of the layout on both axes; UiSystem::ProcessInput's step 2): outside it, in a PixelPerfectCamera's letterbox, nothing is
// drawn, so the pointer reaches no UI there, as viewport.pick takes nothing, whatever an element's rectangle or a World
// canvas reaches past the image's edge. HitTestUi and HitTestUiPointer stay pure geometry and take any point.

namespace Engine {

	class Scene;

	// The widget an element holds (one at most: the five components exclude each other).
	enum class UiWidgetType : uint8_t
	{
		None,
		Button,
		Toggle,
		Slider,
		ScrollView,
		TextInput // C22.3 (decision 45): UiTextInput
	};

	// "Button", "Toggle", "Slider", "ScrollView", "TextInput"; empty for None (ui.tree's widget kind).
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

	// C22.3 (decision 47): the text input's editing keys pressed in the step view (Key.Backspace, Key.Delete, Key.Left,
	// Key.Right, Key.Home, Key.End, and Key.Enter or Key.KeypadEnter): fixed keys, never the Ui settings' bindings.
	enum class UiTextKeys : uint8_t
	{
		None = 0,
		Backspace = 1 << 0,
		Delete = 1 << 1,
		Left = 1 << 2,
		Right = 1 << 3,
		Home = 1 << 4,
		End = 1 << 5,
		Enter = 1 << 6
	};

	template<>
	inline constexpr bool EnableFlagOperators<UiTextKeys> = true;

	// C22.3 (decision 50): a pointer ray in world space, through the scene's primary camera (ComputeUiPointerRay).
	// MinDistance and MaxDistance (added by W22.3's review) bound the ray parameters, in world units along Direction from
	// Origin, at which the camera draws, inclusive: a World canvas is hit only where the ray meets it between them, so a
	// canvas the camera clips takes no pointer. The defaults bound nothing beyond the origin.
	struct UiPointerRay
	{
		glm::vec3 Origin = glm::vec3(0.0f);
		glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f); // unit length
		float MinDistance = 0.0f;
		float MaxDistance = std::numeric_limits<float>::infinity();

		bool operator==(const UiPointerRay&) const = default;
	};

	// What the UiInput phase reads from the tick's latched step view (decision 24). The session builds it: the pointer is
	// the mouse in view coordinates mapped to scene pixels (MapViewPointToScenePoint of the session's output mapping), the
	// button is Mouse.Left, Scroll the step's scroll delta, Navigation the Ui settings' bindings pressed in the step view
	// and Stick the left stick of the connected gamepad whose stick has the largest magnitude (the first such gamepad on a
	// tie); Navigation and Stick are empty and zero while Ui.Navigation is off.
	// C22.3 (decisions 46, 47 and 50) appends what the text input and the world canvases read: the tick's latched text and
	// editing keys, the navigation commands a gamepad pressed (so a focused text input can ignore the keyboard's), and the
	// pointer's ray, which the session computes only while the current layout holds a World canvas.
	struct UiInputFrame
	{
		glm::vec2 Pointer = glm::vec2(0.0f); // scene pixels
		bool PointerDown = false;
		bool PointerPressed = false;
		bool PointerReleased = false;
		glm::vec2 Scroll = glm::vec2(0.0f); // wheel steps, x right-positive, y up-positive (InputState's convention)
		UiNavigation Navigation = UiNavigation::None;
		glm::vec2 Stick = glm::vec2(0.0f); // engine convention: x right-positive, y up-positive, each -1 to 1
		// C22.3: the tick's committed characters (PlayInput::GetText of the step view, the text latch), UTF-8 in the order
		// typed.
		std::string Text{};
		UiTextKeys TextKeys = UiTextKeys::None; // C22.3: the editing keys pressed in the step view
		// C22.3: the commands of Navigation pressed through a gamepad button binding (a subset of Navigation; the stick's
		// edges are UiSystem::ProcessInput's own, step 7); while a text input has focus only these, and NonEditingNavigation's
		// Next, Previous and Cancel, navigate (decision 47).
		UiNavigation GamepadNavigation = UiNavigation::None;
		// The milestone review (decision 47): the commands of Navigation pressed through a gamepad button binding or a key
		// binding other than the keys a text input edits with (Backspace, Delete, Left, Right, Home, End), a subset of
		// Navigation holding GamepadNavigation; while a text input has focus its Next, Previous and Cancel navigate, so an
		// editing key bound to one of them only edits.
		UiNavigation NonEditingNavigation = UiNavigation::None;
		// C22.3: the pointer's ray through the primary camera (ComputeUiPointerRay); nullopt without a primary camera, and
		// whenever the current layout holds no World canvas (the session computes none then).
		std::optional<UiPointerRay> Ray{};
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
		// The pointer where the drag began, in the pixels of the dragged widget's canvas (scene pixels for a Screen canvas, a
		// World canvas's own for one of a World canvas: C22.3, UiSystem::ProcessInput's steps 3 and 4), and the dragged
		// scroll view's ScrollPosition as read then (zero for a slider, whose value follows the pointer directly); both zero
		// while nothing is dragged.
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
		Scroll,       // OnUiScroll(self, element, position): a ScrollView's ScrollPosition changed
		// C22.3 (decision 47): these carry no text (so the ui hash block keeps its layout); the callback receives the input's
		// Text as it is when the callback runs.
		TextChanged, // OnUiTextChanged(self, element, text): typing or an editing key changed a UiTextInput's Text
		Submit       // OnUiSubmit(self, element, text): Enter, or Submit, while the UiTextInput had focus
	};

	// "Click", "ValueChanged", "Focus", "Hover", "Cancel", "Scroll", "TextChanged", "Submit".
	[[nodiscard]] std::string_view UiEventKindToString(UiEventKind kind);
	// The callback that receives `kind`: "OnUiClick", "OnUiValueChanged", "OnUiFocus", "OnUiHover", "OnUiCancel",
	// "OnUiScroll", "OnUiTextChanged", "OnUiSubmit".
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

		bool operator==(const UiHit&) const = default;
	};

	// The hit test of the file comment: the topmost pointer target of `table` (a layout of `scene`) over `point` (scene
	// pixels) and its widget, among the Screen canvases' elements (C22.3: a World canvas's are in its own pixels, which
	// HitTestUiPointer reaches). Pure.
	[[nodiscard]] UiHit HitTestUi(const Scene& scene, const UiLayoutTable& table, glm::vec2 point);

	// --- World canvases (C22.3, decision 50; stream W) --------------------------------------------------------------------

	// The ray through `pointer` (scene pixels of a scene image of `sceneSize`, each >= 1, asserted) from the scene's primary
	// camera (the one its game view renders: the first enabled Camera with Primary in canonical order, as render extraction
	// picks it): the inverse of the projection render extraction gives that camera's view, at its simulated world transform
	// (the TransformSystem's world matrix, never the interpolated render pose). With the Camera component's projection
	// (RenderExtraction.h), n = (2 pointer.x / sceneSize.x - 1, 1 - 2 pointer.y / sceneSize.y) and the aspect
	// sceneSize.x / sceneSize.y, a Perspective camera's ray starts at its position along its orientation times
	// normalize(n.x aspect t, n.y t, -1), t = DetMath::Tan(VerticalFov / 2 in radians); an Orthographic camera's starts at
	// its position plus its orientation times (n.x aspect OrthographicSize, n.y OrthographicSize, -NearClip), along its -Z.
	// Its clip range (MinDistance to MaxDistance; W22.3's review): a Perspective ray's from NearClip / c, c the cosine
	// between the ray and the camera's -Z (1 / |(n.x aspect t, n.y t, -1)|), without an end (the projection's far plane is
	// at infinity, §8.3; FarClip culls meshes, not canvases); an Orthographic ray's from 0 to FarClip - NearClip (its
	// origin is on the near plane), as viewport.pick's ray interval (ComputeViewPixelRayInterval) takes them but for the
	// perspective far plane.
	// A sibling that renders the primary camera through another projection or pose follows it here: since M22's landing
	// (decision 50, risk 11, "Landing"), a primary camera that holds M21's PixelPerfectCamera casts the inverse of its
	// projection as render extraction draws it, whatever the Camera's own Projection and OrthographicSize: Orthographic,
	// with ComputePixelPerfectView's half height (ReferenceHeight / (2 PixelsPerUnit)), from the position it snaps the
	// simulated position to, `sceneSize` then being its reference resolution (the session's ComputeGameViewOutputMapping).
	// nullopt without a primary camera. Pure.
	[[nodiscard]] std::optional<UiPointerRay> ComputeUiPointerRay(const Scene& scene, glm::vec2 pointer, glm::uvec2 sceneSize);

	// Where the pointer is in a canvas's pixels (MapUiPointerToCanvas).
	struct UiCanvasPointer
	{
		glm::vec2 Point = glm::vec2(0.0f); // canvas pixels (scene pixels for a Screen canvas)
		float Distance = 0.0f;             // World: the ray parameter (world units along the unit direction); Screen: 0

		bool operator==(const UiCanvasPointer&) const = default;
	};

	// The pointer in `canvas`'s pixels: a Screen canvas's is `point` itself (scene pixels); a World canvas's is the point
	// where `ray` meets the canvas's plane (UiCanvasLayout::World's z = 0 plane, from either side), mapped through the
	// inverse of World, with its ray parameter: nullopt without a ray, for a ray parallel to the plane, or one that meets it
	// behind its origin (a parameter below 0) or outside its clip range (below MinDistance or above MaxDistance: W22.3's
	// review). Pure.
	[[nodiscard]] std::optional<UiCanvasPointer> MapUiPointerToCanvas(const UiCanvasLayout& canvas, glm::vec2 point,
		const std::optional<UiPointerRay>& ray);

	// The topmost pointer target under the pointer across every canvas of a table (HitTestUiPointer).
	struct UiPointerHit
	{
		UiHit Hit{};
		UUID Canvas{};                           // the hit target's canvas; null without a target
		glm::vec2 CanvasPoint = glm::vec2(0.0f); // the pointer in that canvas's pixels

		bool operator==(const UiPointerHit&) const = default;
	};

	// The file comment's world-canvas rule: HitTestUi's target among the Screen canvases at `point` when there is one;
	// otherwise, among the World canvases the pointer maps onto (MapUiPointerToCanvas), the target of the one with the
	// smallest Distance that has a target at its point (the later in draw order on a tie), hit-tested by HitTestUi's rule
	// within that canvas. Pure.
	[[nodiscard]] UiPointerHit HitTestUiPointer(const Scene& scene, const UiLayoutTable& table, glm::vec2 point,
		const std::optional<UiPointerRay>& ray);

	// Directional navigation (decision 23) from `from` toward `direction`: among the focusable elements of `table` other
	// than `from` in `from`'s render space (C22.3: every Screen canvas's elements, or its own World canvas's; the file
	// comment), with the centre c of each rectangle and v = c - (the centre of from's rectangle), the along-distance a =
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
