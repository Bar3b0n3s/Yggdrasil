#include "EnginePCH.h"
#include "Engine/Scene/UiLayout.h"

#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiLayoutElementComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/UiWorldCanvas.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scene/UiContainerLayout.h"
#include "Engine/Scene/UiWidgets.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// UI layout (UiLayout.h; Docs/Decisions/0033-m22-contract.md decision 4). Two passes: a scan of the scene in canonical order
// finds the effectively enabled canvases and each one's elements (a parent before its children, siblings in order), then each
// canvas, in draw order, places its elements in that order, so a parent's rectangle and a container's placement of its
// children are known before the children are placed.
//
// The arithmetic is the file comment's rule in scene pixels: the rectangle rule of UiRectComponent.h is linear in its lengths,
// so placing an element in canvas units and multiplying by the canvas's scale is placing it in its parent's rectangle in
// scene pixels with Size and Position times the scale. A parent's rectangle is therefore the one its children are placed in
// (snapped when the canvas snaps) without a round trip through canvas units; only a container's size is divided by the scale,
// since its children's placement is in canvas units (UiContainerLayout.h).

namespace Engine {

	namespace {

		constexpr uint32_t NoUiElement = std::numeric_limits<uint32_t>::max();

		// An element the scan found, in its canvas's list (canonical order).
		struct UiScannedElement
		{
			ConstEntity Entity{};
			uint32_t Parent = NoUiElement; // its parent element's index in the list; NoUiElement: the canvas
			uint32_t Depth = 1;
			// Its child elements, linked in canonical order (a container places them).
			uint32_t FirstChild = NoUiElement;
			uint32_t LastChild = NoUiElement;
			uint32_t NextSibling = NoUiElement;
		};

		// A canvas the scan found, with its elements.
		struct UiScannedCanvas
		{
			ConstEntity Entity{};
			int32_t SortOrder = 0;
			std::vector<UiScannedElement> Elements{};
		};

		// What the scan hands an entity's descendants: the canvas they belong to and their parent element.
		struct UiScanFrame
		{
			UUID Entity{};
			uint32_t Canvas = NoUiElement;        // the index of the canvas in the scan's list; NoUiElement: none
			uint32_t ParentElement = NoUiElement; // NoUiElement: the canvas
			uint32_t ParentDepth = 0;             // the parent element's depth, 0 for the canvas
		};

		// The clip an element hands its descendants.
		struct UiInheritedClip
		{
			bool Clipped = false;
			UiClipRect Clip{};
		};

		// What the slider and toggle rules (C22.2, Docs/Decisions/0033-m22-contract.md decision 26) and the text input's
		// (C22.3, decision 48) do to one element of a canvas: a slider's Fill or Handle takes the anchors along its slider's
		// direction in place of its own, and an off toggle's Graphic, or the Placeholder of a text input whose Text is not
		// empty, is Hidden with its subtree.
		struct UiWidgetPartRule
		{
			bool Anchored = false;
			glm::length_t Axis = 0; // 0: x (a Horizontal slider), 1: y (a Vertical one)
			float AnchorMin = 0.0f;
			float AnchorMax = 0.0f;
			bool Hides = false;
		};

	}

	namespace Utils {

		static UiClipRect IntersectUiClipRects(const UiClipRect& a, const UiClipRect& b)
		{
			return UiClipRect{ .MinX = std::max(a.MinX, b.MinX), .MinY = std::max(a.MinY, b.MinY), .MaxX = std::min(a.MaxX, b.MaxX), .MaxY = std::min(a.MaxY, b.MaxY) };
		}

		// ceil(v - 0.5), clamped to plus or minus UiClipCoordinateLimit (a NaN fails the first comparison and takes the lower
		// limit), then converted: always in int32's range.
		static int32_t ToUiClipCoordinate(float v)
		{
			float coordinate = std::ceil(v - 0.5f);
			if (!(coordinate >= -UiClipCoordinateLimit))
				coordinate = -UiClipCoordinateLimit;
			else if (coordinate > UiClipCoordinateLimit)
				coordinate = UiClipCoordinateLimit;
			return static_cast<int32_t>(coordinate);
		}

