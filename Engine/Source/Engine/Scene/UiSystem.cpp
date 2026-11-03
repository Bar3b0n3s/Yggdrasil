#include "EnginePCH.h"
#include "Engine/Scene/UiSystem.h"

#include "Engine/Core/Hash.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiWidgets.h"

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

// The play session's UI (UiSystem.h; Docs/Decisions/0033-m22-contract.md decisions 3, 24, 28 and 29): the layouts, the
// UiInput phase's state machine in the header's ten steps, Ui.SetFocus, the event queue and the ui state-hash block. What the
// input reaches is decided here (Scene/UiInteraction.h); what it does to a widget is Scene/UiWidgets.h's.

namespace Engine {

	namespace Utils {

		// An event raised at `target` (UiEvent's members of its kind).
		static void QueueUiEvent(std::vector<UiEvent>& events, UiEventKind kind, UUID target, bool flag = false)
		{
			events.push_back(UiEvent{ .Kind = kind, .Target = target, .Flag = flag });
		}

		// Focus or Hover (false) at an id the state drops, only while the scene still holds its entity (not pending
		// destruction): an element that is gone hears nothing (UiSystem.h, step 1).
		static void QueueUiLoss(const Scene& scene, std::vector<UiEvent>& events, UiEventKind kind, UUID lost)
		{
			if (lost.IsValid() && scene.FindEntityByID(lost).IsValid())
				QueueUiEvent(events, kind, lost, false);
		}

		// Dragged cleared with its origin and start (UiSystem.h: whenever Dragged is cleared, both return to zero).
		static void ClearUiDrag(UiInteractionState& state)
		{
			state.Dragged = UUID{};
			state.DragOrigin = glm::vec2(0.0f);
			state.DragStart = glm::vec2(0.0f);
		}

		// Focus moved to `focus` (null clears it): Focus (false) at the old focus while the scene holds it, then (true) at the
		// new one. Returns whether a new focus was taken, which navigation and Ui.SetFocus (not a pointer press) then reveal;
		// nothing when focus does not change.
		static bool MoveUiFocus(const Scene& scene, UiInteractionState& state, std::vector<UiEvent>& events, UUID focus)
		{
			if (focus == state.Focused)
				return false;
			const UUID previous = std::exchange(state.Focused, focus);
			QueueUiLoss(scene, events, UiEventKind::Focus, previous);
			if (!focus.IsValid())
				return false;
			QueueUiEvent(events, UiEventKind::Focus, focus, true);
			return true;
		}

		// A layout of `scene` as it is now, at the scene size `current` was laid out at (UiSystem.h: SetFocus and step 8's
		// reveal); before the first layout, which gives no scene size, `current` itself, empty, so nothing is laid out.
		static UiLayoutTable LayOutUiNow(const Scene& scene, const UiLayoutTable& current)
		{
			if (current.SceneSize.x == 0 || current.SceneSize.y == 0)
				return current;
			return ComputeUiLayout(scene, current.SceneSize);
		}

		// Ui.SetFocus's refusal of an element, naming why (FindUiFocusProblem).
		static Error MakeUiFocusError(std::string_view problem)
		{
			return Error(ErrorCode::InvalidArgument, std::format("cannot focus {}", problem))
				.WithHint("focus a laid-out, interactable and focusable UI widget (ui.tree lists each element's widget), or pass nil to "
						  "clear focus");
		}

		// Whether `node` lies below the element `ancestor` in its canvas (through UiLayoutNode::Parent; a top-level element's
		// Parent is its canvas, which is no node, so the walk ends at the canvas).
		static bool IsUiNodeBelow(const UiLayoutTable& table, const UiLayoutNode& node, UUID ancestor)
		{
			for (const UiLayoutNode* parent = FindUiLayoutNode(table, node.Parent); parent != nullptr;
				parent = FindUiLayoutNode(table, parent->Parent))
			{
				if (parent->Entity == ancestor)
					return true;
			}
			return false;
		}

