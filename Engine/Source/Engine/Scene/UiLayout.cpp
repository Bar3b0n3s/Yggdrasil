#include "EnginePCH.h"
#include "Engine/Scene/UiLayout.h"

#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiLayoutElementComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiContainerLayout.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
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

		// Lays out the elements of `canvas` into `table`, in their order (a parent before its children).
		static void LayoutUiCanvas(const UiScannedCanvas& canvas, glm::uvec2 sceneSize, UiLayoutTable& table)
		{
			const UiCanvasComponent& component = canvas.Entity.GetComponent<UiCanvasComponent>();
			const glm::vec2 scenePixels(static_cast<float>(sceneSize.x), static_cast<float>(sceneSize.y));
			const float scale = component.ScaleMode == UiScaleMode::MatchHeight ? scenePixels.y / component.ReferenceHeight : component.ScaleFactor;
			const auto firstNode = static_cast<uint32_t>(table.Nodes.size());
			const auto count = static_cast<uint32_t>(canvas.Elements.size());
			table.Canvases.push_back(UiCanvasLayout{ .Canvas = canvas.Entity.GetUUID(),
				.SortOrder = canvas.SortOrder,
				.Scale = scale,
				.Size = scenePixels / scale,
				.FirstNode = firstNode,
				.NodeCount = count });

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
				else
					PlaceUiRect(rect, parentMin, parentMax, scale, node.Min, node.Max);
				if (component.PixelSnap)
				{
					node.Min = SnapToScenePixels(node.Min);
					node.Max = SnapToScenePixels(node.Max);
				}

				node.Clipped = clip.Clipped;
				node.Clip = clip.Clip;
				const UiClipRect covered = ComputeUiClipRect(node.Min, node.Max);
				UiClipRect visible = IntersectUiClipRects(covered, sceneImage);
				if (clip.Clipped)
					visible = IntersectUiClipRects(visible, clip.Clip);
				node.Visible = !visible.IsEmpty();

				// The element's own flag clips its descendants only.
				if (rect.ClipChildren)
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
