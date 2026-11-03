#include "EnginePCH.h"
#include "Engine/Scene/UiInteraction.h"

#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <cmath>
#include <cstddef>

// UI interaction (UiInteraction.h; Docs/Decisions/0033-m22-contract.md decisions 22 and 23): the names, the hit test of
// pointer targets, focusability and navigation, each a pure function of the scene's components and a layout table. Every
// comparison of a point with a clip converts both sides to double, which holds a float and an int32 exactly, so the result
// never depends on a rounding.

namespace Engine {

	namespace Utils {

		// The widget an element holds and its two flags; Type None (both flags false) for an entity without one or that the
		// scene does not hold (pending destruction included: FindEntityByID leaves it out).
		struct UiWidgetFlags
		{
			UiWidgetType Type = UiWidgetType::None;
			bool Interactable = false;
			bool Focusable = false;
		};

		static UiWidgetFlags ReadUiWidget(ConstEntity entity)
		{
			if (!entity.IsValid())
				return {};
			if (const UiButtonComponent* button = entity.TryGetComponent<UiButtonComponent>())
				return { .Type = UiWidgetType::Button, .Interactable = button->Interactable, .Focusable = button->Focusable };
			if (const UiToggleComponent* toggle = entity.TryGetComponent<UiToggleComponent>())
				return { .Type = UiWidgetType::Toggle, .Interactable = toggle->Interactable, .Focusable = toggle->Focusable };
			if (const UiSliderComponent* slider = entity.TryGetComponent<UiSliderComponent>())
				return { .Type = UiWidgetType::Slider, .Interactable = slider->Interactable, .Focusable = slider->Focusable };
			if (const UiScrollViewComponent* scrollView = entity.TryGetComponent<UiScrollViewComponent>())
				return { .Type = UiWidgetType::ScrollView, .Interactable = scrollView->Interactable, .Focusable = scrollView->Focusable };
			return {};
		}

		static UiWidgetFlags ReadUiWidget(const Scene& scene, UUID element)
		{
			return ReadUiWidget(scene.FindEntityByID(element));
		}

		// The focus rule of UiInteraction.h's file comment for a node of the table.
		static bool IsUiFocusableNode(const Scene& scene, const UiLayoutNode& node)
		{
			if (node.Hidden || !(node.Max.x > node.Min.x) || !(node.Max.y > node.Min.y))
				return false;
			const UiWidgetFlags widget = ReadUiWidget(scene, node.Entity);
			return widget.Type != UiWidgetType::None && widget.Interactable && widget.Focusable;
		}

		// A pointer target: a Visible element holding a widget, or a UiImage or UiText whose RaycastTarget is set.
		static bool IsUiPointerTarget(const Scene& scene, const UiLayoutNode& node)
		{
			if (!node.Visible)
				return false;
			const ConstEntity entity = scene.FindEntityByID(node.Entity);
			if (!entity.IsValid())
				return false;
			if (ReadUiWidget(entity).Type != UiWidgetType::None)
				return true;
			const UiImageComponent* image = entity.TryGetComponent<UiImageComponent>();
			const UiTextComponent* text = entity.TryGetComponent<UiTextComponent>();
			return (image != nullptr && image->RaycastTarget) || (text != nullptr && text->RaycastTarget);
		}

		// Whether `point` lies in the node's rectangle (Min <= p < Max on both axes) and, when it is Clipped, in its clip.
		static bool IsUiPointOverNode(const UiLayoutNode& node, glm::vec2 point)
		{
			if (!(node.Min.x <= point.x && point.x < node.Max.x && node.Min.y <= point.y && point.y < node.Max.y))
				return false;
			if (!node.Clipped)
				return true;
			const double x = point.x;
			const double y = point.y;
			return static_cast<double>(node.Clip.MinX) <= x && x < static_cast<double>(node.Clip.MaxX) && static_cast<double>(node.Clip.MinY) <= y
				&& y < static_cast<double>(node.Clip.MaxY);
		}

		// The nearest element at or above `node`, within its canvas (UiLayoutNode::Parent), that holds a widget; null when
		// none does. A top-level element's Parent is its canvas, which is no node, so the walk ends there.
		static UUID FindUiWidgetAtOrAbove(const Scene& scene, const UiLayoutTable& table, const UiLayoutNode& node)
		{
			for (const UiLayoutNode* current = &node; current != nullptr; current = FindUiLayoutNode(table, current->Parent))
			{
				if (ReadUiWidget(scene, current->Entity).Type != UiWidgetType::None)
					return current->Entity;
			}
			return {};
		}

		// The table's default focus (UiInteraction.h's file comment): the DefaultFocus of the last canvas in draw order whose
		// DefaultFocus is focusable; null when no canvas names one.
		static UUID FindUiDefaultFocus(const Scene& scene, const UiLayoutTable& table)
		{
			for (auto canvas = table.Canvases.rbegin(); canvas != table.Canvases.rend(); ++canvas)
			{
				const ConstEntity entity = scene.FindEntityByID(canvas->Canvas);
				const UiCanvasComponent* component = entity.IsValid() ? entity.TryGetComponent<UiCanvasComponent>() : nullptr;
				if (component == nullptr || !component->DefaultFocus.IsValid())
					continue;
				const UiLayoutNode* focus = FindUiLayoutNode(table, component->DefaultFocus);
				if (focus != nullptr && IsUiFocusableNode(scene, *focus))
					return focus->Entity;
			}
			return {};
		}

		static glm::vec2 GetUiNodeCentre(const UiLayoutNode& node)
		{
			return (node.Min + node.Max) * 0.5f;
		}

	}

