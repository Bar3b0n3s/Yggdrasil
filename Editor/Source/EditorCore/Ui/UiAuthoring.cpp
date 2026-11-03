#include "EditorPCH.h"
#include "EditorCore/Ui/UiAuthoring.h"

#include "Engine/Session/PlaySession.h"

#include <algorithm>
#include <cmath>

// UI authoring (UiAuthoring.h; Docs/Decisions/0033-m22-contract.md decisions 33 and 34): the anchor presets, the rect tool,
// the preview's selection and its frame, as pure functions over UiRect values and layout tables.

namespace Engine {

	namespace Utils {

		// The anchors (AnchorMin, AnchorMax) and the pivot SetPivot gives an axis of a preset: 0 the left or top edge, 0.5 the
		// centre, 1 the right or bottom edge, or stretched from 0 to 1 with the pivot at its centre.
		struct UiAnchorAxis
		{
			float Min = 0.5f;
			float Max = 0.5f;
			float Pivot = 0.5f;
			bool Stretched = false;
		};

		static UiAnchorAxis GetUiAnchorAxis(UiAnchorHorizontal horizontal)
		{
			switch (horizontal)
			{
				case UiAnchorHorizontal::Left:
					return { .Min = 0.0f, .Max = 0.0f, .Pivot = 0.0f };
				case UiAnchorHorizontal::Center:
					return { .Min = 0.5f, .Max = 0.5f, .Pivot = 0.5f };
				case UiAnchorHorizontal::Right:
					return { .Min = 1.0f, .Max = 1.0f, .Pivot = 1.0f };
				case UiAnchorHorizontal::Stretch:
					return { .Min = 0.0f, .Max = 1.0f, .Pivot = 0.5f, .Stretched = true };
			}
			return {};
		}

		static UiAnchorAxis GetUiAnchorAxis(UiAnchorVertical vertical)
		{
			switch (vertical)
			{
				case UiAnchorVertical::Top:
					return { .Min = 0.0f, .Max = 0.0f, .Pivot = 0.0f };
				case UiAnchorVertical::Middle:
					return { .Min = 0.5f, .Max = 0.5f, .Pivot = 0.5f };
				case UiAnchorVertical::Bottom:
					return { .Min = 1.0f, .Max = 1.0f, .Pivot = 1.0f };
				case UiAnchorVertical::Stretch:
					return { .Min = 0.0f, .Max = 1.0f, .Pivot = 0.5f, .Stretched = true };
			}
			return {};
		}

		// One axis of ApplyUiAnchorPreset: the element's Min edge and laid-out size in a parent of `parentSize` (UiRectComponent.h's
		// rule, the parent's Min at 0), then the axis's Position and Size under the new anchors and pivot.
		struct UiAxisPlacement
		{
			float Pivot = 0.5f;
			float Position = 0.0f;
			float Size = 0.0f;
		};

		static UiAxisPlacement PlaceUiAxis(float anchorMin, float anchorMax, float pivot, float position, float size, float parentSize,
			const UiAnchorAxis& preset, UiAnchorPresetOptions options)
		{
			const float span = (anchorMax - anchorMin) * parentSize;
			const float laidOut = std::max(0.0f, span + size);
			const float pivotPoint = anchorMin * parentSize + pivot * span + position;
			const float edge = pivotPoint - pivot * laidOut;

			UiAxisPlacement placement{ .Pivot = options.SetPivot ? preset.Pivot : pivot };
			const float newSpan = (preset.Max - preset.Min) * parentSize;
			if (options.SetPosition)
			{
				placement.Position = 0.0f;
				placement.Size = preset.Stretched ? 0.0f : laidOut;
				return placement;
			}
			// The rectangle stays where it was: the same Min edge and laid-out size under the new anchors and pivot.
			placement.Size = laidOut - newSpan;
			placement.Position = edge + placement.Pivot * laidOut - preset.Min * parentSize - placement.Pivot * newSpan;
			return placement;
		}

