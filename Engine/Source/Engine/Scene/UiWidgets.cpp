#include "EnginePCH.h"
#include "Engine/Scene/UiWidgets.h"

#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

// The widgets' behaviours (UiWidgets.h; Docs/Decisions/0033-m22-contract.md decisions 21, 24 and 26): the Button, Toggle and
// Slider functions (W22.2's stream B), then the ScrollView functions (stream C), which read a scroll view's sizes from the
// layout table, in canvas units, and write its ScrollPosition clamped as read. Values come from IEEE-exact float operations
// (+, -, *, /, comparisons, floor, ceil) on the components' values and the layout's rectangles, so interaction writes the
// same values in every configuration.

namespace Engine {

	namespace Utils {

		// The node of `part` in `table` when it is an element below `widget` (a node of `table`) in the same canvas: in the
		// widget's canvas, with the widget among its parent elements (UiLayoutNode::Parent, up to the canvas). Null for a null
		// part, an entity the table does not hold, the widget itself and any element that is not below it (UiLayout.h's
		// widget rules apply only to such a part). A canvas's nodes are contiguous and in canonical order, a parent before its
		// children, so each parent is found by searching back from the node that names it.
		static const UiLayoutNode* FindUiWidgetPart(const UiLayoutTable& table, const UiLayoutNode& widget, UUID part)
		{
			const UiCanvasLayout* canvas = FindUiCanvasLayout(table, widget.Canvas);
			if (!part.IsValid() || canvas == nullptr)
				return nullptr;
			const size_t first = canvas->FirstNode;
			const size_t end = std::min(table.Nodes.size(), first + static_cast<size_t>(canvas->NodeCount));
			size_t index = first;
			while (index < end && table.Nodes[index].Entity != part)
				++index;
			if (index == end)
				return nullptr;
			const UiLayoutNode& found = table.Nodes[index];
			UUID parent = found.Parent;
			while (parent != widget.Canvas)
			{
				if (parent == widget.Entity)
					return &found;
				while (index > first && table.Nodes[index].Entity != parent)
					--index;
				if (table.Nodes[index].Entity != parent)
					return nullptr;
				parent = table.Nodes[index].Parent;
			}
			return nullptr;
		}

		// A slider's step (UiSliderComponent.h): Step, or a tenth of Max - Min when Step is 0; with WholeNumbers rounded to a
		// whole number of at least 1, whatever Step says.
		static float GetUiSliderStep(const UiSliderComponent& slider)
		{
			const float step = slider.Step > 0.0f ? slider.Step : (slider.Max - slider.Min) / 10.0f;
			return slider.WholeNumbers ? std::max(1.0f, std::floor(step + 0.5f)) : step;
		}

		// Writes `value`, a value as read, to the slider of `entity` when its stored Value differs, and raises ValueChanged
		// when it differs from the value as read before.
		static void WriteUiSliderValue(Entity entity, float value, std::vector<UiEvent>& events)
		{
			UiSliderComponent& slider = entity.GetComponent<UiSliderComponent>();
			const float before = ReadUiSliderValue(slider);
			if (slider.Value != value)
				slider.Value = value;
			if (value != before)
				events.push_back(UiEvent{ .Kind = UiEventKind::ValueChanged, .Target = entity.GetUUID(), .HasNumber = true, .Number = value });
		}

	}

	void ActivateUiWidget(Scene& scene, UUID element, std::vector<UiEvent>& events)
	{
		const Entity entity = scene.FindEntityByID(element);
		if (!entity)
			return;
		if (const UiButtonComponent* button = entity.TryGetComponent<UiButtonComponent>())
		{
			if (button->Interactable)
				events.push_back(UiEvent{ .Kind = UiEventKind::Click, .Target = element });
		}
		else if (UiToggleComponent* toggle = entity.TryGetComponent<UiToggleComponent>(); toggle != nullptr && toggle->Interactable)
		{
			toggle->IsOn = !toggle->IsOn;
			events.push_back(UiEvent{ .Kind = UiEventKind::ValueChanged, .Target = element, .Flag = toggle->IsOn });
		}
	}

	float ReadUiSliderValue(const UiSliderComponent& slider)
	{
		// Clamped to [Min, Max] without std::clamp, which needs Min <= Max: a range a script left inverted, or a NaN value,
		// reads Min.
		const float value = std::max(slider.Min, std::min(slider.Value, slider.Max));
		if (!slider.WholeNumbers)
			return value;
		const float lowest = std::ceil(slider.Min);
		const float highest = std::floor(slider.Max);
		if (!(lowest <= highest))
			return slider.Min;
		return std::max(lowest, std::min(std::floor(value + 0.5f), highest));
	}

