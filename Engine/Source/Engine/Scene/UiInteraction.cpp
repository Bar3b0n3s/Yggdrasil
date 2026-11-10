#include "EnginePCH.h"
#include "Engine/Scene/UiInteraction.h"

#include "Engine/Core/DetMath.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
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
			// C22.3 (decision 45): the fifth widget.
			if (const UiTextInputComponent* textInput = entity.TryGetComponent<UiTextInputComponent>())
				return { .Type = UiWidgetType::TextInput, .Interactable = textInput->Interactable, .Focusable = textInput->Focusable };
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

		// C22.3 (decision 50): HitTestUi's rule within one canvas of the table: the last pointer target of its node range over
		// `point` (its canvas's pixels), and its widget.
		static UiHit HitTestUiCanvas(const Scene& scene, const UiLayoutTable& table, const UiCanvasLayout& canvas, glm::vec2 point)
		{
			// A table of the scene's layout holds its canvases' node ranges; a range past the nodes is cut to them.
			const size_t first = std::min<size_t>(canvas.FirstNode, table.Nodes.size());
			const size_t last = std::min<size_t>(first + canvas.NodeCount, table.Nodes.size());
			for (size_t index = last; index > first; --index)
			{
				const UiLayoutNode& node = table.Nodes[index - 1];
				if (!IsUiPointOverNode(node, point) || !IsUiPointerTarget(scene, node))
					continue;
				return UiHit{ .Target = node.Entity, .Widget = FindUiWidgetAtOrAbove(scene, table, node) };
			}
			return {};
		}

		// The canvas of the table whose node range holds `node` (a node of the table), or nullptr.
		static const UiCanvasLayout* FindUiNodeCanvas(const UiLayoutTable& table, const UiLayoutNode& node)
		{
			const auto index = static_cast<size_t>(&node - table.Nodes.data());
			for (const UiCanvasLayout& canvas : table.Canvases)
			{
				if (index >= canvas.FirstNode && index - canvas.FirstNode < canvas.NodeCount)
					return &canvas;
			}
			return nullptr;
		}

		// The render space of a table's node for directional navigation (decision 50): every Screen canvas's nodes share one
		// (the null id), and each World canvas's nodes have their own (its canvas's id).
		static UUID GetUiRenderSpace(const UiLayoutTable& table, const UiLayoutNode& node)
		{
			const UiCanvasLayout* canvas = FindUiNodeCanvas(table, node);
			return canvas != nullptr && canvas->RenderMode == UiCanvasRenderMode::World ? canvas->Canvas : UUID{};
		}

		// pi / 180 in double, as the projection of render extraction converts the field of view (RenderSnapshot.cpp's).
		constexpr double UiRayDegreesToRadians = 0.017453292519943295769236907684886;

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
			case UiWidgetType::TextInput:  return "TextInput";
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
			case UiEventKind::TextChanged:  return "TextChanged";
			case UiEventKind::Submit:       return "Submit";
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
			case UiEventKind::TextChanged:  return "OnUiTextChanged";
			case UiEventKind::Submit:       return "OnUiSubmit";
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
		// The table is in draw order, canvas after canvas, so the topmost target is the last one over the point among the
		// Screen canvases' nodes (C22.3: a World canvas's are in its own pixels).
		for (auto canvas = table.Canvases.rbegin(); canvas != table.Canvases.rend(); ++canvas)
		{
			if (canvas->RenderMode == UiCanvasRenderMode::World)
				continue;
			if (const UiHit hit = Utils::HitTestUiCanvas(scene, table, *canvas, point); hit.Target.IsValid())
				return hit;
		}
		return {};
	}

	UUID FindUiNavigationTarget(const Scene& scene, const UiLayoutTable& table, UUID from, UiDirection direction)
	{
		const UiLayoutNode* origin = from.IsValid() ? FindUiLayoutNode(table, from) : nullptr;
		if (origin == nullptr)
			return Utils::FindUiDefaultFocus(scene, table);

		const glm::vec2 centre = Utils::GetUiNodeCentre(*origin);
		// C22.3 (decision 50): only within `from`'s render space, whose rectangles share its pixels.
		const UUID space = Utils::GetUiRenderSpace(table, *origin);
		UUID best{};
		float bestScore = 0.0f;
		for (const UiLayoutNode& node : table.Nodes)
		{
			if (node.Entity == from || !Utils::IsUiFocusableNode(scene, node) || Utils::GetUiRenderSpace(table, node) != space)
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

	// World canvases (C22.3, Docs/Decisions/0033-m22-contract.md decision 50). The ray and the plane's intersection are
	// computed in double from the float inputs (IEEE-exact operations, square roots and DetMath only), then rounded to float
	// once, so a canvas point that lands on a whole canvas pixel in exact arithmetic lands on it here too.

	std::optional<UiPointerRay> ComputeUiPointerRay(const Scene& scene, glm::vec2 pointer, glm::uvec2 sceneSize)
	{
		ENGINE_CORE_ASSERT(sceneSize.x >= 1 && sceneSize.y >= 1, "ComputeUiPointerRay needs a scene image of at least 1 x 1, got {} x {}", sceneSize.x,
			sceneSize.y);
		const ConstEntity camera = FindPrimaryCamera(scene);
		if (!camera.IsValid() || sceneSize.x == 0 || sceneSize.y == 0)
			return std::nullopt;
		const CameraComponent& component = camera.GetComponent<CameraComponent>();
		// The simulated pose: the world matrix the last TransformSystem::Update gave the camera (the chain walk before the first
		// one), its rotation without its scale, as render extraction's view matrix takes it (the identity for a matrix that
		// does not decompose).
		const WorldTransformComponent* world = camera.TryGetComponent<WorldTransformComponent>();
		const glm::mat4 matrix = world != nullptr ? world->Matrix : TransformSystem::ComputeWorldMatrix(camera);
		const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(matrix);
		const glm::quat orientation = decomposed.has_value() ? decomposed->Rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		const glm::dmat3 rotation = glm::mat3_cast(glm::dquat{ static_cast<double>(orientation.w), static_cast<double>(orientation.x),
			static_cast<double>(orientation.y), static_cast<double>(orientation.z) });
		const glm::dvec3 position(matrix[3]);

		// The pointer in normalized device coordinates (+y up) and the projection's aspect.
		const glm::dvec2 size(static_cast<double>(sceneSize.x), static_cast<double>(sceneSize.y));
		const double x = 2.0 * static_cast<double>(pointer.x) / size.x - 1.0;
		const double y = 1.0 - 2.0 * static_cast<double>(pointer.y) / size.y;
		const double aspect = size.x / size.y;
		UiPointerRay ray;
		if (component.Projection == ProjectionType::Perspective)
		{
			const double tangent = DetMath::Tan(static_cast<double>(component.VerticalFov) * Utils::UiRayDegreesToRadians * 0.5);
			const glm::dvec3 view(x * aspect * tangent, y * tangent, -1.0);
			ray.Origin = glm::vec3(position);
			ray.Direction = glm::vec3(rotation * glm::normalize(view));
			// The near plane lies NearClip along the camera's -Z, so NearClip / c along the ray, c = 1 / |view| its cosine with
			// -Z; the far plane is at infinity (MaxDistance keeps its default).
			ray.MinDistance = static_cast<float>(static_cast<double>(component.NearClip) * glm::length(view));
			return ray;
		}
		const double halfHeight = static_cast<double>(component.OrthographicSize);
		ray.Origin = glm::vec3(position + rotation * glm::dvec3(x * aspect * halfHeight, y * halfHeight, -static_cast<double>(component.NearClip)));
		ray.Direction = glm::vec3(rotation * glm::dvec3(0.0, 0.0, -1.0));
		// From the near plane, where the ray starts, to the far plane.
		ray.MaxDistance = static_cast<float>(static_cast<double>(component.FarClip) - static_cast<double>(component.NearClip));
		return ray;
	}

	std::optional<UiCanvasPointer> MapUiPointerToCanvas(const UiCanvasLayout& canvas, glm::vec2 point, const std::optional<UiPointerRay>& ray)
	{
		if (canvas.RenderMode != UiCanvasRenderMode::World)
			return UiCanvasPointer{ .Point = point, .Distance = 0.0f };
		if (!ray.has_value())
			return std::nullopt;
		// The canvas's plane: its pixel (0, 0) at `corner` and its x and y pixel axes `u` and `v` in world space (World's
		// columns), so a point of the plane is corner + a u + b v at canvas pixel (a, b).
		const glm::dvec3 corner(canvas.World[3]);
		const glm::dvec3 u(canvas.World[0]);
		const glm::dvec3 v(canvas.World[1]);
		const glm::dvec3 normal = glm::cross(u, v);
		const glm::dvec3 origin(ray->Origin);
		const glm::dvec3 direction(ray->Direction);
		// The ray meets the plane at origin + t direction, from either side; none when it runs parallel to it.
		const double facing = glm::dot(direction, normal);
		if (!(facing != 0.0) || !std::isfinite(facing))
			return std::nullopt;
		const double t = glm::dot(corner - origin, normal) / facing;
		if (!(t >= 0.0) || !std::isfinite(t))
			return std::nullopt;
		// Where the camera draws (W22.3's review): a plane met outside the ray's clip range is clipped away there.
		if (!(t >= static_cast<double>(ray->MinDistance) && t <= static_cast<double>(ray->MaxDistance)))
			return std::nullopt;
		// The hit's canvas pixel: the least-squares solution of corner + a u + b v = hit, exact for a point of the plane, through
		// the axes' Gram matrix (u and v need not be orthogonal: a canvas under a non-uniformly scaled, rotated parent is sheared).
		const glm::dvec3 offset = origin + direction * t - corner;
		const double uu = glm::dot(u, u);
		const double uv = glm::dot(u, v);
		const double vv = glm::dot(v, v);
		const double determinant = uu * vv - uv * uv;
		if (!(determinant > 0.0) || !std::isfinite(determinant))
			return std::nullopt;
		const double ou = glm::dot(offset, u);
		const double ov = glm::dot(offset, v);
		const glm::dvec2 pixel((ou * vv - ov * uv) / determinant, (ov * uu - ou * uv) / determinant);
		return UiCanvasPointer{ .Point = glm::vec2(pixel), .Distance = static_cast<float>(t) };
	}

	UiPointerHit HitTestUiPointer(const Scene& scene, const UiLayoutTable& table, glm::vec2 point, const std::optional<UiPointerRay>& ray)
	{
		// A Screen canvas's target under the pointer wins: Screen canvases draw over the world.
		for (auto canvas = table.Canvases.rbegin(); canvas != table.Canvases.rend(); ++canvas)
		{
			if (canvas->RenderMode == UiCanvasRenderMode::World)
				continue;
			if (const UiHit hit = Utils::HitTestUiCanvas(scene, table, *canvas, point); hit.Target.IsValid())
				return UiPointerHit{ .Hit = hit, .Canvas = canvas->Canvas, .CanvasPoint = point };
		}
		if (!ray.has_value())
			return {};
		// Otherwise the nearest World canvas along the ray with a target at its point, the later in draw order on a tie. A
		// point outside the canvas's rectangle hits nothing on it: the canvas is drawn only inside it (UiPass.h).
		UiPointerHit best{};
		float bestDistance = 0.0f;
		for (const UiCanvasLayout& canvas : table.Canvases)
		{
			if (canvas.RenderMode != UiCanvasRenderMode::World)
				continue;
			const std::optional<UiCanvasPointer> mapped = MapUiPointerToCanvas(canvas, point, ray);
			if (!mapped.has_value() || (best.Hit.Target.IsValid() && mapped->Distance > bestDistance))
				continue;
			const glm::vec2 size(static_cast<float>(canvas.SceneSize.x), static_cast<float>(canvas.SceneSize.y));
			if (!(mapped->Point.x >= 0.0f && mapped->Point.x < size.x && mapped->Point.y >= 0.0f && mapped->Point.y < size.y))
				continue;
			const UiHit hit = Utils::HitTestUiCanvas(scene, table, canvas, mapped->Point);
			if (!hit.Target.IsValid())
				continue;
			best = UiPointerHit{ .Hit = hit, .Canvas = canvas.Canvas, .CanvasPoint = mapped->Point };
			bestDistance = mapped->Distance;
		}
		return best;
	}

}