		// The handle of one axis under `point`: -1 the min edge, 1 the max edge, 0 neither (within `grab` of neither).
		static int FindUiRectEdge(float min, float max, float point, float grab)
		{
			const float toMin = std::abs(point - min);
			const float toMax = std::abs(point - max);
			const bool nearMin = toMin <= grab;
			const bool nearMax = toMax <= grab;
			if (nearMin && nearMax)
				return toMin < toMax ? -1 : 1;
			return nearMin ? -1 : nearMax ? 1
										  : 0;
		}

		// The drag of one axis's edge (-1 min, 1 max, 0 none) by `delta`, clamped so that the laid-out size `startSize` never
		// goes below 0 (DragUiRect).
		static void DragUiRectEdge(int edge, float startSize, float pivot, float delta, float& position, float& size)
		{
			if (edge > 0)
			{
				const float moved = std::max(delta, -startSize);
				size += moved;
				position += pivot * moved;
			}
			else if (edge < 0)
			{
				const float moved = std::min(delta, startSize);
				size -= moved;
				position += (1.0f - pivot) * moved;
			}
		}

	}

	UiRectComponent ApplyUiAnchorPreset(const UiRectComponent& rect, UiAnchorPreset preset, glm::vec2 parentSize, UiAnchorPresetOptions options)
	{
		const Utils::UiAnchorAxis horizontal = Utils::GetUiAnchorAxis(preset.Horizontal);
		const Utils::UiAnchorAxis vertical = Utils::GetUiAnchorAxis(preset.Vertical);
		const Utils::UiAxisPlacement x = Utils::PlaceUiAxis(rect.AnchorMin.x, rect.AnchorMax.x, rect.Pivot.x, rect.Position.x, rect.Size.x,
			parentSize.x, horizontal, options);
		const Utils::UiAxisPlacement y = Utils::PlaceUiAxis(rect.AnchorMin.y, rect.AnchorMax.y, rect.Pivot.y, rect.Position.y, rect.Size.y,
			parentSize.y, vertical, options);
		UiRectComponent result = rect;
		result.AnchorMin = glm::vec2(horizontal.Min, vertical.Min);
		result.AnchorMax = glm::vec2(horizontal.Max, vertical.Max);
		result.Pivot = glm::vec2(x.Pivot, y.Pivot);
		result.Position = glm::vec2(x.Position, y.Position);
		result.Size = glm::vec2(x.Size, y.Size);
		return result;
	}

	std::optional<UiAnchorPreset> FindUiAnchorPreset(const UiRectComponent& rect)
	{
		std::optional<UiAnchorHorizontal> horizontal;
		for (const UiAnchorHorizontal candidate :
			{ UiAnchorHorizontal::Left, UiAnchorHorizontal::Center, UiAnchorHorizontal::Right, UiAnchorHorizontal::Stretch })
		{
			const Utils::UiAnchorAxis axis = Utils::GetUiAnchorAxis(candidate);
			if (rect.AnchorMin.x == axis.Min && rect.AnchorMax.x == axis.Max)
				horizontal = candidate;
		}
		std::optional<UiAnchorVertical> vertical;
		for (const UiAnchorVertical candidate :
			{ UiAnchorVertical::Top, UiAnchorVertical::Middle, UiAnchorVertical::Bottom, UiAnchorVertical::Stretch })
		{
			const Utils::UiAnchorAxis axis = Utils::GetUiAnchorAxis(candidate);
			if (rect.AnchorMin.y == axis.Min && rect.AnchorMax.y == axis.Max)
				vertical = candidate;
		}
		if (!horizontal || !vertical)
			return std::nullopt;
		return UiAnchorPreset{ *horizontal, *vertical };
	}

