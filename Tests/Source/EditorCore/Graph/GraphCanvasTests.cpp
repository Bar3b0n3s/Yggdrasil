#include "TestsPCH.h"

#include "EditorCore/Graph/GraphCanvas.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// The graph canvas's geometry (M24, W24.2's stream A; Docs/Decisions/0035-m24-contract.md decision 15): the view's mapping, its
// zoom and framing, a node's layout and the hit tests the canvas's input uses.

namespace Engine {

	namespace {

		// Seven graph units a character, as a fixed-width font would measure.
		float MeasureCharacters(std::string_view text)
		{
			return 7.0f * static_cast<float>(text.size());
		}

		GraphNodeShape MakeShape(uint32_t node, glm::vec2 position, std::vector<GraphPinShape> pins, std::string title = "Node")
		{
			return GraphNodeShape{ .Node = node, .Position = position, .Title = std::move(title), .Pins = std::move(pins) };
		}

		GraphPinShape Pin(std::string name, GraphPinDirection direction, GraphPinType type = GraphPinType::Number)
		{
			std::string label = name;
			return GraphPinShape{ .Name = std::move(name), .Direction = direction, .Type = type, .Label = std::move(label) };
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("GraphCanvas: a graph point maps to the canvas and back, and zooming keeps the point under its anchor")
		{
			const GraphCanvasView view{ .Origin = glm::vec2(-100.0f, 50.0f), .Zoom = 0.5f };
			CHECK(GraphToCanvas(view, glm::vec2(-100.0f, 50.0f)) == glm::vec2(0.0f));
			CHECK(GraphToCanvas(view, glm::vec2(100.0f, 250.0f)) == glm::vec2(100.0f, 100.0f));
			CHECK(CanvasToGraph(view, glm::vec2(100.0f, 100.0f)) == glm::vec2(100.0f, 250.0f));

			const glm::vec2 anchor(300.0f, 120.0f);
			const glm::vec2 under = CanvasToGraph(view, anchor);
			const GraphCanvasView zoomed = ZoomGraphCanvas(view, anchor, 1.5f);
			CHECK(zoomed.Zoom == 1.5f);
			const glm::vec2 still = CanvasToGraph(zoomed, anchor);
			CHECK(still.x == doctest::Approx(under.x));
			CHECK(still.y == doctest::Approx(under.y));

			// The zoom stays in its range, and a zoom that is not a number leaves the view alone.
			CHECK(ZoomGraphCanvas(view, anchor, 100.0f).Zoom == MaxGraphCanvasZoom);
			CHECK(ZoomGraphCanvas(view, anchor, 0.001f).Zoom == MinGraphCanvasZoom);
			const GraphCanvasView kept = ZoomGraphCanvas(view, anchor, std::numeric_limits<float>::quiet_NaN());
			CHECK(kept.Zoom == view.Zoom);
			CHECK(kept.Origin == view.Origin);
		}

		TEST_CASE("GraphCanvas: framing centres a rectangle in the canvas at the largest zoom up to 1 that fits it")
		{
			const glm::vec2 canvas(800.0f, 600.0f);
			// A small rectangle shows at zoom 1, centred.
			const GraphCanvasView small = FrameGraphCanvas(glm::vec2(0.0f), glm::vec2(200.0f, 100.0f), canvas, 40.0f);
			CHECK(small.Zoom == 1.0f);
			const glm::vec2 center = GraphToCanvas(small, glm::vec2(100.0f, 50.0f));
			CHECK(center.x == doctest::Approx(400.0f));
			CHECK(center.y == doctest::Approx(300.0f));

			// A wide one shrinks to fit between the margins, the corners given in either order.
			const GraphCanvasView wide = FrameGraphCanvas(glm::vec2(1440.0f, 100.0f), glm::vec2(0.0f), canvas, 40.0f);
			CHECK(wide.Zoom == doctest::Approx(720.0f / 1440.0f));
			CHECK(GraphToCanvas(wide, glm::vec2(0.0f, 50.0f)).x == doctest::Approx(40.0f));
			CHECK(GraphToCanvas(wide, glm::vec2(1440.0f, 50.0f)).x == doctest::Approx(760.0f));

			// A huge one stops at the smallest zoom; a canvas narrower than the margins uses none.
			CHECK(FrameGraphCanvas(glm::vec2(0.0f), glm::vec2(1.0e6f), canvas, 40.0f).Zoom == MinGraphCanvasZoom);
			const GraphCanvasView cramped = FrameGraphCanvas(glm::vec2(0.0f), glm::vec2(60.0f), glm::vec2(60.0f), 40.0f);
			CHECK(cramped.Zoom == 1.0f);
			CHECK(GraphToCanvas(cramped, glm::vec2(0.0f)) == glm::vec2(0.0f));
		}

		TEST_CASE("GraphCanvas: a node is laid out with its inputs on the left, its outputs on the right and room for every label")
		{
			const GraphNodeMetrics metrics{ .TitleHeight = 24.0f, .RowHeight = 20.0f, .PinRadius = 5.0f, .Padding = 8.0f, .LabelGap = 6.0f, .ColumnGap = 16.0f, .MinWidth = 120.0f };
			const GraphNodeShape shape = MakeShape(7, glm::vec2(100.0f, 40.0f),
				{ Pin("In", GraphPinDirection::Input, GraphPinType::Exec), Pin("A", GraphPinDirection::Input), Pin("B", GraphPinDirection::Input),
					Pin("Then", GraphPinDirection::Output, GraphPinType::Exec), Pin("Result", GraphPinDirection::Output) });
			const GraphNodeLayout layout = LayOutGraphNode(shape, metrics, MeasureCharacters);
			CHECK(layout.Node == 7);
			CHECK(layout.Minimum == glm::vec2(100.0f, 40.0f));
			const float width = layout.Maximum.x - layout.Minimum.x;
			CHECK(width == metrics.MinWidth);
			// The title, then three rows (the most pins of one side), then half a padding.
			CHECK(layout.Maximum.y - layout.Minimum.y == doctest::Approx(24.0f + 3.0f * 20.0f + 4.0f));
			REQUIRE(layout.Pins.size() == 5);
			CHECK(layout.Pins[0].Name == "In");
			CHECK(layout.Pins[0].Center == glm::vec2(108.0f, 40.0f + 24.0f + 10.0f));
			CHECK(layout.Pins[2].Center == glm::vec2(108.0f, 40.0f + 24.0f + 50.0f));
			CHECK(layout.Pins[3].Name == "Then");
			CHECK(layout.Pins[3].Center == glm::vec2(100.0f + width - 8.0f, 40.0f + 24.0f + 10.0f));
			CHECK(layout.Pins[4].Direction == GraphPinDirection::Output);
			CHECK(layout.Pins[4].Type == GraphPinType::Number);
			CHECK(layout.Pins[4].Center.y == 40.0f + 24.0f + 30.0f);

			// Long labels and a long title widen the box to hold them.
			GraphNodeShape labelled = shape;
			labelled.Pins[1].Label = std::string(30, 'a');
			labelled.Pins[4].Label = std::string(20, 'b');
			const GraphNodeLayout wider = LayOutGraphNode(labelled, metrics, MeasureCharacters);
			CHECK(wider.Maximum.x - wider.Minimum.x == doctest::Approx((8.0f + 5.0f + 6.0f) * 2.0f + 16.0f + 7.0f * 50.0f));
			const GraphNodeLayout titled = LayOutGraphNode(MakeShape(1, glm::vec2(0.0f), {}, std::string(40, 't')), metrics, MeasureCharacters);
			CHECK(titled.Maximum.x == doctest::Approx(7.0f * 40.0f + 16.0f));
			CHECK(titled.Maximum.y == doctest::Approx(24.0f));
		}

		TEST_CASE("GraphCanvas: hit tests find the topmost pin and node, the nodes a box overlaps and the nearest link")
		{
			const GraphNodeMetrics metrics{};
			// Node 2 overlaps node 1 and is drawn after it, on top.
			const std::vector<GraphNodeLayout> layouts = {
				LayOutGraphNode(MakeShape(1, glm::vec2(0.0f), { Pin("Value", GraphPinDirection::Output) }), metrics, MeasureCharacters),
				LayOutGraphNode(MakeShape(2, glm::vec2(60.0f, 0.0f), { Pin("A", GraphPinDirection::Input) }), metrics, MeasureCharacters),
				LayOutGraphNode(MakeShape(3, glm::vec2(400.0f, 300.0f), { Pin("B", GraphPinDirection::Input) }), metrics, MeasureCharacters),
			};
			CHECK(FindGraphNodeAt(layouts, glm::vec2(80.0f, 10.0f)) == 2);
			CHECK(FindGraphNodeAt(layouts, glm::vec2(10.0f, 10.0f)) == 1);
			CHECK(FindGraphNodeAt(layouts, glm::vec2(300.0f, 10.0f)) == 0);

			const GraphPinLayout* value = FindGraphPinLayout(layouts, GraphPinRef{ .Node = 1, .Pin = "Value" });
			const GraphPinLayout* b = FindGraphPinLayout(layouts, GraphPinRef{ .Node = 3, .Pin = "B" });
			REQUIRE(value != nullptr);
			REQUIRE(b != nullptr);
			CHECK(FindGraphPinLayout(layouts, GraphPinRef{ .Node = 3, .Pin = "Nope" }) == nullptr);
			const std::optional<GraphPinHit> hit = FindGraphPinAt(layouts, b->Center + glm::vec2(3.0f, 0.0f), 5.0f);
			REQUIRE(hit.has_value());
			CHECK(*hit == GraphPinHit{ .Node = 3, .Pin = "B", .Direction = GraphPinDirection::Input });
			CHECK_FALSE(FindGraphPinAt(layouts, b->Center + glm::vec2(9.0f, 0.0f), 5.0f).has_value());
			// Node 1's output lies under node 2's box: node 2 is on top, but has no pin there, so the output is found.
			const std::optional<GraphPinHit> under = FindGraphPinAt(layouts, value->Center, 5.0f);
			REQUIRE(under.has_value());
			CHECK(under->Node == 1);

			CHECK(FindGraphNodesInRect(layouts, glm::vec2(500.0f, 500.0f), glm::vec2(130.0f, 5.0f)) == std::vector<uint32_t>{ 2, 3 });
			CHECK(FindGraphNodesInRect(layouts, glm::vec2(-50.0f), glm::vec2(-10.0f)).empty());

			// A link's curve leaves its output to the right and enters its input from the left.
			const auto curve = GetGraphLinkCurve(glm::vec2(0.0f), glm::vec2(200.0f, 100.0f));
			CHECK(curve[0] == glm::vec2(0.0f));
			CHECK(curve[1].x > 0.0f);
			CHECK(curve[1].y == 0.0f);
			CHECK(curve[2].x < 200.0f);
			CHECK(curve[2].y == 100.0f);
			CHECK(GetGraphLinkDistance(curve, glm::vec2(100.0f, 50.0f)) < 1.0f);
			CHECK(GetGraphLinkDistance(curve, glm::vec2(100.0f, 90.0f)) > 20.0f);

			const std::vector<GraphLink> links = { GraphLink{ .From = { .Node = 1, .Pin = "Value" }, .To = { .Node = 3, .Pin = "B" } },
				GraphLink{ .From = { .Node = 1, .Pin = "Value" }, .To = { .Node = 9, .Pin = "Missing" } } };
			const glm::vec2 middle = (value->Center + b->Center) * 0.5f;
			const std::optional<GraphLink> link = FindGraphLinkAt(links, layouts, middle, 4.0f);
			REQUIRE(link.has_value());
			CHECK(*link == links.front());
			CHECK_FALSE(FindGraphLinkAt(links, layouts, middle + glm::vec2(0.0f, 60.0f), 4.0f).has_value());
		}

		// W24.2's review: an input literal in its pin's label, cut by bytes at a UTF-8 boundary.
		TEST_CASE("GraphCanvas: a literal shows as its JSON text, a string quoted, cut to its byte limit never inside a UTF-8 sequence")
		{
			CHECK(FormatGraphLiteral(Json(2)) == "2");
			CHECK(FormatGraphLiteral(Json(true)) == "true");
			CHECK(FormatGraphLiteral(Json("door")) == "\"door\"");
			CHECK(FormatGraphLiteral(Json::array({ 1, 0, 0 })) == "[1,0,0]");
			// At the limit a literal is whole; past it, it keeps the bytes that fit before "...".
			const std::string fits(MaxGraphLiteralLabelBytes - 2, 'a');
			CHECK(FormatGraphLiteral(Json(fits)) == "\"" + fits + "\"");
			const std::string longer(MaxGraphLiteralLabelBytes, 'a');
			const std::string cut = FormatGraphLiteral(Json(longer));
			CHECK(cut.size() == MaxGraphLiteralLabelBytes);
			CHECK(cut == "\"" + std::string(MaxGraphLiteralLabelBytes - 4, 'a') + "...");
			// A two-byte character across the cut is left out whole, not split.
			const std::string accented = std::string(MaxGraphLiteralLabelBytes - 5, 'a') + "Ã©tail";
			CHECK(FormatGraphLiteral(Json(accented)) == "\"" + std::string(MaxGraphLiteralLabelBytes - 5, 'a') + "...");
		}
	}

}
