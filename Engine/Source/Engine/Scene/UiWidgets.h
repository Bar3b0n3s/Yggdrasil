#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scene/UiLayout.h"

#include <glm/glm.hpp>

#include <vector>

// The widgets' behaviours (M22, C22.2, R-UI-WIDGETS and R-UI-SCROLL; Docs/Decisions/0033-m22-contract.md decisions 21, 24
// and 26): what UiSystem::ProcessInput (Scene/UiSystem.h) does to a Button, Toggle, Slider or ScrollView once it has decided
// which widget the input reaches, and the reads the layout and the interaction share. Stream B owns the Button, Toggle and
// Slider functions, stream C the ScrollView ones. Each function writes the widget's component in `scene` (the scene's
// serialization, so the state hash, covers what interaction changes) and appends the events it raises to `events`, in the
// order raised; it does nothing for an element that is not the widget it names (or not in `table`, where it reads one).
// `table` is the current layout of `scene` (UiSystem::GetLayout): rectangles are read in scene pixels and turned into
// canvas units by their canvas's Scale. IEEE-exact float operations only. Main thread.

namespace Engine {

	class Scene;

	// --- Button and Toggle (stream B) ----------------------------------------------------------------------------------

	// Activation (a pointer click, or Submit while focused): a Button raises Click at `element`; a Toggle flips IsOn and
	// raises ValueChanged (Flag: the new IsOn); any other element, or a widget that is not interactable, does nothing.
	void ActivateUiWidget(Scene& scene, UUID element, std::vector<UiEvent>& events);

	// --- Slider (stream B) ---------------------------------------------------------------------------------------------

	// The slider's value as read (UiSliderComponent.h): Value clamped to [Min, Max]; with WholeNumbers, floor(v + 0.5)
	// clamped to [ceil(Min), floor(Max)], or Min when the range holds no whole number. Always in [Min, Max]. Pure.
	[[nodiscard]] float ReadUiSliderValue(const UiSliderComponent& slider);
	// Its normalized position: (ReadUiSliderValue - Min) / (Max - Min), in [0, 1]; 0 when Min is not below Max. Pure.
	[[nodiscard]] float GetUiSliderPosition(const UiSliderComponent& slider);

	// Sets the slider `slider` to the value at `pointer` (scene pixels) along its slide rectangle (UiSliderComponent.h),
	// writing it as read and raising ValueChanged (HasNumber, Number: the new value) when it differs from the value as read
	// before.
	void SetUiSliderFromPointer(Scene& scene, const UiLayoutTable& table, UUID slider, glm::vec2 pointer, std::vector<UiEvent>& events);

	// A navigation press while `slider` is focused: a direction along its Direction (Left or Right when Horizontal, Down or
	// Up when Vertical) changes the value as read by its step (UiSliderComponent.h: with WholeNumbers a whole number, at
	// least 1) toward Max (Right, Up) or Min (Left, Down), written as read, raising ValueChanged when it changed; returns
	// true for such a direction (the slider consumes it, whether or not the value changed), false for the other two (they
	// navigate).
	[[nodiscard]] bool StepUiSlider(Scene& scene, UUID slider, UiDirection direction, std::vector<UiEvent>& events);

	// --- ScrollView (stream C) -----------------------------------------------------------------------------------------

	// The scroll position as read: `position` clamped per axis to [0, max(0, contentSize - viewSize)] (canvas units), 0 on
	// an axis the scroll view does not scroll (Horizontal for x, Vertical for y). Pure.
	[[nodiscard]] glm::vec2 ClampUiScrollPosition(const UiScrollViewComponent& scrollView, glm::vec2 viewSize, glm::vec2 contentSize, glm::vec2 position);

	// The innermost interactable scroll view at or above `element` in `table` (through UiLayoutNode::Parent, within its
	// canvas); null when there is none. The wheel scrolls it. Pure.
	[[nodiscard]] UUID FindUiScrollView(const Scene& scene, const UiLayoutTable& table, UUID element);

	// Moves `scrollView`'s ScrollPosition, as read, by `delta` canvas units, clamped and written, raising Scroll (Vector:
	// the new position) when it changed; returns whether it changed.
	bool ScrollUiScrollView(Scene& scene, const UiLayoutTable& table, UUID scrollView, glm::vec2 delta, std::vector<UiEvent>& events);

	// A pointer drag of `scrollView` that began with its ScrollPosition at `start`: the position becomes start - (the
	// pointer's move since the drag began, in scene pixels, over the canvas's Scale), clamped and written, raising Scroll
	// when it changed.
	void DragUiScrollView(Scene& scene, const UiLayoutTable& table, UUID scrollView, glm::vec2 start, glm::vec2 pointerMove, std::vector<UiEvent>& events);

	// A navigation press while the Focusable `scrollView` is focused: a direction along a scrolling axis (Up or Down when
	// Vertical, Left or Right when Horizontal) scrolls by ScrollStep (Up and Left toward 0); returns true for such a
	// direction (consumed, whether or not it moved), false otherwise.
	[[nodiscard]] bool StepUiScrollView(Scene& scene, const UiLayoutTable& table, UUID scrollView, UiDirection direction, std::vector<UiEvent>& events);

	// After focus moved to `element`: every scroll view whose Content holds `element` (from the innermost outward) scrolls
	// the least that puts the element's rectangle inside its own (the element's top-left corner first when it is larger),
	// using the rectangles of `table` and adding each scroll view's change to the element's rectangle before the next.
	void RevealInUiScrollViews(Scene& scene, const UiLayoutTable& table, UUID element, std::vector<UiEvent>& events);

}
