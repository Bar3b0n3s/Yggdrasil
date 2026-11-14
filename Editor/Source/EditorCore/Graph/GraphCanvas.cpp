#include "EditorPCH.h"
#include "EditorCore/Graph/GraphCanvas.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Utf8.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

// The graph canvas's geometry (Docs/Decisions/0035-m24-contract.md decision 15, W24.2's stream A).

namespace Engine {

	namespace Utils {

		// The point of `curve` at parameter `t` in [0, 1].
		static glm::vec2 EvaluateGraphLinkCurve(const std::array<glm::vec2, 4>& curve, float t)
		{
			const float u = 1.0f - t;
			return u * u * u * curve[0] + 3.0f * u * u * t * curve[1] + 3.0f * u * t * t * curve[2] + t * t * t * curve[3];
		}

		// The distance from `point` to the segment [a, b].
		static float SegmentDistance(const glm::vec2& a, const glm::vec2& b, const glm::vec2& point)
		{
			const glm::vec2 along = b - a;
			const float length = glm::dot(along, along);
			const float t = length > 0.0f ? std::clamp(glm::dot(point - a, along) / length, 0.0f, 1.0f) : 0.0f;
			return glm::length(point - (a + along * t));
		}

	}

	std::string FormatGraphLiteral(const Json& value)
	{
		const std::string text = value.is_string() ? std::format("\"{}\"", JsonReader(value).ReadString().value_or(std::string())) : value.dump();
		if (text.size() <= MaxGraphLiteralLabelBytes)
			return text;
		constexpr std::string_view Ellipsis = "...";
		const std::string_view head = std::string_view(text).substr(0, MaxGraphLiteralLabelBytes - Ellipsis.size());
		return std::format("{}{}", head.substr(0, CompleteUtf8Prefix(head)), Ellipsis);
	}

	glm::vec2 GraphToCanvas(const GraphCanvasView& view, const glm::vec2& point)
	{
		return (point - view.Origin) * view.Zoom;
	}

	glm::vec2 CanvasToGraph(const GraphCanvasView& view, const glm::vec2& point)
	{
		return point / view.Zoom + view.Origin;
	}

	GraphCanvasView ZoomGraphCanvas(const GraphCanvasView& view, const glm::vec2& anchor, float zoom)
	{
		if (!std::isfinite(zoom))
			return view;
		const float clamped = std::clamp(zoom, MinGraphCanvasZoom, MaxGraphCanvasZoom);
		const glm::vec2 anchored = CanvasToGraph(view, anchor);
		return GraphCanvasView{ .Origin = anchored - anchor / clamped, .Zoom = clamped };
	}

	GraphCanvasView FrameGraphCanvas(const glm::vec2& minimum, const glm::vec2& maximum, const glm::vec2& canvasSize, float margin)
	{
		const glm::vec2 low = glm::min(minimum, maximum);
		const glm::vec2 high = glm::max(minimum, maximum);
		const glm::vec2 size = high - low;
		const float edge = canvasSize.x > 2.0f * margin && canvasSize.y > 2.0f * margin ? margin : 0.0f;
		const glm::vec2 room = glm::max(canvasSize - glm::vec2(2.0f * edge), glm::vec2(1.0f));
		float zoom = 1.0f;
		if (size.x > 0.0f)
			zoom = std::min(zoom, room.x / size.x);
		if (size.y > 0.0f)
			zoom = std::min(zoom, room.y / size.y);
		zoom = std::clamp(zoom, MinGraphCanvasZoom, MaxGraphCanvasZoom);
		const glm::vec2 center = (low + high) * 0.5f;
		return GraphCanvasView{ .Origin = center - canvasSize * 0.5f / zoom, .Zoom = zoom };
	}

	GraphNodeLayout LayOutGraphNode(const GraphNodeShape& shape, const GraphNodeMetrics& metrics, const std::function<float(std::string_view)>& measure)
	{
		float inputLabels = 0.0f;
		float outputLabels = 0.0f;
		size_t inputs = 0;
		size_t outputs = 0;
		for (const GraphPinShape& pin : shape.Pins)
		{
			const float width = measure(pin.Label);
			if (pin.Direction == GraphPinDirection::Input)
			{
				inputLabels = std::max(inputLabels, width);
				++inputs;
			}
			else
			{
				outputLabels = std::max(outputLabels, width);
				++outputs;
			}
		}
		// Each column holds a pin, the gap to its label and the label; the columns sit side by side with a gap between them.
		const float column = metrics.Padding + metrics.PinRadius + metrics.LabelGap;
		const float pinsWidth = (inputs > 0 ? column + inputLabels : metrics.Padding) + metrics.ColumnGap + (outputs > 0 ? outputLabels + column : metrics.Padding);
		const float titleWidth = measure(shape.Title) + 2.0f * metrics.Padding;
		const float width = std::max({ metrics.MinWidth, pinsWidth, titleWidth });
		const float rows = static_cast<float>(std::max(inputs, outputs));
		const float height = metrics.TitleHeight + rows * metrics.RowHeight + (rows > 0.0f ? metrics.Padding * 0.5f : 0.0f);

		GraphNodeLayout layout{ .Node = shape.Node, .Minimum = shape.Position, .Maximum = shape.Position + glm::vec2(width, height), .Pins = {} };
		size_t inputRow = 0;
		size_t outputRow = 0;
		for (const GraphPinShape& pin : shape.Pins)
		{
			const bool input = pin.Direction == GraphPinDirection::Input;
			const size_t row = input ? inputRow++ : outputRow++;
			const float y = shape.Position.y + metrics.TitleHeight + (static_cast<float>(row) + 0.5f) * metrics.RowHeight;
			const float x = input ? shape.Position.x + metrics.Padding : shape.Position.x + width - metrics.Padding;
			layout.Pins.push_back({ .Name = pin.Name, .Direction = pin.Direction, .Type = pin.Type, .Center = glm::vec2(x, y) });
		}
		return layout;
	}