		// A scroll view's ScrollPosition as read (UiScrollViewComponent.h, ClampUiScrollPosition): clamped to its content's
		// overflow, the sizes being the two laid-out rectangles in canvas units. A Content that is not an element below the
		// scroll view leaves nothing to scroll: its size is then the view's, as UiWidgets.cpp's scroll functions read it (a
		// node's parent chain in the table never leaves its canvas, whose entity is no node). Zero for an element that is
		// not a laid-out scroll view.
		static glm::vec2 ReadUiScrollPosition(const Scene& scene, const UiLayoutTable& table, UUID element)
		{
			const ConstEntity entity = scene.FindEntityByID(element);
			const UiScrollViewComponent* scrollView = entity.IsValid() ? entity.TryGetComponent<UiScrollViewComponent>() : nullptr;
			const UiLayoutNode* view = FindUiLayoutNode(table, element);
			if (scrollView == nullptr || view == nullptr)
				return glm::vec2(0.0f);
			const UiCanvasLayout* canvas = FindUiCanvasLayout(table, view->Canvas);
			const float scale = canvas != nullptr ? canvas->Scale : 1.0f;
			const glm::vec2 viewSize = (view->Max - view->Min) / scale;
			glm::vec2 contentSize = viewSize;
			const UiLayoutNode* content = scrollView->Content.IsValid() ? FindUiLayoutNode(table, scrollView->Content) : nullptr;
			if (content != nullptr && IsUiNodeBelow(table, *content, element))
				contentSize = (content->Max - content->Min) / scale;
			return ClampUiScrollPosition(*scrollView, viewSize, contentSize, scrollView->ScrollPosition);
		}

		// The navigation command of a stick direction (None for None).
		static UiNavigation ToUiNavigation(UiStickDirection direction)
		{
			switch (direction)
			{
				case UiStickDirection::None:  return UiNavigation::None;
				case UiStickDirection::Up:    return UiNavigation::Up;
				case UiStickDirection::Down:  return UiNavigation::Down;
				case UiStickDirection::Left:  return UiNavigation::Left;
				case UiStickDirection::Right: return UiNavigation::Right;
			}
			return UiNavigation::None;
		}

		// Why `element` cannot take focus in `current`, a layout of the scene as it is at the call (Ui.SetFocus's refusal), or
		// empty when it can (IsUiFocusable).
		static std::string FindUiFocusProblem(const Scene& scene, const UiLayoutTable& current, UUID element)
		{
			const ConstEntity entity = scene.FindEntityByID(element);
			if (!entity.IsValid())
				return std::format("{} is not an entity of the running scene", element);
			const std::string path = scene.GetEntityPath(entity);
			const UiLayoutNode* node = FindUiLayoutNode(current, element);
			if (node == nullptr)
				return std::format("'{}' is not laid out: it is not a UI element (UiRect) below an enabled canvas", path);
			if (node->Hidden)
				return std::format("'{}' is hidden inside an off toggle's Graphic", path);
			if (GetUiWidgetType(scene, element) == UiWidgetType::None)
				return std::format("'{}' holds no widget (UiButton, UiToggle, UiSlider or UiScrollView)", path);
			if (!IsUiInteractable(scene, element))
				return std::format("'{}' is not interactable (its Interactable is false)", path);
			if (!IsUiFocusable(scene, current, element))
			{
				if (!(node->Max.x > node->Min.x) || !(node->Max.y > node->Min.y))
					return std::format("'{}' has no area (a zero width or height)", path);
				return std::format("'{}' is not focusable (its Focusable is false)", path);
			}
			return {};
		}

	}

	void UiSystem::LayoutStep(const Scene& scene, glm::uvec2 sceneSize)
	{
		m_StepLayout = ComputeUiLayout(scene, sceneSize);
		m_FrameLayoutCurrent = false;
	}