	std::string_view UiWidgetTypeToString(UiWidgetType type)
	{
		switch (type)
		{
			case UiWidgetType::None:       return {};
			case UiWidgetType::Button:     return "Button";
			case UiWidgetType::Toggle:     return "Toggle";
			case UiWidgetType::Slider:     return "Slider";
			case UiWidgetType::ScrollView: return "ScrollView";
		}
		return {};
	}

	std::string_view UiEventKindToString(UiEventKind kind)
	{
		switch (kind)
		{
			case UiEventKind::Click:        return "Click";
			case UiEventKind::ValueChanged: return "ValueChanged";
			case UiEventKind::Focus:        return "Focus";
			case UiEventKind::Hover:        return "Hover";
			case UiEventKind::Cancel:       return "Cancel";
			case UiEventKind::Scroll:       return "Scroll";
		}
		return {};
	}

	std::string_view GetUiEventCallbackName(UiEventKind kind)
	{
		switch (kind)
		{
			case UiEventKind::Click:        return "OnUiClick";
			case UiEventKind::ValueChanged: return "OnUiValueChanged";
			case UiEventKind::Focus:        return "OnUiFocus";
			case UiEventKind::Hover:        return "OnUiHover";
			case UiEventKind::Cancel:       return "OnUiCancel";
			case UiEventKind::Scroll:       return "OnUiScroll";
		}
		return {};
	}

	UiWidgetType GetUiWidgetType(const Scene& scene, UUID element)
	{
		return Utils::ReadUiWidget(scene, element).Type;
	}

	bool IsUiInteractable(const Scene& scene, UUID element)
	{
		const Utils::UiWidgetFlags widget = Utils::ReadUiWidget(scene, element);
		return widget.Type != UiWidgetType::None && widget.Interactable;
	}

	bool IsUiFocusable(const Scene& scene, const UiLayoutTable& table, UUID element)
	{
		const UiLayoutNode* node = element.IsValid() ? FindUiLayoutNode(table, element) : nullptr;
		return node != nullptr && Utils::IsUiFocusableNode(scene, *node);
	}

	UiHit HitTestUi(const Scene& scene, const UiLayoutTable& table, glm::vec2 point)
	{
		// The table is in draw order, so the topmost target is the last one over the point.
		for (auto node = table.Nodes.rbegin(); node != table.Nodes.rend(); ++node)
		{
			if (!Utils::IsUiPointOverNode(*node, point) || !Utils::IsUiPointerTarget(scene, *node))
				continue;
			return UiHit{ .Target = node->Entity, .Widget = Utils::FindUiWidgetAtOrAbove(scene, table, *node) };
		}
		return {};
	}

	UUID FindUiNavigationTarget(const Scene& scene, const UiLayoutTable& table, UUID from, UiDirection direction)
	{
		const UiLayoutNode* origin = from.IsValid() ? FindUiLayoutNode(table, from) : nullptr;
		if (origin == nullptr)
			return Utils::FindUiDefaultFocus(scene, table);

		const glm::vec2 centre = Utils::GetUiNodeCentre(*origin);
		UUID best{};
		float bestScore = 0.0f;
		for (const UiLayoutNode& node : table.Nodes)
		{
			if (node.Entity == from || !Utils::IsUiFocusableNode(scene, node))
				continue;
			const glm::vec2 offset = Utils::GetUiNodeCentre(node) - centre;
			float along = 0.0f;
			float across = 0.0f;
			switch (direction)
			{
				case UiDirection::Up:
					along = -offset.y;
					across = std::abs(offset.x);
					break;
				case UiDirection::Down:
					along = offset.y;
					across = std::abs(offset.x);
					break;
				case UiDirection::Left:
					along = -offset.x;
					across = std::abs(offset.y);
					break;
				case UiDirection::Right:
					along = offset.x;
					across = std::abs(offset.y);
					break;
			}
			if (!(along > 0.0f))
				continue;
			// The first in draw order wins a tie: only a strictly smaller score replaces it.
			const float score = along + 2.0f * across;
			if (!best.IsValid() || score < bestScore)
			{
				best = node.Entity;
				bestScore = score;
			}
		}
		return best;
	}

	UUID FindNextUiFocus(const Scene& scene, const UiLayoutTable& table, UUID from, bool forward)
	{
		const UiLayoutNode* origin = from.IsValid() ? FindUiLayoutNode(table, from) : nullptr;
		if (origin == nullptr || !Utils::IsUiFocusableNode(scene, *origin))
			return Utils::FindUiDefaultFocus(scene, table);

		// Every other node once, in draw order from `from`, wrapping; `from` itself, focusable, ends the walk.
		const size_t count = table.Nodes.size();
		const auto start = static_cast<size_t>(origin - table.Nodes.data());
		for (size_t step = 1; step <= count; ++step)
		{
			const size_t index = forward ? (start + step) % count : (start + count - step) % count;
			if (Utils::IsUiFocusableNode(scene, table.Nodes[index]))
				return table.Nodes[index].Entity;
		}
		return {};
	}

	UiStickDirection GetUiStickDirection(glm::vec2 stick)
	{
		constexpr float Threshold = 0.5f;
		const float x = std::abs(stick.x);
		const float y = std::abs(stick.y);
		// The larger axis decides, x on a tie.
		if (x >= y)
		{
			if (!(x >= Threshold))
				return UiStickDirection::None;
			return stick.x > 0.0f ? UiStickDirection::Right : UiStickDirection::Left;
		}
		if (!(y >= Threshold))
			return UiStickDirection::None;
		return stick.y > 0.0f ? UiStickDirection::Up : UiStickDirection::Down;
	}

}
