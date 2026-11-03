#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scene/UiLayout.h"

#include <glm/glm.hpp>

#include <vector>

// The play session's UI (M22, C22.1, R-UI-CORE; Docs/Decisions/0033-m22-contract.md decisions 3 and 4): the two layouts of
// the session's scene that the step order keeps (ADR 0028's phase-2 ledger, PlaySessionPhase):
//   - the step's layout, made by the UiLayout phase at the end of every fixed step (after every phase that can change UI data);
//   - the frame's layout, made by the FrameUiLayout phase after FrameTransformUpdate and before RenderExtraction, so UI that
//     OnUpdate or OnLateUpdate wrote is laid out as the frame draws it.
// The current layout (GetLayout) is the later of the two: the frame's when a frame phase ran since the last step's layout,
// the step's own when none did. W22.2's UiInput hit-tests it, and ui.tree reads it for the play scene, so a hit test depends
// only on the state, never on how many frames ran between two ticks. The session also runs LayoutStep, outside the phase
// list, in PlaySession::Activate and right after a Scene.Load or Scene.Reload replaces its scene, each time both before the
// scene's startup callbacks (OnCreate, OnStart) and after them, so the current layout always describes the session's current
// scene, in those callbacks too: never a replaced scene's, and empty only before Activate. Both layouts are at the session's
// scene resolution (the view's size in W22.1; the later lander of M21 and M22 makes it the scene resolution M21's
// PixelPerfectCamera sets). Layouts are derived data: they never reach the state hash (W22.2's ui hash block holds the
// interaction state and the event queue only).
// Owned by the PlaySession (GetUi); runs in Play and Simulate alike. Main thread; not copyable.
//
// Interaction (C22.2, R-UI-INPUT, R-UI-EVENTS and R-UI-DET; decisions 23, 24, 27 and 29). The UiInput phase (after
// ApplyInput, ADR 0028's ledger) calls ProcessInput with the tick's latched step input; it updates the interaction state
// (UiInteractionState: focus, hover, press, drag), writes what the widgets change (a toggle's IsOn, a slider's Value, a scroll
// view's ScrollPosition) to the scene through Scene/UiWidgets.h, and queues the UI events it raises. The UiEvents phase
// (after StartFlush) takes the queue (TakeEvents) and the script engine delivers it, bubbling (ScriptEngine::DispatchUiEvents);
// a Simulate session, which runs no UiEvents phase, takes and drops the queue right after UiInput. SetFocus (the Ui script
// module's Ui.SetFocus) changes focus at once, checked and revealed against a layout of the scene as it is at the call, and
// queues its events behind those already queued. The state and the queue are session state outside the scene:
// AppendStateHash is the state hash's ui block, and a scene replacement resets them (ResetInteraction) with the layouts.

namespace Engine {

	class Scene;
	class XXH64Hasher;

	class UiSystem
	{
	public:
		UiSystem() = default;
		~UiSystem() = default;

		UiSystem(const UiSystem&) = delete;
		UiSystem& operator=(const UiSystem&) = delete;

		// The UiLayout phase, the session's start and its scene replacements (see the file comment): ComputeUiLayout(scene,
		// sceneSize) becomes the step's layout and the current one.
		void LayoutStep(const Scene& scene, glm::uvec2 sceneSize);
		// The FrameUiLayout phase: ComputeUiLayout(scene, sceneSize) becomes the frame's layout and the current one.
		void LayoutFrame(const Scene& scene, glm::uvec2 sceneSize);

		// The current layout (see the file comment); an empty table before the first layout (a session's: its Activate).
		[[nodiscard]] const UiLayoutTable& GetLayout() const;
		[[nodiscard]] const UiLayoutTable& GetStepLayout() const { return m_StepLayout; }
		[[nodiscard]] const UiLayoutTable& GetFrameLayout() const { return m_FrameLayout; }
		// True when the frame's layout is the current one (a FrameUiLayout ran after the last UiLayout).
		[[nodiscard]] bool IsFrameLayoutCurrent() const { return m_FrameLayoutCurrent; }

		// --- Interaction (C22.2; stream A, the widgets' behaviours from Scene/UiWidgets.h) -------------------------------