	void UiSystem::LayoutFrame(const Scene& scene, glm::uvec2 sceneSize)
	{
		m_FrameLayout = ComputeUiLayout(scene, sceneSize);
		m_FrameLayoutCurrent = true;
	}

	const UiLayoutTable& UiSystem::GetLayout() const
	{
		return m_FrameLayoutCurrent ? m_FrameLayout : m_StepLayout;
	}

	void UiSystem::ProcessInput(Scene& scene, const UiInputFrame& input)
	{
		const UiLayoutTable& table = GetLayout();
		UiInteractionState& state = m_Interaction;

		// 1. Stale state: ids the layout no longer holds, a focus that is no longer focusable (IsUiFocusable also needs the
		// element in the layout) and a press or drag that is no longer interactable.
		const auto isElement = [&table](UUID id)
		{
			return FindUiLayoutNode(table, id) != nullptr;
		};
		if (state.Focused.IsValid() && !IsUiFocusable(scene, table, state.Focused))
			Utils::QueueUiLoss(scene, m_Events, UiEventKind::Focus, std::exchange(state.Focused, UUID{}));
		if (state.Hovered.IsValid() && !isElement(state.Hovered))
			Utils::QueueUiLoss(scene, m_Events, UiEventKind::Hover, std::exchange(state.Hovered, UUID{}));
		if (state.Pressed.IsValid() && (!isElement(state.Pressed) || !IsUiInteractable(scene, state.Pressed)))
			state.Pressed = UUID{};
		if (state.Dragged.IsValid() && (!isElement(state.Dragged) || !IsUiInteractable(scene, state.Dragged)))
			Utils::ClearUiDrag(state);
		if (table.Canvases.empty())
		{
			// Without a canvas every id is gone by now; the rest returns to its default and no input is read, so the state
			// (and the ui block) stays empty whatever the pointer, the wheel, the stick and the bindings do.
			state = UiInteractionState{};
			return;
		}

		// 2. Hover: the widget the topmost target reaches, when it is interactable.
		const UiHit hit = HitTestUi(scene, table, input.Pointer);
		state.PointerOverUi = hit.Target.IsValid();
		const UUID hovered = hit.Widget.IsValid() && IsUiInteractable(scene, hit.Widget) ? hit.Widget : UUID{};
		if (hovered != state.Hovered)
		{
			Utils::QueueUiLoss(scene, m_Events, UiEventKind::Hover, std::exchange(state.Hovered, hovered));
			if (hovered.IsValid())
				Utils::QueueUiEvent(m_Events, UiEventKind::Hover, hovered, true);
		}

		// 3. Press on the hovered widget: focus for a focusable one only; a slider takes the value at the pointer and a scroll
		// view the pointer's drag.
		const bool pressedNow = input.PointerPressed && state.Hovered.IsValid();
		if (pressedNow)
		{
			state.Pressed = state.Hovered;
			Utils::ClearUiDrag(state);
			if (IsUiFocusable(scene, table, state.Pressed))
				static_cast<void>(Utils::MoveUiFocus(scene, state, m_Events, state.Pressed));
			switch (GetUiWidgetType(scene, state.Pressed))
			{
				case UiWidgetType::Slider:
					SetUiSliderFromPointer(scene, table, state.Pressed, input.Pointer, m_Events);
					state.Dragged = state.Pressed;
					state.DragOrigin = input.Pointer;
					break;
				case UiWidgetType::ScrollView:
					state.Dragged = state.Pressed;
					state.DragOrigin = input.Pointer;
					state.DragStart = Utils::ReadUiScrollPosition(scene, table, state.Pressed);
					break;
				case UiWidgetType::None:
				case UiWidgetType::Button:
				case UiWidgetType::Toggle:
					break;
			}
		}

		// 4. Drag, after the press's own step: the slider follows the pointer, the scroll view the pointer's move.
		if (input.PointerDown && state.Dragged.IsValid() && !pressedNow)
		{
			switch (GetUiWidgetType(scene, state.Dragged))
			{
				case UiWidgetType::Slider:
					SetUiSliderFromPointer(scene, table, state.Dragged, input.Pointer, m_Events);
					break;
				case UiWidgetType::ScrollView:
					DragUiScrollView(scene, table, state.Dragged, state.DragStart, input.Pointer - state.DragOrigin, m_Events);
					break;
				case UiWidgetType::None:
				case UiWidgetType::Button:
				case UiWidgetType::Toggle:
					break;
			}
		}

		// 5. Release: a click (or a toggle's flip) only over the widget the press began on. A press of this step that the
		// pointer still holds is not released by an earlier release edge of the same step (the step view's edges carry no
		// order, and the pointer being down says the press came last).
		const bool released = input.PointerReleased || (state.Pressed.IsValid() && !input.PointerDown);
		if (released && !(pressedNow && input.PointerDown))
		{
			if (state.Pressed.IsValid() && state.Pressed == state.Hovered)
				ActivateUiWidget(scene, state.Pressed, m_Events);
			state.Pressed = UUID{};
			Utils::ClearUiDrag(state);
		}

		// 6. Wheel over a target: the innermost interactable scroll view at or above it (y up-positive: a step down scrolls
		// down).
		if ((input.Scroll.x != 0.0f || input.Scroll.y != 0.0f) && hit.Target.IsValid())
		{
			const UUID scrolled = FindUiScrollView(scene, table, hit.Target);
			const ConstEntity entity = std::as_const(scene).FindEntityByID(scrolled);
			if (const UiScrollViewComponent* scrollView = entity.IsValid() ? entity.TryGetComponent<UiScrollViewComponent>() : nullptr)
				static_cast<void>(
					ScrollUiScrollView(scene, table, scrolled, glm::vec2(input.Scroll.x, -input.Scroll.y) * scrollView->ScrollStep, m_Events));
		}

		// 7. Stick: each new direction is one press.
		UiNavigation navigation = input.Navigation;
		const UiStickDirection stick = GetUiStickDirection(input.Stick);
		if (stick != state.Stick)
			navigation |= Utils::ToUiNavigation(stick);
		state.Stick = stick;

		// 8. Navigation in the order Next, Previous, Up, Down, Left, Right. A focused slider or scroll view may consume a
		// direction; with nothing focused both finders give the default focus or nothing. A new focus is revealed against the
		// scene as it is now, since this step's earlier writes (the wheel, a drag, a step, an earlier reveal) moved content
		// that `table` does not show.
		const auto navigate = [this, &scene, &state, &table](UUID target)
		{
			if (Utils::MoveUiFocus(scene, state, m_Events, target))
				RevealInUiScrollViews(scene, Utils::LayOutUiNow(scene, table), target, m_Events);
		};
		constexpr std::array<std::pair<UiNavigation, UiDirection>, 4> Directions = { std::pair{ UiNavigation::Up, UiDirection::Up },
			std::pair{ UiNavigation::Down, UiDirection::Down }, std::pair{ UiNavigation::Left, UiDirection::Left },
			std::pair{ UiNavigation::Right, UiDirection::Right } };
		for (const bool forward : { true, false })
		{
			if (HasFlag(navigation, forward ? UiNavigation::Next : UiNavigation::Previous))
				navigate(FindNextUiFocus(scene, table, state.Focused, forward));
		}
		for (const auto& [command, direction] : Directions)
		{
			if (!HasFlag(navigation, command))
				continue;
			if (state.Focused.IsValid())
			{
				const UiWidgetType focused = GetUiWidgetType(scene, state.Focused);
				if (focused == UiWidgetType::Slider && StepUiSlider(scene, state.Focused, direction, m_Events))
					continue;
				if (focused == UiWidgetType::ScrollView && StepUiScrollView(scene, table, state.Focused, direction, m_Events))
					continue;
			}
			const UUID target = FindUiNavigationTarget(scene, table, state.Focused, direction);
			if (target.IsValid())
				navigate(target);
		}

		// 9. Submit activates the focused widget; nothing without one.
		if (HasFlag(navigation, UiNavigation::Submit) && state.Focused.IsValid())
			ActivateUiWidget(scene, state.Focused, m_Events);

		// 10. Cancel at the focused element, else at the last canvas of the layout.
		if (HasFlag(navigation, UiNavigation::Cancel))
		{
			const UUID target = state.Focused.IsValid() ? state.Focused : table.Canvases.back().Canvas;
			if (std::as_const(scene).FindEntityByID(target).IsValid())
				Utils::QueueUiEvent(m_Events, UiEventKind::Cancel, target);
		}
	}