		// PixelSnap: each component to floor(v + 0.5).
		static glm::vec2 SnapToScenePixels(glm::vec2 v)
		{
			return glm::vec2(std::floor(v.x + 0.5f), std::floor(v.y + 0.5f));
		}

		// The scan: every effectively enabled canvas in canonical order, each with its elements in canonical order. The
		// canonical order is depth first, so the frames of an entity's ancestors are on `ancestors` when it is reached: the
		// ones that are not are popped, down to its parent's.
		static std::vector<UiScannedCanvas> ScanUiCanvases(const Scene& scene)
		{
			std::vector<UiScannedCanvas> canvases;
			std::vector<UiScanFrame> ancestors;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const ConstEntity entity = scene.FindEntityByID(id);
				if (!entity.IsValid())
					continue;
				const UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
				while (!ancestors.empty() && ancestors.back().Entity != parent)
					ancestors.pop_back();
				const UiScanFrame inherited = ancestors.empty() ? UiScanFrame{} : ancestors.back();

				UiScanFrame frame{ .Entity = id };
				if (entity.HasComponent<HierarchyDisabledTag>())
				{
					// Effectively disabled: neither it nor its subtree, which is disabled too, is laid out.
				}
				else if (const UiCanvasComponent* canvas = entity.TryGetComponent<UiCanvasComponent>())
				{
					// A canvas, nested or not, starts a UI tree of its own.
					frame.Canvas = static_cast<uint32_t>(canvases.size());
					canvases.push_back(UiScannedCanvas{ .Entity = entity, .SortOrder = canvas->SortOrder });
				}
				else if (inherited.Canvas != NoUiElement)
				{
					frame.Canvas = inherited.Canvas;
					frame.ParentElement = inherited.ParentElement;
					frame.ParentDepth = inherited.ParentDepth;
					if (entity.HasComponent<UiRectComponent>())
					{
						std::vector<UiScannedElement>& elements = canvases[inherited.Canvas].Elements;
						const auto index = static_cast<uint32_t>(elements.size());
						elements.push_back(UiScannedElement{ .Entity = entity, .Parent = inherited.ParentElement, .Depth = inherited.ParentDepth + 1 });
						if (inherited.ParentElement != NoUiElement)
						{
							UiScannedElement& parentElement = elements[inherited.ParentElement];
							if (parentElement.LastChild == NoUiElement)
								parentElement.FirstChild = index;
							else
								elements[parentElement.LastChild].NextSibling = index;
							parentElement.LastChild = index;
						}
						frame.ParentElement = index;
						frame.ParentDepth = inherited.ParentDepth + 1;
					}
					// Without a UiRect the entity is transparent: its descendants keep its parent element.
				}
				ancestors.push_back(frame);
			}
			return canvases;
		}

		// UiRectComponent.h's rule in scene pixels: the parent's rectangle [parentMin, parentMax] is in scene pixels, and the
		// rectangle's lengths in canvas units are times `scale`.
		static void PlaceUiRect(const UiRectComponent& rect, glm::vec2 parentMin, glm::vec2 parentMax, float scale, glm::vec2& min, glm::vec2& max)
		{
			const glm::vec2 parentSize = parentMax - parentMin;
			const glm::vec2 anchorMin = parentMin + rect.AnchorMin * parentSize;
			const glm::vec2 anchorMax = parentMin + rect.AnchorMax * parentSize;
			const glm::vec2 anchorSize = anchorMax - anchorMin;
			const glm::vec2 size = glm::max(anchorSize + rect.Size * scale, glm::vec2(0.0f));
			const glm::vec2 pivot = anchorMin + rect.Pivot * anchorSize + rect.Position * scale;
			min = pivot - rect.Pivot * size;
			max = min + size;
		}