	float GetUiSliderPosition(const UiSliderComponent& slider)
	{
		if (!(slider.Min < slider.Max))
			return 0.0f;
		// The value as read lies in [Min, Max], and rounding is monotonic, so the quotient lies in [0, 1].
		return (ReadUiSliderValue(slider) - slider.Min) / (slider.Max - slider.Min);
	}

	void SetUiSliderFromPointer(Scene& scene, const UiLayoutTable& table, UUID slider, glm::vec2 pointer, std::vector<UiEvent>& events)
	{
		const Entity entity = scene.FindEntityByID(slider);
		const UiLayoutNode* node = FindUiLayoutNode(table, slider);
		const UiSliderComponent* component = entity ? entity.TryGetComponent<UiSliderComponent>() : nullptr;
		if (component == nullptr || node == nullptr || !component->Interactable)
			return;
		// The slide rectangle: the Handle's parent element's, else the Fill's parent's, else the slider's own.
		const UiLayoutNode* area = node;
		const UiLayoutNode* part = Utils::FindUiWidgetPart(table, *node, component->Handle);
		if (part == nullptr)
			part = Utils::FindUiWidgetPart(table, *node, component->Fill);
		if (part != nullptr)
		{
			const UiLayoutNode* parent = FindUiLayoutNode(table, part->Parent);
			area = parent != nullptr ? parent : node;
		}
		// t along the slide rectangle from its Min edge (the left one when Horizontal, the bottom one when Vertical), clamped to
		// [0, 1]; a rectangle without length along the direction gives 0. The ends map to Min and Max exactly.
		const bool horizontal = component->Direction == UiLayoutDirection::Horizontal;
		const float length = horizontal ? area->Max.x - area->Min.x : area->Max.y - area->Min.y;
		const float along = horizontal ? pointer.x - area->Min.x : area->Max.y - pointer.y;
		const float t = length > 0.0f ? std::max(0.0f, std::min(along / length, 1.0f)) : 0.0f;
		UiSliderComponent target = *component;
		target.Value = t >= 1.0f ? component->Max : component->Min + t * (component->Max - component->Min);
		Utils::WriteUiSliderValue(entity, ReadUiSliderValue(target), events);
	}

	bool StepUiSlider(Scene& scene, UUID slider, UiDirection direction, std::vector<UiEvent>& events)
	{
		const Entity entity = scene.FindEntityByID(slider);
		const UiSliderComponent* component = entity ? entity.TryGetComponent<UiSliderComponent>() : nullptr;
		if (component == nullptr || !component->Interactable)
			return false;
		const bool horizontal = component->Direction == UiLayoutDirection::Horizontal;
		const bool towardMax = direction == (horizontal ? UiDirection::Right : UiDirection::Up);
		const bool towardMin = direction == (horizontal ? UiDirection::Left : UiDirection::Down);
		if (!towardMax && !towardMin)
			return false;
		const float current = ReadUiSliderValue(*component);
		const float step = Utils::GetUiSliderStep(*component);
		UiSliderComponent target = *component;
		target.Value = towardMax ? current + step : current - step;
		Utils::WriteUiSliderValue(entity, ReadUiSliderValue(target), events);
		return true;
	}

	// --- ScrollView (stream C) -----------------------------------------------------------------------------------------

	namespace {

		// A scroll view as the functions below read and write it: its component in the scene, its node in the layout table,
		// its canvas's scale and the two sizes its position is clamped by (ClampUiScrollPosition), in canvas units.
		struct UiScrollViewTarget
		{
			UUID Entity{};
			UiScrollViewComponent* Component = nullptr;
			const UiLayoutNode* Node = nullptr;
			float Scale = 1.0f;
			glm::vec2 ViewSize = glm::vec2(0.0f);
			glm::vec2 ContentSize = glm::vec2(0.0f);
		};

	}

	namespace Utils {

		// One axis of ClampUiScrollPosition: `position` in [0, max(0, range)]; a NaN, which no valid value produces, is 0.
		static float ClampUiScrollAxis(float position, float range)
		{
			if (!(position > 0.0f) || !(range > 0.0f))
				return 0.0f;
			return position < range ? position : range;
		}