	Status UiSystem::SetFocus(Scene& scene, UUID element)
	{
		if (!element.IsValid())
		{
			static_cast<void>(Utils::MoveUiFocus(scene, m_Interaction, m_Events, element));
			return {};
		}
		// The scene as it is at the call: a script may have enabled, created, moved or scrolled UI since the last layout.
		const UiLayoutTable current = Utils::LayOutUiNow(scene, GetLayout());
		const std::string problem = Utils::FindUiFocusProblem(scene, current, element);
		if (!problem.empty())
			return std::unexpected(Utils::MakeUiFocusError(problem));
		if (Utils::MoveUiFocus(scene, m_Interaction, m_Events, element))
			RevealInUiScrollViews(scene, current, element, m_Events);
		return {};
	}

	Result<bool> UiSystem::CheckFocus(const Scene& scene, UUID element) const
	{
		if (element.IsValid())
		{
			const std::string problem = Utils::FindUiFocusProblem(scene, Utils::LayOutUiNow(scene, GetLayout()), element);
			if (!problem.empty())
				return std::unexpected(Utils::MakeUiFocusError(problem));
		}
		return element != m_Interaction.Focused;
	}

	std::vector<UiEvent> UiSystem::TakeEvents()
	{
		return std::exchange(m_Events, {});
	}