		// The slider, toggle and text-input rules of `canvas`'s elements (UiLayout.h), by element index; empty when none
		// applies, as in every canvas without a slider, an off toggle or a text input with text that names a part. A rule applies only to a part that is an element
		// below its widget in this canvas: found among the canvas's elements, with the widget on its chain of parent
		// elements (so neither the widget itself nor an element outside its subtree). When several rules reach one element
		// (a Fill that is also its slider's Handle, or a part of two nested sliders), the later in canonical order wins.
		static std::vector<UiWidgetPartRule> FindUiWidgetPartRules(const UiScannedCanvas& canvas)
		{
			std::vector<UiWidgetPartRule> rules;
			std::vector<std::pair<UUID, uint32_t>> indices; // the elements' ids, sorted, made at the first part
			const auto findPart = [&canvas, &rules, &indices](uint32_t widget, UUID part) -> UiWidgetPartRule*
			{
				if (!part.IsValid())
					return nullptr;
				if (indices.empty())
				{
					indices.reserve(canvas.Elements.size());
					for (uint32_t index = 0; index < canvas.Elements.size(); ++index)
						indices.emplace_back(canvas.Elements[index].Entity.GetUUID(), index);
					std::ranges::sort(indices);
					rules.resize(canvas.Elements.size());
				}
				const auto found = std::ranges::lower_bound(indices, part, {}, &std::pair<UUID, uint32_t>::first);
				if (found == indices.end() || found->first != part)
					return nullptr;
				for (uint32_t parent = canvas.Elements[found->second].Parent; parent != NoUiElement; parent = canvas.Elements[parent].Parent)
				{
					if (parent == widget)
						return &rules[found->second];
				}
				return nullptr;
			};
			for (uint32_t index = 0; index < canvas.Elements.size(); ++index)
			{
				const ConstEntity entity = canvas.Elements[index].Entity;
				if (const UiSliderComponent* slider = entity.TryGetComponent<UiSliderComponent>())
				{
					// Horizontal: Min at the left edge; Vertical: Min at the bottom edge, so the anchors count from 1 down; M38's
					// HorizontalReverse (decision 12): Min at the right edge, counting from 1 down along X.
					const float t = GetUiSliderPosition(*slider);
					const bool horizontal = slider->Direction == UiLayoutDirection::Horizontal;
					const glm::length_t axis = slider->Direction == UiLayoutDirection::Vertical ? 1 : 0;
					if (UiWidgetPartRule* fill = findPart(index, slider->Fill))
						*fill = UiWidgetPartRule{ .Anchored = true,
							.Axis = axis,
							.AnchorMin = horizontal ? 0.0f : 1.0f - t,
							.AnchorMax = horizontal ? t : 1.0f,
							.Hides = fill->Hides };
					if (UiWidgetPartRule* handle = findPart(index, slider->Handle))
						*handle = UiWidgetPartRule{ .Anchored = true,
							.Axis = axis,
							.AnchorMin = horizontal ? t : 1.0f - t,
							.AnchorMax = horizontal ? t : 1.0f - t,
							.Hides = handle->Hides };
				}
				else if (const UiToggleComponent* toggle = entity.TryGetComponent<UiToggleComponent>(); toggle != nullptr && !toggle->IsOn)
				{
					if (UiWidgetPartRule* graphic = findPart(index, toggle->Graphic))
						graphic->Hides = true;
				}
				else if (const UiTextInputComponent* textInput = entity.TryGetComponent<UiTextInputComponent>();
					textInput != nullptr && !textInput->Text.empty())
				{
					if (UiWidgetPartRule* placeholder = findPart(index, textInput->Placeholder))
						placeholder->Hides = true;
				}
			}
			return rules;
		}