		// The UiInput phase (decision 24): against GetLayout(), a layout of `scene`, in this order, appending each event to
		// the queue as it is raised:
		//   1. Stale state: an id of the state that is not an element of the layout is cleared, and so are a Focused that is
		//      no longer focusable and a Pressed or Dragged no longer interactable; a cleared Focused or Hovered whose entity
		//      the scene still holds (not pending destruction) raises Focus or Hover (false) at it. When the layout holds no
		//      canvas, the rest of the state then returns to its default (DragOrigin and DragStart zero, Stick None,
		//      PointerOverUi false) and the phase ends here, reading no input: a scene without a canvas keeps an empty state
		//      whatever the pointer, the wheel, the stick and the bindings do (Roadmap M22: scenes without UI keep their hash).
		//   2. Hover: HitTestUi at input.Pointer; PointerOverUi is whether a target was hit; the hovered widget is the hit's
		//      widget when it is interactable, else none. A change raises Hover (false) at the old one, then (true) at the new.
		//   3. Press (PointerPressed) on the hovered widget: it becomes Pressed; a focusable one (IsUiFocusable) takes focus
		//      (Focus false at the old focus, true at it), while one whose Focusable is off leaves focus as it is, so a HUD
		//      button that is not Focusable is clicked without taking keyboard focus; a Slider takes the value at the pointer
		//      (SetUiSliderFromPointer) and becomes Dragged with DragOrigin the pointer and DragStart zero; a ScrollView becomes
		//      Dragged with DragOrigin the pointer and DragStart its ScrollPosition as read.
		//   4. Drag (PointerDown, Dragged set, and not the press's own step): a Slider takes the value at the pointer; a
		//      ScrollView follows the pointer (DragUiScrollView with the pointer's move from DragOrigin).
		//   5. Release (PointerReleased, or Pressed set while the pointer is not down): when the hovered widget is Pressed it
		//      is activated (ActivateUiWidget: a Button's Click, a Toggle's flip); then Pressed and Dragged are cleared.
		//   Whenever Dragged is cleared (here or in step 1), DragOrigin and DragStart return to zero.
		//   6. Wheel (a non-zero Scroll) over a target: the innermost interactable scroll view at or above it
		//      (FindUiScrollView) scrolls by (Scroll.x, -Scroll.y) times its ScrollStep (ScrollUiScrollView).
		//   7. Stick: GetUiStickDirection(input.Stick) different from the state's Stick and not None adds that direction to
		//      the navigation commands; the state's Stick becomes it.
		//   8. Navigation, in the order Next, Previous, Up, Down, Left, Right: a direction is first offered to the focused
		//      element when it is a Slider (StepUiSlider) or a ScrollView (StepUiScrollView), which may consume it; otherwise
		//      FindUiNavigationTarget (a direction) or FindNextUiFocus (Next, Previous) picks the element that takes focus
		//      (Focus false at the old, true at the new), and the scroll views holding it reveal it (RevealInUiScrollViews)
		//      against a layout of `scene` as it is then (ComputeUiLayout at GetLayout()'s scene size), so a scroll position this
		//      step already changed (the wheel, a drag, a step or an earlier reveal) is the one the reveal moves from.
		//      With nothing focused, both pick the layout's default focus (a canvas's DefaultFocus, Scene/UiInteraction.h)
		//      or nothing, so a command does nothing until something has focus or a canvas names its DefaultFocus.
		//   9. Submit: the focused element is activated (ActivateUiWidget); nothing when none is focused.
		//  10. Cancel: Cancel at the focused element, else at the last canvas of the layout (its entity), else none.
		// Writes go to `scene`'s components only (never its structure).
		void ProcessInput(Scene& scene, const UiInputFrame& input);

		// The interaction state (decision 24), the default before the first UiInput and after ResetInteraction.
		[[nodiscard]] const UiInteractionState& GetInteraction() const { return m_Interaction; }

		// Ui.SetFocus (decision 28, amended by the review of W22.2): focuses `element`, an element focusable (IsUiFocusable)
		// in a layout of `scene` as it is at the call (ComputeUiLayout at GetLayout()'s scene size, never stored), or clears
		// focus with a null id; Focus (false) at the old focus and (true) at the new are queued (nothing when it does not
		// change), and the scroll views holding the new focus reveal it against that layout. So a menu a callback has just
		// enabled, created or moved takes focus in that callback, and a scroll position written since the last layout is the
		// one the reveal moves from; layout is a pure function of the scene, so this stays deterministic. Before the first
		// layout there is no scene size, so nothing is laid out (a session lays its scene out before any script runs). Errors:
		// InvalidArgument for an id that is not focusable, naming why (not laid out, hidden, no widget, not interactable, not
		// focusable or no area); nothing changes then.
		[[nodiscard]] Status SetFocus(Scene& scene, UUID element);

		// SetFocus's check, changing nothing: whether SetFocus(scene, element) would change focus (false for the current
		// focus), or the InvalidArgument it returns for an element it refuses. The Ui module asks first, so a call that
		// changes nothing reports no mutation (decision 28).
		[[nodiscard]] Result<bool> CheckFocus(const Scene& scene, UUID element) const;

		// The UiEvents phase: the queued events, in the order queued, leaving the queue empty.
		[[nodiscard]] std::vector<UiEvent> TakeEvents();
		[[nodiscard]] size_t GetQueuedEventCount() const { return m_Events.size(); }

		// A scene replacement (Scene.Load, Scene.Reload): the interaction state returns to its default and the queue is
		// emptied, before the replacement's LayoutStep.
		void ResetInteraction();

		// The state hash's ui block (decision 29; ADR 0028's ledger: after the physics2d block): nothing while the
		// interaction state IsEmpty and the event queue is empty, so every scene without a canvas (which keeps both empty)
		// hashes as before; otherwise the 8 ASCII bytes "UiState1" (as UpdateU64 of their little-endian value), then Focused,
		// Hovered, Pressed and Dragged (UUID values), DragOrigin.x, DragOrigin.y, DragStart.x and DragStart.y (each float's
		// bit pattern, zero-extended), Stick and PointerOverUi (as integers), then the queue: its event count, then each
		// event's Kind, Target (its UUID value), Flag, HasNumber, Number (its bit pattern), Vector.x and Vector.y (their bit
		// patterns), in the order queued, each value with UpdateU64. The queue crosses a tick boundary when a script calls
		// Ui.SetFocus after the tick's UiEvents phase: its events reach their callbacks at the next UiEvents phase, so the
		// block holds them.
		void AppendStateHash(XXH64Hasher& hasher) const;
	private:
		UiLayoutTable m_StepLayout{};
		UiLayoutTable m_FrameLayout{};
		bool m_FrameLayoutCurrent = false;
		UiInteractionState m_Interaction{};
		std::vector<UiEvent> m_Events{};
	};

}