	void UiSystem::ResetInteraction()
	{
		m_Interaction = {};
		m_Events.clear();
	}

	void UiSystem::AppendStateHash(XXH64Hasher& hasher) const
	{
		if (m_Interaction.IsEmpty() && m_Events.empty())
			return;
		// "UiState1" as the little-endian value of its 8 ASCII bytes.
		constexpr uint64_t Marker = 0x3165746174536955ull;
		const auto floatBits = [](float value)
		{
			return static_cast<uint64_t>(std::bit_cast<uint32_t>(value));
		};
		const UiInteractionState& state = m_Interaction;
		hasher.UpdateU64(Marker);
		for (const UUID id : { state.Focused, state.Hovered, state.Pressed, state.Dragged })
			hasher.UpdateU64(id.GetValue());
		for (const float value : { state.DragOrigin.x, state.DragOrigin.y, state.DragStart.x, state.DragStart.y })
			hasher.UpdateU64(floatBits(value));
		hasher.UpdateU64(static_cast<uint64_t>(std::to_underlying(state.Stick)));
		hasher.UpdateU64(state.PointerOverUi ? 1 : 0);
		hasher.UpdateU64(static_cast<uint64_t>(m_Events.size()));
		for (const UiEvent& event : m_Events)
		{
			hasher.UpdateU64(static_cast<uint64_t>(std::to_underlying(event.Kind)));
			hasher.UpdateU64(event.Target.GetValue());
			hasher.UpdateU64(event.Flag ? 1 : 0);
			hasher.UpdateU64(event.HasNumber ? 1 : 0);
			hasher.UpdateU64(floatBits(event.Number));
			hasher.UpdateU64(floatBits(event.Vector.x));
			hasher.UpdateU64(floatBits(event.Vector.y));
		}
	}

}