		// The scroll view rule (C22.2, decision 26): how far, in scene pixels, the element at `index` of `canvas` moves as the
		// Content of the scroll views above it in the canvas, each by its ScrollPosition clamped to the overflow of the
		// element's laid-out size `size` over the scroll view's (canvas units). Zero for an element that no scroll view above
		// it names; a Content that is not below its scroll view is never reached from it.
		static glm::vec2 GetUiScrollOffset(const UiScannedCanvas& canvas, uint32_t index, const UiLayoutTable& table, uint32_t firstNode, glm::vec2 size,
			float scale)
		{
			const UUID entity = canvas.Elements[index].Entity.GetUUID();
			glm::vec2 offset(0.0f);
			for (uint32_t ancestor = canvas.Elements[index].Parent; ancestor != NoUiElement; ancestor = canvas.Elements[ancestor].Parent)
			{
				const UiScrollViewComponent* scrollView = canvas.Elements[ancestor].Entity.TryGetComponent<UiScrollViewComponent>();
				if (scrollView == nullptr || scrollView->Content != entity)
					continue;
				const UiLayoutNode& view = table.Nodes[firstNode + ancestor];
				offset += ClampUiScrollPosition(*scrollView, (view.Max - view.Min) / scale, size, scrollView->ScrollPosition) * scale;
			}
			return offset;
		}

		// The canvas layout of `canvas` laid out over `tableSize` scene pixels (C22.3, decision 49): a Screen canvas covers the
		// scene image; a World canvas covers its own WorldWidth x WorldHeight canvas pixels, whatever the scene resolution, and
		// carries the matrix that places them in the world at its entity's simulated pose (the TransformSystem's world matrix).
		static UiCanvasLayout MakeUiCanvasLayout(const UiScannedCanvas& canvas, const UiCanvasComponent& component, glm::uvec2 tableSize)
		{
			UiCanvasLayout layout{ .Canvas = canvas.Entity.GetUUID(), .SortOrder = canvas.SortOrder, .RenderMode = component.RenderMode };
			glm::uvec2 size = tableSize;
			if (component.RenderMode == UiCanvasRenderMode::World)
			{
				size = GetUiWorldCanvasSize(component.WorldWidth, component.WorldHeight);
				layout.SceneSize = size;
				// The world matrix the last TransformSystem::Update gave the entity, or the chain walk before the first one.
				const WorldTransformComponent* world = canvas.Entity.TryGetComponent<WorldTransformComponent>();
				const glm::mat4 entityWorld = world != nullptr ? world->Matrix : TransformSystem::ComputeWorldMatrix(canvas.Entity);
				layout.World = entityWorld * MakeUiWorldCanvasLocalMatrix(size, component.PixelsPerUnit);
			}
			const glm::vec2 pixels(static_cast<float>(size.x), static_cast<float>(size.y));
			layout.Scale = component.ScaleMode == UiScaleMode::MatchHeight ? pixels.y / component.ReferenceHeight : component.ScaleFactor;
			layout.Size = pixels / layout.Scale;
			return layout;
		}