		// The parent element of `node` in `table`, within its canvas; null for a top-level element (its parent is its canvas).
		static const UiLayoutNode* FindUiParentNode(const UiLayoutTable& table, const UiLayoutNode& node)
		{
			const UiLayoutNode* parent = FindUiLayoutNode(table, node.Parent);
			return parent != nullptr && parent->Canvas == node.Canvas ? parent : nullptr;
		}

		// Whether `node` is an element below `ancestor` (an element of `table`) in its canvas.
		static bool IsUiNodeBelow(const UiLayoutTable& table, const UiLayoutNode& node, UUID ancestor)
		{
			const UiLayoutNode* current = &node;
			for (size_t steps = 0; steps < table.Nodes.size(); ++steps)
			{
				current = FindUiParentNode(table, *current);
				if (current == nullptr)
					return false;
				if (current->Entity == ancestor)
					return true;
			}
			return false;
		}

		// The scroll view `scrollView` of `scene` as `table` lays it out; nothing when the scene holds no such entity with a
		// UiScrollView or the table does not hold it. A Content that is not an element below it in its canvas leaves nothing
		// to scroll (UiScrollViewComponent.h): its content size is then the view's.
		static std::optional<UiScrollViewTarget> FindUiScrollViewTarget(Scene& scene, const UiLayoutTable& table, UUID scrollView)
		{
			const Entity entity = scene.FindEntityByID(scrollView);
			UiScrollViewComponent* component = entity.IsValid() ? entity.TryGetComponent<UiScrollViewComponent>() : nullptr;
			const UiLayoutNode* node = FindUiLayoutNode(table, scrollView);
			const UiCanvasLayout* canvas = node != nullptr ? FindUiCanvasLayout(table, node->Canvas) : nullptr;
			if (component == nullptr || canvas == nullptr)
				return std::nullopt;
			UiScrollViewTarget target{ .Entity = scrollView, .Component = component, .Node = node, .Scale = canvas->Scale };
			target.ViewSize = (node->Max - node->Min) / canvas->Scale;
			target.ContentSize = target.ViewSize;
			if (const UiLayoutNode* content = FindUiLayoutNode(table, component->Content);
				content != nullptr && IsUiNodeBelow(table, *content, scrollView))
				target.ContentSize = (content->Max - content->Min) / canvas->Scale;
			return target;
		}

		// The scroll view's ScrollPosition as read.
		static glm::vec2 ReadUiScrollPosition(const UiScrollViewTarget& target)
		{
			return ClampUiScrollPosition(*target.Component, target.ViewSize, target.ContentSize, target.Component->ScrollPosition);
		}

		// Moves the scroll view to `position`, clamped: when that differs from its position as read, writes it and raises
		// Scroll with it. Returns whether it moved.
		static bool MoveUiScrollPosition(const UiScrollViewTarget& target, glm::vec2 position, std::vector<UiEvent>& events)
		{
			const glm::vec2 before = ReadUiScrollPosition(target);
			const glm::vec2 after = ClampUiScrollPosition(*target.Component, target.ViewSize, target.ContentSize, position);
			if (after == before)
				return false;
			target.Component->ScrollPosition = after;
			events.push_back(UiEvent{ .Kind = UiEventKind::Scroll, .Target = target.Entity, .Vector = after });
			return true;
		}

		// The least move, along one axis in scene pixels, of a rectangle [min, max] that puts it inside the view [viewMin,
		// viewMax]: its start on the view's when it is larger than the view or starts before it, its end on the view's when
		// it ends after it, else none.
		static float GetUiRevealShift(float min, float max, float viewMin, float viewMax)
		{
			if (max - min > viewMax - viewMin || min < viewMin)
				return min - viewMin;
			if (max > viewMax)
				return max - viewMax;
			return 0.0f;
		}

	}

	glm::vec2 ClampUiScrollPosition(const UiScrollViewComponent& scrollView, glm::vec2 viewSize, glm::vec2 contentSize, glm::vec2 position)
	{
		const glm::vec2 range = contentSize - viewSize;
		return glm::vec2(scrollView.Horizontal ? Utils::ClampUiScrollAxis(position.x, range.x) : 0.0f,
			scrollView.Vertical ? Utils::ClampUiScrollAxis(position.y, range.y) : 0.0f);
	}