	std::optional<UiRectHandle> FindUiRectHandle(glm::vec2 min, glm::vec2 max, glm::vec2 point, float grab)
	{
		const int x = Utils::FindUiRectEdge(min.x, max.x, point.x, grab);
		const int y = Utils::FindUiRectEdge(min.y, max.y, point.y, grab);
		const bool betweenX = point.x >= min.x - grab && point.x <= max.x + grab;
		const bool betweenY = point.y >= min.y - grab && point.y <= max.y + grab;
		if (x != 0 && y != 0)
		{
			if (y < 0)
				return x < 0 ? UiRectHandle::TopLeft : UiRectHandle::TopRight;
			return x < 0 ? UiRectHandle::BottomLeft : UiRectHandle::BottomRight;
		}
		if (y != 0 && betweenX)
			return y < 0 ? UiRectHandle::Top : UiRectHandle::Bottom;
		if (x != 0 && betweenY)
			return x < 0 ? UiRectHandle::Left : UiRectHandle::Right;
		if (point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y)
			return UiRectHandle::Move;
		return std::nullopt;
	}

	UiRectComponent DragUiRect(const UiRectComponent& start, glm::vec2 startSize, UiRectHandle handle, glm::vec2 delta)
	{
		UiRectComponent result = start;
		int x = 0;
		int y = 0;
		switch (handle)
		{
			case UiRectHandle::Move:
				result.Position += delta;
				return result;
			case UiRectHandle::Left:
				x = -1;
				break;
			case UiRectHandle::Right:
				x = 1;
				break;
			case UiRectHandle::Top:
				y = -1;
				break;
			case UiRectHandle::Bottom:
				y = 1;
				break;
			case UiRectHandle::TopLeft:
				x = -1;
				y = -1;
				break;
			case UiRectHandle::TopRight:
				x = 1;
				y = -1;
				break;
			case UiRectHandle::BottomLeft:
				x = -1;
				y = 1;
				break;
			case UiRectHandle::BottomRight:
				x = 1;
				y = 1;
				break;
		}
		Utils::DragUiRectEdge(x, startSize.x, start.Pivot.x, delta.x, result.Position.x, result.Size.x);
		Utils::DragUiRectEdge(y, startSize.y, start.Pivot.y, delta.y, result.Position.y, result.Size.y);
		return result;
	}

	UUID PickUiElement(const UiLayoutTable& table, glm::vec2 point)
	{
		for (auto node = table.Nodes.rbegin(); node != table.Nodes.rend(); ++node)
		{
			if (node->Hidden)
				continue;
			if (point.x < node->Min.x || point.x >= node->Max.x || point.y < node->Min.y || point.y >= node->Max.y)
				continue;
			if (node->Clipped
				&& (point.x < static_cast<float>(node->Clip.MinX) || point.x >= static_cast<float>(node->Clip.MaxX)
					|| point.y < static_cast<float>(node->Clip.MinY) || point.y >= static_cast<float>(node->Clip.MaxY)))
				continue;
			return node->Entity;
		}
		return {};
	}

	glm::uvec2 GetUiPreviewFrameSize(glm::uvec2 gameResolution, const WindowSettings& window)
	{
		if (gameResolution.x != 0 && gameResolution.y != 0)
			return gameResolution;
		return GetDeterministicViewSize(window);
	}

	UiPreviewFrame ComputeUiPreviewFrame(glm::uvec2 frameSize, glm::vec2 panelMin, glm::vec2 panelSize)
	{
		const glm::vec2 frame(static_cast<float>(std::max(frameSize.x, 1u)), static_cast<float>(std::max(frameSize.y, 1u)));
		if (!std::isfinite(panelSize.x) || !std::isfinite(panelSize.y) || panelSize.x <= 0.0f || panelSize.y <= 0.0f)
			return UiPreviewFrame{ .Min = panelMin, .Size = glm::vec2(0.0f), .Scale = 0.0f };
		const float scale = std::min(panelSize.x / frame.x, panelSize.y / frame.y);
		const glm::vec2 size = frame * scale;
		return UiPreviewFrame{ .Min = panelMin + (panelSize - size) * 0.5f, .Size = size, .Scale = scale };
	}

	glm::vec2 MapUiPreviewPointToScene(const UiPreviewFrame& frame, glm::vec2 panelPoint)
	{
		if (frame.Scale == 0.0f)
			return panelPoint;
		return (panelPoint - frame.Min) / frame.Scale;
	}

}