		// Lays out the elements of `canvas` into `table`, in their order (a parent before its children). A World canvas's
		// elements are placed in its own canvas pixels, over its size instead of the scene image's (C22.3, decision 49).
		static void LayoutUiCanvas(const UiScannedCanvas& canvas, glm::uvec2 tableSize, UiLayoutTable& table)
		{
			const UiCanvasComponent& component = canvas.Entity.GetComponent<UiCanvasComponent>();
			UiCanvasLayout canvasLayout = MakeUiCanvasLayout(canvas, component, tableSize);
			const glm::uvec2 sceneSize = component.RenderMode == UiCanvasRenderMode::World ? canvasLayout.SceneSize : tableSize;
			const glm::vec2 scenePixels(static_cast<float>(sceneSize.x), static_cast<float>(sceneSize.y));
			const float scale = canvasLayout.Scale;
			const auto firstNode = static_cast<uint32_t>(table.Nodes.size());
			const auto count = static_cast<uint32_t>(canvas.Elements.size());
			canvasLayout.FirstNode = firstNode;
			canvasLayout.NodeCount = count;
			table.Canvases.push_back(canvasLayout);

			// The scene image's pixels, which bound every element's visibility.
			const UiClipRect sceneImage{ .MinX = 0,
				.MinY = 0,
				.MaxX = static_cast<int32_t>(std::min(sceneSize.x, static_cast<uint32_t>(UiClipCoordinateLimit))),
				.MaxY = static_cast<int32_t>(std::min(sceneSize.y, static_cast<uint32_t>(UiClipCoordinateLimit))) };
			// A container's placement of each child that takes part in it, set when the container is laid out (before its
			// children), and the clip each element hands its descendants.
			std::vector<std::optional<UiContainerPlacement>> placements(count);
			std::vector<UiInheritedClip> childClips(count);
			std::vector<UiContainerChild> participants;
			std::vector<uint32_t> participantIndices;
			// The slider, toggle and text-input rules (decisions 26 and 48), by element index; empty when no widget names a part
			// here.
			const std::vector<UiWidgetPartRule> partRules = FindUiWidgetPartRules(canvas);

			for (uint32_t index = 0; index < count; ++index)
			{
				const UiScannedElement& element = canvas.Elements[index];
				const UiRectComponent& rect = element.Entity.GetComponent<UiRectComponent>();
				UiLayoutNode node{ .Entity = element.Entity.GetUUID(),
					.Canvas = canvas.Entity.GetUUID(),
					.Parent = element.Parent == NoUiElement ? canvas.Entity.GetUUID() : canvas.Elements[element.Parent].Entity.GetUUID(),
					.Depth = element.Depth };

				glm::vec2 parentMin(0.0f);
				glm::vec2 parentMax = scenePixels;
				UiInheritedClip clip{};
				if (element.Parent != NoUiElement)
				{
					const UiLayoutNode& parentNode = table.Nodes[firstNode + element.Parent];
					parentMin = parentNode.Min;
					parentMax = parentNode.Max;
					clip = childClips[element.Parent];
				}
				if (const std::optional<UiContainerPlacement>& placement = placements[index]; placement.has_value())
				{
					node.Min = parentMin + placement->Position * scale;
					node.Max = node.Min + placement->Size * scale;
				}
				else if (!partRules.empty() && partRules[index].Anchored)
				{
					// A slider's Fill or Handle: its anchors along the slider's direction follow the value, the rest are its own.
					const UiWidgetPartRule& rule = partRules[index];
					UiRectComponent anchored = rect;
					anchored.AnchorMin[rule.Axis] = rule.AnchorMin;
					anchored.AnchorMax[rule.Axis] = rule.AnchorMax;
					PlaceUiRect(anchored, parentMin, parentMax, scale, node.Min, node.Max);
				}
				else
					PlaceUiRect(rect, parentMin, parentMax, scale, node.Min, node.Max);
				if (component.PixelSnap)
				{
					node.Min = SnapToScenePixels(node.Min);
					node.Max = SnapToScenePixels(node.Max);
				}
				// A scroll view's Content moves by its scroll offset, before its children are placed in it (so its subtree moves
				// with it), and snaps again.
				if (const glm::vec2 offset = GetUiScrollOffset(canvas, index, table, firstNode, (node.Max - node.Min) / scale, scale);
					offset != glm::vec2(0.0f))
				{
					node.Min -= offset;
					node.Max -= offset;
					if (component.PixelSnap)
					{
						node.Min = SnapToScenePixels(node.Min);
						node.Max = SnapToScenePixels(node.Max);
					}
				}

				node.Clipped = clip.Clipped;
				node.Clip = clip.Clip;
				const UiClipRect covered = ComputeUiClipRect(node.Min, node.Max);
				UiClipRect visible = IntersectUiClipRects(covered, sceneImage);
				if (clip.Clipped)
					visible = IntersectUiClipRects(visible, clip.Clip);
				// An off toggle's Graphic, a text input's Placeholder while it has text, and their subtrees are laid out but Hidden,
				// so never Visible.
				node.Hidden = !partRules.empty()
					&& (partRules[index].Hides || (element.Parent != NoUiElement && table.Nodes[firstNode + element.Parent].Hidden));
				node.Visible = !visible.IsEmpty() && !node.Hidden;

				// The element's own flag clips its descendants only; a scroll view and a text input clip their descendants whatever
				// the flag says.
				if (rect.ClipChildren || element.Entity.HasComponent<UiScrollViewComponent>() || element.Entity.HasComponent<UiTextInputComponent>())
					childClips[index] = UiInheritedClip{ .Clipped = true, .Clip = clip.Clipped ? IntersectUiClipRects(clip.Clip, covered) : covered };
				else
					childClips[index] = clip;

				// A container places the child elements that take part in it (UiContainerLayout.h), in canvas units from its
				// rectangle's top-left corner.
				if (const UiLayoutComponent* layout = element.Entity.TryGetComponent<UiLayoutComponent>())
				{
					participants.clear();
					participantIndices.clear();
					for (uint32_t child = element.FirstChild; child != NoUiElement; child = canvas.Elements[child].NextSibling)
					{
						const ConstEntity childEntity = canvas.Elements[child].Entity;
						const UiLayoutElementComponent* participation = childEntity.TryGetComponent<UiLayoutElementComponent>();
						if (participation != nullptr && participation->IgnoreLayout)
							continue;
						UiContainerChild entry{ .Size = glm::max(childEntity.GetComponent<UiRectComponent>().Size, glm::vec2(0.0f)) };
						if (participation != nullptr)
						{
							entry.MinSize = participation->MinSize;
							entry.Grow = participation->Grow;
							entry.Shrink = participation->Shrink;
						}
						participants.push_back(entry);
						participantIndices.push_back(child);
					}
					if (!participants.empty())
					{
						const std::vector<UiContainerPlacement> placed = LayoutUiContainer(*layout, (node.Max - node.Min) / scale, participants);
						ENGINE_CORE_ASSERT(placed.size() == participants.size(), "LayoutUiContainer placed {} of {} children", placed.size(),
							participants.size());
						const size_t placedCount = std::min(placed.size(), participants.size());
						for (size_t i = 0; i < placedCount; ++i)
							placements[participantIndices[i]] = placed[i];
					}
				}

				table.Nodes.push_back(node);
			}
		}

	}

	UiLayoutTable ComputeUiLayout(const Scene& scene, glm::uvec2 sceneSize)
	{
		ENGINE_CORE_ASSERT(sceneSize.x >= 1 && sceneSize.y >= 1, "ComputeUiLayout needs a scene resolution of at least 1 x 1, got {} x {}", sceneSize.x,
			sceneSize.y);
		UiLayoutTable table{ .SceneSize = sceneSize };
		// A scene without a canvas (every scene without UI) is not scanned.
		const auto* canvasStorage = scene.GetRegistry().storage<UiCanvasComponent>();
		if (canvasStorage == nullptr || canvasStorage->empty())
			return table;

		const std::vector<UiScannedCanvas> canvases = Utils::ScanUiCanvases(scene);
		// Draw order: ascending SortOrder, ties in canonical order (the scan's).
		std::vector<uint32_t> order(canvases.size());
		for (uint32_t index = 0; index < order.size(); ++index)
			order[index] = index;
		std::ranges::stable_sort(order, [&canvases](uint32_t a, uint32_t b)
		{
			return canvases[a].SortOrder < canvases[b].SortOrder;
		});
		table.Canvases.reserve(canvases.size());
		for (const uint32_t index : order)
			Utils::LayoutUiCanvas(canvases[index], sceneSize, table);
		return table;
	}

	UiClipRect ComputeUiClipRect(glm::vec2 min, glm::vec2 max)
	{
		return UiClipRect{ .MinX = Utils::ToUiClipCoordinate(min.x),
			.MinY = Utils::ToUiClipCoordinate(min.y),
			.MaxX = Utils::ToUiClipCoordinate(max.x),
			.MaxY = Utils::ToUiClipCoordinate(max.y) };
	}

	const UiLayoutNode* FindUiLayoutNode(const UiLayoutTable& table, UUID entity)
	{
		const auto found = std::ranges::find(table.Nodes, entity, &UiLayoutNode::Entity);
		return found == table.Nodes.end() ? nullptr : &*found;
	}

	const UiCanvasLayout* FindUiCanvasLayout(const UiLayoutTable& table, UUID canvas)
	{
		const auto found = std::ranges::find(table.Canvases, canvas, &UiCanvasLayout::Canvas);
		return found == table.Canvases.end() ? nullptr : &*found;
	}

}
