#include "TestsPCH.h"

#include "Engine/Scene/UiLayout.h"

#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiLayoutElementComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

// UI layout (M22, C22.1; Docs/Decisions/0033-m22-contract.md decisions 4 and 5): rectangles in scene pixels from anchors,
// pivots, positions, sizes and canvas scales; pixel snapping; clipping; draw order. Every expected value is the exact result
// of IEEE-exact float arithmetic, so the unit suite's Debug and Release runs check that the tables are identical across
// configurations (Roadmap M22's acceptance). Skipped by the contract; stream A (stream D for the containers) un-skips them.

namespace Engine {

	namespace {

		Entity AddCanvas(Scene& scene, std::string_view name, const UiCanvasComponent& canvas)
		{
			Entity entity = scene.CreateEntity(name);
			entity.AddComponent<UiCanvasComponent>(canvas);
			return entity;
		}

		Entity AddElement(Scene& scene, std::string_view name, Entity parent, const UiRectComponent& rect)
		{
			Entity entity = scene.CreateEntity(name, parent);
			entity.AddComponent<UiRectComponent>(rect);
			return entity;
		}

		// A rectangle at `position` of `size` from its parent's top-left corner.
		UiRectComponent MakeTopLeftRect(glm::vec2 position, glm::vec2 size)
		{
			return UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = position, .Size = size };
		}

		const UiLayoutNode& RequireNode(const UiLayoutTable& table, Entity entity)
		{
			const UiLayoutNode* node = FindUiLayoutNode(table, entity.GetUUID());
			REQUIRE_MESSAGE(node != nullptr, entity.GetName());
			return *node;
		}