	UUID FindUiScrollView(const Scene& scene, const UiLayoutTable& table, UUID element)
	{
		const UiLayoutNode* node = FindUiLayoutNode(table, element);
		// At most one step per node: a table from ComputeUiLayout has no cycle, and the bound keeps any other finite.
		for (size_t steps = 0; node != nullptr && steps < table.Nodes.size(); ++steps)
		{
			const ConstEntity entity = scene.FindEntityByID(node->Entity);
			const UiScrollViewComponent* scrollView = entity.IsValid() ? entity.TryGetComponent<UiScrollViewComponent>() : nullptr;
			if (scrollView != nullptr && scrollView->Interactable)
				return node->Entity;
			node = Utils::FindUiParentNode(table, *node);
		}
		return UUID();
	}

	bool ScrollUiScrollView(Scene& scene, const UiLayoutTable& table, UUID scrollView, glm::vec2 delta, std::vector<UiEvent>& events)
	{
		const std::optional<UiScrollViewTarget> target = Utils::FindUiScrollViewTarget(scene, table, scrollView);
		if (!target.has_value())
			return false;
		return Utils::MoveUiScrollPosition(*target, Utils::ReadUiScrollPosition(*target) + delta, events);
	}

	void DragUiScrollView(Scene& scene, const UiLayoutTable& table, UUID scrollView, glm::vec2 start, glm::vec2 pointerMove,
		std::vector<UiEvent>& events)
	{
		const std::optional<UiScrollViewTarget> target = Utils::FindUiScrollViewTarget(scene, table, scrollView);
		if (target.has_value())
			Utils::MoveUiScrollPosition(*target, start - pointerMove / target->Scale, events);
	}

	bool StepUiScrollView(Scene& scene, const UiLayoutTable& table, UUID scrollView, UiDirection direction, std::vector<UiEvent>& events)
	{
		const std::optional<UiScrollViewTarget> target = Utils::FindUiScrollViewTarget(scene, table, scrollView);
		if (!target.has_value())
			return false;
		const float step = target->Component->ScrollStep;
		glm::vec2 delta(0.0f);
		switch (direction)
		{
			case UiDirection::Up:
				if (!target->Component->Vertical)
					return false;
				delta.y = -step;
				break;
			case UiDirection::Down:
				if (!target->Component->Vertical)
					return false;
				delta.y = step;
				break;
			case UiDirection::Left:
				if (!target->Component->Horizontal)
					return false;
				delta.x = -step;
				break;
			case UiDirection::Right:
				if (!target->Component->Horizontal)
					return false;
				delta.x = step;
				break;
		}
		Utils::MoveUiScrollPosition(*target, Utils::ReadUiScrollPosition(*target) + delta, events);
		return true;
	}

	void RevealInUiScrollViews(Scene& scene, const UiLayoutTable& table, UUID element, std::vector<UiEvent>& events)
	{
		const UiLayoutNode* node = FindUiLayoutNode(table, element);
		if (node == nullptr)
			return;
		glm::vec2 min = node->Min;
		glm::vec2 max = node->Max;
		// The element and its ancestors below the scroll view the walk has reached: its Content holds the element when it is
		// one of them.
		std::vector<UUID> below{ element };
		for (size_t steps = 0; steps < table.Nodes.size(); ++steps)
		{
			node = Utils::FindUiParentNode(table, *node);
			if (node == nullptr)
				break;
			const std::optional<UiScrollViewTarget> target = Utils::FindUiScrollViewTarget(scene, table, node->Entity);
			if (target.has_value() && std::ranges::find(below, target->Component->Content) != below.end())
			{
				// The least scroll that puts [min, max] inside the view, per axis, in scene pixels (the element's top-left
				// corner first when it is larger than the view); the clamp keeps it to the axes that scroll.
				const glm::vec2 shift(Utils::GetUiRevealShift(min.x, max.x, target->Node->Min.x, target->Node->Max.x),
					Utils::GetUiRevealShift(min.y, max.y, target->Node->Min.y, target->Node->Max.y));
				const glm::vec2 before = Utils::ReadUiScrollPosition(*target);
				const glm::vec2 after = ClampUiScrollPosition(*target->Component, target->ViewSize, target->ContentSize,
					before + shift / target->Scale);
				Utils::MoveUiScrollPosition(*target, after, events);
				// The content, and the element with it, moves by the change, opposite to the scroll position.
				const glm::vec2 moved = (after - before) * target->Scale;
				min -= moved;
				max -= moved;
			}
			below.push_back(node->Entity);
		}
	}

}
