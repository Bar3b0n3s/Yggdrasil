#pragma once

#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Core/Json/Json.h"

#include <glm/vec2.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The graph canvas's geometry (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): the view's pan and zoom,
// each node's box and pins in graph units (the .graph file's Position units), the links' curves and the hit tests the canvas's
// input uses. UI-free and pure, so the mapping, the layout and the picking are tested headless; the canvas
// (Editor/Panels/GraphEditorPanel) measures its text with its font and draws. Graph units are canvas pixels at zoom 1, and a
// canvas point is in pixels from the canvas's top-left corner.

namespace Engine {

	// The canvas's view: which graph point is at the canvas's top-left corner, and the zoom.
	struct GraphCanvasView
	{
		glm::vec2 Origin = glm::vec2(0.0f); // graph units
		float Zoom = 1.0f;                  // canvas pixels per graph unit, within [MinGraphCanvasZoom, MaxGraphCanvasZoom]
	};

	inline constexpr float MinGraphCanvasZoom = 0.2f;
	inline constexpr float MaxGraphCanvasZoom = 2.0f;

	// The canvas point of graph point `point`, and back. Pure.
	[[nodiscard]] glm::vec2 GraphToCanvas(const GraphCanvasView& view, const glm::vec2& point);
	[[nodiscard]] glm::vec2 CanvasToGraph(const GraphCanvasView& view, const glm::vec2& point);

	// `view` zoomed to `zoom` (clamped to the zoom range; a non-finite zoom keeps the view's) about canvas point `anchor`: the
	// graph point under `anchor` stays under it. Pure.
	[[nodiscard]] GraphCanvasView ZoomGraphCanvas(const GraphCanvasView& view, const glm::vec2& anchor, float zoom);

	// The view that shows the graph rectangle [minimum, maximum] centred in a canvas of `canvasSize` pixels, `margin` pixels
	// from its edges where it fits: the largest zoom up to 1 that fits it, never below MinGraphCanvasZoom. A canvas smaller
	// than twice the margin uses no margin. Pure.
	[[nodiscard]] GraphCanvasView FrameGraphCanvas(const glm::vec2& minimum, const glm::vec2& maximum, const glm::vec2& canvasSize, float margin);

	// The sizes a node's box is laid out with, in graph units: the panel's font metrics at zoom 1.
	struct GraphNodeMetrics
	{
		float TitleHeight = 24.0f; // the title bar
		float RowHeight = 20.0f;   // one row of pins
		float PinRadius = 5.0f;
		float Padding = 8.0f;    // between the box's edge and its pins' centres, and around the title
		float LabelGap = 6.0f;   // between a pin and its label
		float ColumnGap = 16.0f; // between the input labels and the output labels
		float MinWidth = 120.0f;
	};

	// The most bytes of an input literal a pin's label shows after the pin's name (FormatGraphLiteral).
	inline constexpr size_t MaxGraphLiteralLabelBytes = 24;

	// The literal `value` as a node shows it after its input pin's name ("Speed = 2"): a string quoted, any other value as its
	// JSON text; one longer than MaxGraphLiteralLabelBytes bytes is cut to fit with "..." at its end, never inside a UTF-8
	// sequence. Pure.
	[[nodiscard]] std::string FormatGraphLiteral(const Json& value);

	// One pin as the canvas shows it.
	struct GraphPinShape
	{
		std::string Name{};
		GraphPinDirection Direction = GraphPinDirection::Input;
		GraphPinType Type = GraphPinType::Any;
		std::string Label{}; // the text beside the pin: its name, and an unlinked input's literal ("Speed = 2")
	};

	// One node as the canvas shows it: its title and its pins in the description's order (the exec input, the data inputs,
	// the exec outputs, the data outputs).
	struct GraphNodeShape
	{
		uint32_t Node = 0;
		glm::vec2 Position = glm::vec2(0.0f); // the box's top-left corner (GraphNode::Position)
		std::string Title{};
		std::vector<GraphPinShape> Pins{};
	};

	// A laid-out pin: its centre in graph units.
	struct GraphPinLayout
	{
		std::string Name{};
		GraphPinDirection Direction = GraphPinDirection::Input;
		GraphPinType Type = GraphPinType::Any;
		glm::vec2 Center = glm::vec2(0.0f);
	};

	// A laid-out node: its box and its pins, in graph units.
	struct GraphNodeLayout
	{
		uint32_t Node = 0;
		glm::vec2 Minimum = glm::vec2(0.0f);
		glm::vec2 Maximum = glm::vec2(0.0f);
		std::vector<GraphPinLayout> Pins{};
	};

	// Lays out `shape`: the title bar on top, then one row per pin pair, the inputs on the left edge and the outputs on the
	// right edge, each column in the shape's order. The box is wide enough for the title, and for the widest input label
	// beside the widest output label, as `measure` gives their widths in graph units, and never narrower than
	// metrics.MinWidth. Pure when `measure` is.
	[[nodiscard]] GraphNodeLayout LayOutGraphNode(const GraphNodeShape& shape, const GraphNodeMetrics& metrics, const std::function<float(std::string_view)>& measure);

	// A pin under a point.
	struct GraphPinHit
	{
		uint32_t Node = 0;
		std::string Pin{};
		GraphPinDirection Direction = GraphPinDirection::Input;

		bool operator==(const GraphPinHit&) const = default;
	};

	// The pin of `layouts` whose centre is within `radius` graph units of `point`, the nearest, searching the nodes from the
	// last (drawn on top) to the first; nullopt when none is. Pure.
	[[nodiscard]] std::optional<GraphPinHit> FindGraphPinAt(std::span<const GraphNodeLayout> layouts, const glm::vec2& point, float radius);

	// The node of `layouts` whose box holds `point`, the last one (drawn on top) first; 0 when none does. Pure.
	[[nodiscard]] uint32_t FindGraphNodeAt(std::span<const GraphNodeLayout> layouts, const glm::vec2& point);

	// The nodes of `layouts` whose boxes overlap the rectangle between the corners `first` and `second` (in either order),
	// sorted by id. Pure.
	[[nodiscard]] std::vector<uint32_t> FindGraphNodesInRect(std::span<const GraphNodeLayout> layouts, const glm::vec2& first, const glm::vec2& second);

	// The laid-out pin `pin` of `layouts`, or nullptr.
	[[nodiscard]] const GraphPinLayout* FindGraphPinLayout(std::span<const GraphNodeLayout> layouts, const GraphPinRef& pin);

	// The cubic Bezier curve (its four control points) the canvas draws a link with, from an output pin's centre to an input
	// pin's centre: it leaves the output to the right and enters the input from the left. Pure.
	[[nodiscard]] std::array<glm::vec2, 4> GetGraphLinkCurve(const glm::vec2& from, const glm::vec2& to);

	// The distance from `point` to `curve`, measured along 32 straight segments. Pure.
	[[nodiscard]] float GetGraphLinkDistance(const std::array<glm::vec2, 4>& curve, const glm::vec2& point);

	// The link of `links` whose curve passes within `tolerance` graph units of `point`, the nearest; nullopt when none does or
	// a link's pins are not laid out. Pure.
	[[nodiscard]] std::optional<GraphLink> FindGraphLinkAt(std::span<const GraphLink> links, std::span<const GraphNodeLayout> layouts, const glm::vec2& point, float tolerance);

}