		// A canvas at one scene pixel per unit that keeps fractional corners.
		UiCanvasComponent MakeUnsnappedCanvas()
		{
			return UiCanvasComponent{ .PixelSnap = false };
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("UiLayout: a rectangle's clip covers the pixels whose centres it holds" * doctest::skip(true))
		{
			// Pixel x is covered when min <= x + 0.5 < max: ceil(v - 0.5) at both ends.
			CHECK(ComputeUiClipRect(glm::vec2(10.4f, 0.5f), glm::vec2(20.5f, 3.6f)) == UiClipRect{ .MinX = 10, .MinY = 0, .MaxX = 20, .MaxY = 4 });
			CHECK(ComputeUiClipRect(glm::vec2(-3.0f, 2.0f), glm::vec2(1.0f, 2.0f)) == UiClipRect{ .MinX = -3, .MinY = 2, .MaxX = 1, .MaxY = 2 });
			CHECK(ComputeUiClipRect(glm::vec2(-3.0f, 2.0f), glm::vec2(1.0f, 2.0f)).IsEmpty());
			// Beyond int32's range (a MatchHeight canvas at 1080 pixels per unit holding a rectangle at 2e6 units), each
			// coordinate clamps to plus or minus 2^30 before it converts.
			constexpr int32_t Limit = 1 << 30;
			CHECK(ComputeUiClipRect(glm::vec2(-3.0e9f, 5.0f), glm::vec2(2.16e9f, 3.0e9f)) == UiClipRect{ .MinX = -Limit, .MinY = 5, .MaxX = Limit, .MaxY = Limit });
			CHECK(ComputeUiClipRect(glm::vec2(2.16e9f, 0.0f), glm::vec2(3.0e9f, 1.0f)).IsEmpty());
		}

		TEST_CASE("UiLayout: anchors, pivot, position and size place an element in its parent in scene pixels" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", MakeUnsnappedCanvas());
			Entity corner = AddElement(scene, "Corner", canvas, MakeTopLeftRect(glm::vec2(10.0f, 20.0f), glm::vec2(50.0f, 30.0f)));
			// Stretched anchors: the canvas less 10 pixels on each side horizontally and 5 vertically.
			Entity inset = AddElement(scene, "Inset", canvas, UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(1.0f), .Size = glm::vec2(-20.0f, -10.0f) });
			// Anchored to its parent's bottom-right corner, pivot at its own.
			Entity badge = AddElement(scene, "Badge", corner,
				UiRectComponent{ .AnchorMin = glm::vec2(1.0f), .AnchorMax = glm::vec2(1.0f), .Pivot = glm::vec2(1.0f), .Size = glm::vec2(20.0f, 10.0f) });
			// The defaults: a 100 x 100 element centred in the canvas.
			Entity centred = AddElement(scene, "Centred", canvas, UiRectComponent{});

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 100));
			CHECK(table.SceneSize == glm::uvec2(200, 100));
			REQUIRE(table.Canvases.size() == 1);
			CHECK(table.Canvases[0].Canvas == canvas.GetUUID());
			CHECK(table.Canvases[0].Scale == 1.0f);
			CHECK(table.Canvases[0].Size == glm::vec2(200.0f, 100.0f));
			CHECK(table.Canvases[0].NodeCount == 4);

			const UiLayoutNode& cornerNode = RequireNode(table, corner);
			CHECK(cornerNode.Min == glm::vec2(10.0f, 20.0f));
			CHECK(cornerNode.Max == glm::vec2(60.0f, 50.0f));
			CHECK(cornerNode.Parent == canvas.GetUUID());
			CHECK(cornerNode.Canvas == canvas.GetUUID());
			CHECK(cornerNode.Depth == 1);
			CHECK(cornerNode.Visible);
			CHECK_FALSE(cornerNode.Clipped);

			const UiLayoutNode& insetNode = RequireNode(table, inset);
			CHECK(insetNode.Min == glm::vec2(10.0f, 5.0f));
			CHECK(insetNode.Max == glm::vec2(190.0f, 95.0f));

			const UiLayoutNode& badgeNode = RequireNode(table, badge);
			CHECK(badgeNode.Min == glm::vec2(40.0f, 40.0f));
			CHECK(badgeNode.Max == glm::vec2(60.0f, 50.0f));
			CHECK(badgeNode.Parent == corner.GetUUID());
			CHECK(badgeNode.Depth == 2);

			const UiLayoutNode& centredNode = RequireNode(table, centred);
			CHECK(centredNode.Min == glm::vec2(50.0f, 0.0f));
			CHECK(centredNode.Max == glm::vec2(150.0f, 100.0f));

			// Canonical order: Corner, its Badge, Inset, Centred.
			REQUIRE(table.Nodes.size() == 4);
			CHECK(table.Nodes[0].Entity == corner.GetUUID());
			CHECK(table.Nodes[1].Entity == badge.GetUUID());
			CHECK(table.Nodes[2].Entity == inset.GetUUID());
			CHECK(table.Nodes[3].Entity == centred.GetUUID());
		}

		TEST_CASE("UiLayout: a canvas maps its units to scene pixels by ScaleFactor or by MatchHeight" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const UiRectComponent rect = MakeTopLeftRect(glm::vec2(10.0f, 5.0f), glm::vec2(20.0f, 10.0f));
			Entity doubled = AddCanvas(scene, "Doubled", UiCanvasComponent{ .ScaleFactor = 2.0f, .PixelSnap = false });
			Entity doubledElement = AddElement(scene, "DoubledElement", doubled, rect);
			// 200 scene pixels high over 100 units: 2 pixels per unit.
			Entity matched = AddCanvas(scene, "Matched", UiCanvasComponent{ .ScaleMode = UiScaleMode::MatchHeight, .ReferenceHeight = 100.0f, .PixelSnap = false });
			Entity matchedElement = AddElement(scene, "MatchedElement", matched, rect);
			// 200 scene pixels high over 400 units: half a pixel per unit.
			Entity halved = AddCanvas(scene, "Halved", UiCanvasComponent{ .ScaleMode = UiScaleMode::MatchHeight, .ReferenceHeight = 400.0f, .PixelSnap = false });
			Entity halvedElement = AddElement(scene, "HalvedElement", halved, rect);

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(400, 200));
			REQUIRE(table.Canvases.size() == 3);
			CHECK(table.Canvases[0].Scale == 2.0f);
			CHECK(table.Canvases[0].Size == glm::vec2(200.0f, 100.0f));
			CHECK(table.Canvases[1].Scale == 2.0f);
			CHECK(table.Canvases[2].Scale == 0.5f);
			CHECK(table.Canvases[2].Size == glm::vec2(800.0f, 400.0f));
			CHECK(RequireNode(table, doubledElement).Min == glm::vec2(20.0f, 10.0f));
			CHECK(RequireNode(table, doubledElement).Max == glm::vec2(60.0f, 30.0f));
			CHECK(RequireNode(table, matchedElement).Min == glm::vec2(20.0f, 10.0f));
			CHECK(RequireNode(table, matchedElement).Max == glm::vec2(60.0f, 30.0f));
			CHECK(RequireNode(table, halvedElement).Min == glm::vec2(5.0f, 2.5f));
			CHECK(RequireNode(table, halvedElement).Max == glm::vec2(15.0f, 7.5f));
		}

		TEST_CASE("UiLayout: PixelSnap rounds every corner to whole scene pixels before the children are placed" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", UiCanvasComponent{});
			Entity panel = AddElement(scene, "Panel", canvas, MakeTopLeftRect(glm::vec2(10.4f, 20.6f), glm::vec2(5.2f, 5.2f)));
			// Centred in the panel: in the snapped panel [10, 21]-[16, 26] its centre is (13, 23.5), in the unsnapped one (13, 23.2).
			Entity dot = AddElement(scene, "Dot", panel, UiRectComponent{ .Pivot = glm::vec2(0.0f), .Size = glm::vec2(1.0f) });

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(64, 64));
			CHECK(RequireNode(table, panel).Min == glm::vec2(10.0f, 21.0f));
			CHECK(RequireNode(table, panel).Max == glm::vec2(16.0f, 26.0f));
			// [13, 23.5]-[14, 24.5] rounds half up to [13, 24]-[14, 25]; from the unsnapped panel it would be [13, 23]-[14, 24].
			CHECK(RequireNode(table, dot).Min == glm::vec2(13.0f, 24.0f));
			CHECK(RequireNode(table, dot).Max == glm::vec2(14.0f, 25.0f));
		}

		TEST_CASE("UiLayout: ClipChildren clips the descendants to the intersection of their clipping ancestors, not the element itself" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", MakeUnsnappedCanvas());
			UiRectComponent panelRect = MakeTopLeftRect(glm::vec2(10.0f), glm::vec2(40.0f));
			panelRect.ClipChildren = true;
			Entity panel = AddElement(scene, "Panel", canvas, panelRect);
			UiRectComponent innerRect = MakeTopLeftRect(glm::vec2(20.0f), glm::vec2(40.0f)); // [30, 70] in the scene
			innerRect.ClipChildren = true;
			Entity inner = AddElement(scene, "Inner", panel, innerRect);
			Entity deep = AddElement(scene, "Deep", inner, MakeTopLeftRect(glm::vec2(10.0f), glm::vec2(50.0f)));       // [40, 90]
			Entity outside = AddElement(scene, "Outside", panel, MakeTopLeftRect(glm::vec2(50.0f), glm::vec2(10.0f))); // [60, 70]

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(100, 100));
			CHECK_FALSE(RequireNode(table, panel).Clipped);
			const UiLayoutNode& innerNode = RequireNode(table, inner);
			CHECK(innerNode.Clipped);
			CHECK(innerNode.Clip == UiClipRect{ .MinX = 10, .MinY = 10, .MaxX = 50, .MaxY = 50 });
			CHECK(innerNode.Visible);
			const UiLayoutNode& deepNode = RequireNode(table, deep);
			CHECK(deepNode.Clipped);
			CHECK(deepNode.Clip == UiClipRect{ .MinX = 30, .MinY = 30, .MaxX = 50, .MaxY = 50 });
			CHECK(deepNode.Min == glm::vec2(40.0f));
			CHECK(deepNode.Visible);
			const UiLayoutNode& outsideNode = RequireNode(table, outside);
			CHECK(outsideNode.Clipped);
			CHECK_FALSE(outsideNode.Visible);
		}

		TEST_CASE("UiLayout: canvases are in SortOrder, ties in entity order, each with its elements in canonical order" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity front = AddCanvas(scene, "Front", UiCanvasComponent{ .SortOrder = 1 });
			Entity frontElement = AddElement(scene, "FrontElement", front, UiRectComponent{});
			Entity back = AddCanvas(scene, "Back", UiCanvasComponent{ .SortOrder = 0 });
			Entity backElement = AddElement(scene, "BackElement", back, UiRectComponent{});
			Entity middle = AddCanvas(scene, "Middle", UiCanvasComponent{ .SortOrder = 0 });
			Entity middleFirst = AddElement(scene, "MiddleFirst", middle, UiRectComponent{});
			Entity middleSecond = AddElement(scene, "MiddleSecond", middle, UiRectComponent{});

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			REQUIRE(table.Canvases.size() == 3);
			CHECK(table.Canvases[0].Canvas == back.GetUUID());
			CHECK(table.Canvases[1].Canvas == middle.GetUUID());
			CHECK(table.Canvases[2].Canvas == front.GetUUID());
			CHECK(table.Canvases[0].FirstNode == 0);
			CHECK(table.Canvases[0].NodeCount == 1);
			CHECK(table.Canvases[1].FirstNode == 1);
			CHECK(table.Canvases[1].NodeCount == 2);
			CHECK(table.Canvases[2].FirstNode == 3);
			REQUIRE(table.Nodes.size() == 4);
			CHECK(table.Nodes[0].Entity == backElement.GetUUID());
			CHECK(table.Nodes[1].Entity == middleFirst.GetUUID());
			CHECK(table.Nodes[2].Entity == middleSecond.GetUUID());
			CHECK(table.Nodes[3].Entity == frontElement.GetUUID());
			CHECK(FindUiCanvasLayout(table, middle.GetUUID()) == &table.Canvases[1]);
		}

		TEST_CASE("UiLayout: an effectively disabled element, its subtree and an element without a canvas are not laid out" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", UiCanvasComponent{});
			Entity hidden = AddElement(scene, "Hidden", canvas, UiRectComponent{});
			Entity hiddenChild = AddElement(scene, "HiddenChild", hidden, UiRectComponent{});
			hidden.SetActive(false);
			Entity shown = AddElement(scene, "Shown", canvas, UiRectComponent{});
			Entity stray = scene.CreateEntity("Stray");
			stray.AddComponent<UiRectComponent>(UiRectComponent{});
			Entity disabledCanvas = AddCanvas(scene, "DisabledCanvas", UiCanvasComponent{});
			AddElement(scene, "UnderDisabledCanvas", disabledCanvas, UiRectComponent{});
			disabledCanvas.SetActive(false);

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			REQUIRE(table.Canvases.size() == 1);
			REQUIRE(table.Nodes.size() == 1);
			CHECK(table.Nodes[0].Entity == shown.GetUUID());
			CHECK(FindUiLayoutNode(table, hidden.GetUUID()) == nullptr);
			CHECK(FindUiLayoutNode(table, hiddenChild.GetUUID()) == nullptr);
			CHECK(FindUiLayoutNode(table, stray.GetUUID()) == nullptr);
		}

		// Streams A and D: the stream that lands last of the two un-skips it.
		TEST_CASE("UiLayout: a container places its children, and an element that ignores the layout or is disabled takes no space" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", UiCanvasComponent{});
			Entity column = AddElement(scene, "Column", canvas, MakeTopLeftRect(glm::vec2(10.0f), glm::vec2(100.0f, 200.0f)));
			column.AddComponent<UiLayoutComponent>(UiLayoutComponent{ .Kind = UiLayoutKind::Stack,
				.Direction = UiLayoutDirection::Vertical,
				.Padding = UiPadding{ .Left = 5.0f, .Top = 5.0f, .Right = 5.0f, .Bottom = 5.0f },
				.Spacing = glm::vec2(0.0f, 4.0f) });
			Entity first = AddElement(scene, "First", column, UiRectComponent{ .Size = glm::vec2(50.0f, 20.0f) });
			Entity skipped = AddElement(scene, "Skipped", column, UiRectComponent{ .Size = glm::vec2(50.0f, 30.0f) });
			skipped.SetActive(false);
			Entity free = AddElement(scene, "Free", column, MakeTopLeftRect(glm::vec2(70.0f, 0.0f), glm::vec2(10.0f)));
			free.AddComponent<UiLayoutElementComponent>(UiLayoutElementComponent{ .IgnoreLayout = true });
			Entity second = AddElement(scene, "Second", column, UiRectComponent{ .Size = glm::vec2(60.0f, 10.0f) });
			// A grouping entity without a UiRect is transparent: the column places its element as its own child, after Second.
			Entity group = scene.CreateEntity("Group", column);
			Entity grouped = AddElement(scene, "Grouped", group, UiRectComponent{ .Size = glm::vec2(40.0f, 10.0f) });

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			// Inside the padding: First at (15, 15), then 4 units of spacing, then Second; Skipped and Free take no space.
			CHECK(RequireNode(table, first).Min == glm::vec2(15.0f, 15.0f));
			CHECK(RequireNode(table, first).Max == glm::vec2(65.0f, 35.0f));
			CHECK(RequireNode(table, second).Min == glm::vec2(15.0f, 39.0f));
			CHECK(RequireNode(table, second).Max == glm::vec2(75.0f, 49.0f));
			const UiLayoutNode& groupedNode = RequireNode(table, grouped);
			CHECK(groupedNode.Min == glm::vec2(15.0f, 53.0f));
			CHECK(groupedNode.Max == glm::vec2(55.0f, 63.0f));
			CHECK(groupedNode.Parent == column.GetUUID());
			CHECK(groupedNode.Depth == 2);
			CHECK(FindUiLayoutNode(table, group.GetUUID()) == nullptr);
			// Free keeps its own placement in the column's rectangle.
			CHECK(RequireNode(table, free).Min == glm::vec2(80.0f, 10.0f));
		}
	}

}