	std::optional<GraphPinHit> FindGraphPinAt(std::span<const GraphNodeLayout> layouts, const glm::vec2& point, float radius)
	{
		std::optional<GraphPinHit> best;
		float bestDistance = std::numeric_limits<float>::max();
		for (auto node = layouts.rbegin(); node != layouts.rend(); ++node)
		{
			for (const GraphPinLayout& pin : node->Pins)
			{
				const float distance = glm::length(pin.Center - point);
				if (distance <= radius && distance < bestDistance)
				{
					best = GraphPinHit{ .Node = node->Node, .Pin = pin.Name, .Direction = pin.Direction };
					bestDistance = distance;
				}
			}
			// A pin of a node on top hides the pins of the nodes below it.
			if (best.has_value())
				return best;
		}
		return best;
	}

	uint32_t FindGraphNodeAt(std::span<const GraphNodeLayout> layouts, const glm::vec2& point)
	{
		for (auto node = layouts.rbegin(); node != layouts.rend(); ++node)
		{
			if (point.x >= node->Minimum.x && point.y >= node->Minimum.y && point.x <= node->Maximum.x && point.y <= node->Maximum.y)
				return node->Node;
		}
		return 0;
	}

	std::vector<uint32_t> FindGraphNodesInRect(std::span<const GraphNodeLayout> layouts, const glm::vec2& first, const glm::vec2& second)
	{
		const glm::vec2 low = glm::min(first, second);
		const glm::vec2 high = glm::max(first, second);
		std::vector<uint32_t> nodes;
		for (const GraphNodeLayout& node : layouts)
		{
			if (node.Minimum.x <= high.x && node.Maximum.x >= low.x && node.Minimum.y <= high.y && node.Maximum.y >= low.y)
				nodes.push_back(node.Node);
		}
		std::ranges::sort(nodes);
		return nodes;
	}

	const GraphPinLayout* FindGraphPinLayout(std::span<const GraphNodeLayout> layouts, const GraphPinRef& pin)
	{
		for (const GraphNodeLayout& node : layouts)
		{
			if (node.Node != pin.Node)
				continue;
			const auto found = std::ranges::find(node.Pins, pin.Pin, &GraphPinLayout::Name);
			return found != node.Pins.end() ? &*found : nullptr;
		}
		return nullptr;
	}

	std::array<glm::vec2, 4> GetGraphLinkCurve(const glm::vec2& from, const glm::vec2& to)
	{
		// The tangents grow with the horizontal distance, so a link that runs backwards still loops out of its pins.
		const float reach = std::max(std::abs(to.x - from.x) * 0.5f, 40.0f);
		return { from, from + glm::vec2(reach, 0.0f), to - glm::vec2(reach, 0.0f), to };
	}

	float GetGraphLinkDistance(const std::array<glm::vec2, 4>& curve, const glm::vec2& point)
	{
		constexpr int Segments = 32;
		float distance = std::numeric_limits<float>::max();
		glm::vec2 previous = curve[0];
		for (int segment = 1; segment <= Segments; ++segment)
		{
			const glm::vec2 next = Utils::EvaluateGraphLinkCurve(curve, static_cast<float>(segment) / static_cast<float>(Segments));
			distance = std::min(distance, Utils::SegmentDistance(previous, next, point));
			previous = next;
		}
		return distance;
	}

	std::optional<GraphLink> FindGraphLinkAt(std::span<const GraphLink> links, std::span<const GraphNodeLayout> layouts, const glm::vec2& point, float tolerance)
	{
		std::optional<GraphLink> best;
		float bestDistance = std::numeric_limits<float>::max();
		for (const GraphLink& link : links)
		{
			const GraphPinLayout* from = FindGraphPinLayout(layouts, link.From);
			const GraphPinLayout* to = FindGraphPinLayout(layouts, link.To);
			if (from == nullptr || to == nullptr)
				continue;
			const float distance = GetGraphLinkDistance(GetGraphLinkCurve(from->Center, to->Center), point);
			if (distance <= tolerance && distance < bestDistance)
			{
				best = link;
				bestDistance = distance;
			}
		}
		return best;
	}

}
