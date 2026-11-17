#include "TestsPCH.h"

#include "Engine/Scene/UiLayout.h"

#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiLayoutElementComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

// UI layout (M22, C22.1; Docs/Decisions/0033-m22-contract.md decisions 4 and 5): rectangles in scene pixels from anchors,
// pivots, positions, sizes and canvas scales; pixel snapping; clipping; draw order. Every expected value is the exact result
// of IEEE-exact float arithmetic, so the unit suite's Debug and Release runs check that the tables are identical across
// configurations (Roadmap M22's acceptance).

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
		TEST_CASE("UiLayout: a rectangle's clip covers the pixels whose centres it holds")
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

		TEST_CASE("UiLayout: anchors, pivot, position and size place an element in its parent in scene pixels")
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

		TEST_CASE("UiLayout: a canvas maps its units to scene pixels by ScaleFactor or by MatchHeight")
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

		TEST_CASE("UiLayout: PixelSnap rounds every corner to whole scene pixels before the children are placed")
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

		TEST_CASE("UiLayout: ClipChildren clips the descendants to the intersection of their clipping ancestors, not the element itself")
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

		TEST_CASE("UiLayout: canvases are in SortOrder, ties in entity order, each with its elements in canonical order")
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

		TEST_CASE("UiLayout: an effectively disabled element, its subtree and an element without a canvas are not laid out")
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

		TEST_CASE("UiLayout: a canvas below another canvas's element starts a tree of its own over the whole scene image")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity outer = AddCanvas(scene, "Outer", MakeUnsnappedCanvas());
			Entity panel = AddElement(scene, "Panel", outer, MakeTopLeftRect(glm::vec2(10.0f), glm::vec2(50.0f)));
			Entity nested = scene.CreateEntity("Nested", panel);
			nested.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .ScaleFactor = 2.0f, .PixelSnap = false });
			Entity inner = AddElement(scene, "Inner", nested, MakeTopLeftRect(glm::vec2(5.0f), glm::vec2(10.0f)));
			// After the nested canvas's subtree, Panel's next child is the outer canvas's again.
			Entity after = AddElement(scene, "After", panel, MakeTopLeftRect(glm::vec2(1.0f), glm::vec2(2.0f)));

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 100));
			REQUIRE(table.Canvases.size() == 2);
			CHECK(table.Canvases[0].Canvas == outer.GetUUID());
			CHECK(table.Canvases[0].NodeCount == 2);
			CHECK(table.Canvases[1].Canvas == nested.GetUUID());
			CHECK(table.Canvases[1].FirstNode == 2);
			CHECK(table.Canvases[1].NodeCount == 1);
			CHECK(table.Canvases[1].Size == glm::vec2(100.0f, 50.0f));
			REQUIRE(table.Nodes.size() == 3);
			CHECK(table.Nodes[0].Entity == panel.GetUUID());
			CHECK(table.Nodes[1].Entity == after.GetUUID());
			CHECK(table.Nodes[2].Entity == inner.GetUUID());

			// The nested canvas's element is placed in the scene image at its own scale, not in Panel.
			const UiLayoutNode& innerNode = RequireNode(table, inner);
			CHECK(innerNode.Canvas == nested.GetUUID());
			CHECK(innerNode.Parent == nested.GetUUID());
			CHECK(innerNode.Depth == 1);
			CHECK(innerNode.Min == glm::vec2(10.0f));
			CHECK(innerNode.Max == glm::vec2(30.0f));
			const UiLayoutNode& afterNode = RequireNode(table, after);
			CHECK(afterNode.Canvas == outer.GetUUID());
			CHECK(afterNode.Parent == panel.GetUUID());
			CHECK(afterNode.Depth == 2);
			CHECK(afterNode.Min == glm::vec2(11.0f));
			CHECK(FindUiLayoutNode(table, nested.GetUUID()) == nullptr);
		}

		TEST_CASE("UiLayout: an element is visible only when its rectangle covers a pixel of the scene image")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// Without a canvas the table is empty, at the scene resolution it was asked for.
			const UiLayoutTable withoutCanvas = ComputeUiLayout(scene, glm::uvec2(200, 100));
			CHECK(withoutCanvas.SceneSize == glm::uvec2(200, 100));
			CHECK(withoutCanvas.Canvases.empty());
			CHECK(withoutCanvas.Nodes.empty());

			Entity canvas = AddCanvas(scene, "Canvas", MakeUnsnappedCanvas());
			Entity inside = AddElement(scene, "Inside", canvas, MakeTopLeftRect(glm::vec2(10.0f), glm::vec2(10.0f)));
			Entity straddling = AddElement(scene, "Straddling", canvas, MakeTopLeftRect(glm::vec2(-5.0f), glm::vec2(10.0f)));
			Entity beyond = AddElement(scene, "Beyond", canvas, MakeTopLeftRect(glm::vec2(200.0f, 10.0f), glm::vec2(10.0f)));
			Entity empty = AddElement(scene, "Empty", canvas, MakeTopLeftRect(glm::vec2(10.0f), glm::vec2(0.0f, 10.0f)));
			// From x 10.2 to 10.4: no pixel centre (10.5) inside.
			Entity sliver = AddElement(scene, "Sliver", canvas, MakeTopLeftRect(glm::vec2(10.2f, 10.0f), glm::vec2(0.2f, 10.0f)));

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 100));
			CHECK(RequireNode(table, inside).Visible);
			CHECK(RequireNode(table, straddling).Visible);
			CHECK_FALSE(RequireNode(table, beyond).Visible);
			CHECK_FALSE(RequireNode(table, empty).Visible);
			CHECK_FALSE(RequireNode(table, sliver).Visible);
			CHECK_FALSE(RequireNode(table, inside).Clipped);
		}

		TEST_CASE("UiLayout: a container places its children, and an element that ignores the layout or is disabled takes no space")
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

		// C22.2 (Docs/Decisions/0033-m22-contract.md decision 26; stream B): a slider's anchors along its direction follow its
		// value as read, and an off toggle's Graphic is laid out but hidden with its subtree.
		TEST_CASE("UiLayout: a slider's value places its Fill and Handle, and an off toggle hides its Graphic")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", UiCanvasComponent{});
			Entity slider = AddElement(scene, "Slider", canvas, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(100.0f, 20.0f)));
			Entity fill = AddElement(scene, "Fill", slider, UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f, 1.0f), .Size = glm::vec2(0.0f) });
			Entity handle = AddElement(scene, "Handle", slider, UiRectComponent{ .AnchorMin = glm::vec2(0.0f, 0.5f), .AnchorMax = glm::vec2(0.0f, 0.5f), .Size = glm::vec2(10.0f, 20.0f) });
			// Value 3 of [1, 9] reads as 3: a quarter along.
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Value = 3.0f, .Min = 1.0f, .Max = 9.0f, .Fill = fill.GetUUID(), .Handle = handle.GetUUID() });
			Entity vertical = AddElement(scene, "Vertical", canvas, MakeTopLeftRect(glm::vec2(200.0f, 0.0f), glm::vec2(20.0f, 100.0f)));
			Entity column = AddElement(scene, "Column", vertical, UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(1.0f, 0.0f), .Size = glm::vec2(0.0f) });
			// A value past Max reads as Max: the fill covers the whole height.
			vertical.AddComponent<UiSliderComponent>(UiSliderComponent{ .Value = 2.0f, .Direction = UiLayoutDirection::Vertical, .Fill = column.GetUUID() });
			Entity toggle = AddElement(scene, "Toggle", canvas, MakeTopLeftRect(glm::vec2(0.0f, 50.0f), glm::vec2(20.0f)));
			Entity check = AddElement(scene, "Check", toggle, UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(1.0f), .Size = glm::vec2(-4.0f) });
			Entity mark = AddElement(scene, "Mark", check, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(4.0f)));
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{ .Graphic = check.GetUUID() });

			UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			CHECK(RequireNode(table, fill).Min == glm::vec2(0.0f, 0.0f));
			CHECK(RequireNode(table, fill).Max == glm::vec2(25.0f, 20.0f));
			CHECK(RequireNode(table, handle).Min == glm::vec2(20.0f, 0.0f));
			CHECK(RequireNode(table, handle).Max == glm::vec2(30.0f, 20.0f));
			CHECK(RequireNode(table, column).Min == glm::vec2(200.0f, 0.0f));
			CHECK(RequireNode(table, column).Max == glm::vec2(220.0f, 100.0f));
			// The toggle is off: its check mark and the mark inside it are laid out, hidden and not visible.
			CHECK(RequireNode(table, check).Min == glm::vec2(2.0f, 52.0f));
			CHECK(RequireNode(table, check).Hidden);
			CHECK_FALSE(RequireNode(table, check).Visible);
			CHECK(RequireNode(table, mark).Hidden);
			CHECK_FALSE(RequireNode(table, toggle).Hidden);

			toggle.GetComponent<UiToggleComponent>().IsOn = true;
			table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			CHECK_FALSE(RequireNode(table, check).Hidden);
			CHECK(RequireNode(table, check).Visible);
			CHECK(RequireNode(table, mark).Visible);
		}

		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 12): a HorizontalReverse slider's Min is at its right edge: its
		// Fill runs from the right edge to the value, and its Handle sits at 1 - t, as a Vertical slider counts from its far edge.
		TEST_CASE("UiLayout: a HorizontalReverse slider fills from its right edge and places its Handle at 1 - t")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", UiCanvasComponent{});
			Entity slider = AddElement(scene, "Slider", canvas, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(100.0f, 20.0f)));
			Entity fill = AddElement(scene, "Fill", slider, UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f, 1.0f), .Size = glm::vec2(0.0f) });
			Entity handle = AddElement(scene, "Handle", slider, UiRectComponent{ .AnchorMin = glm::vec2(0.0f, 0.5f), .AnchorMax = glm::vec2(0.0f, 0.5f), .Size = glm::vec2(10.0f, 20.0f) });
			// Value 3 of [1, 9]: a quarter along from the right.
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Value = 3.0f, .Min = 1.0f, .Max = 9.0f, .Direction = UiLayoutDirection::HorizontalReverse, .Fill = fill.GetUUID(), .Handle = handle.GetUUID() });
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			CHECK(RequireNode(table, fill).Min == glm::vec2(75.0f, 0.0f));
			CHECK(RequireNode(table, fill).Max == glm::vec2(100.0f, 20.0f));
			CHECK(RequireNode(table, handle).Min == glm::vec2(70.0f, 0.0f));
			CHECK(RequireNode(table, handle).Max == glm::vec2(80.0f, 20.0f));
		}

		// C22.2 (decision 26; stream C): a scroll view clips its descendants whatever ClipChildren says, and its content moves
		// by the scroll position clamped to the content's overflow, along the axes it scrolls.
		TEST_CASE("UiLayout: a scroll view clips its descendants and moves its content by the clamped scroll position")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", UiCanvasComponent{});
			Entity view = AddElement(scene, "View", canvas, MakeTopLeftRect(glm::vec2(10.0f), glm::vec2(100.0f, 40.0f)));
			Entity content = AddElement(scene, "Content", view, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(100.0f, 200.0f)));
			Entity item = AddElement(scene, "Item", content, MakeTopLeftRect(glm::vec2(0.0f, 50.0f), glm::vec2(100.0f, 20.0f)));
			// x does not scroll (Horizontal is off): its 30 is ignored.
			view.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = content.GetUUID(), .ScrollPosition = glm::vec2(30.0f) });

			UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			CHECK(RequireNode(table, content).Min == glm::vec2(10.0f, -20.0f));
			CHECK(RequireNode(table, content).Max == glm::vec2(110.0f, 180.0f));
			const UiLayoutNode& itemNode = RequireNode(table, item);
			CHECK(itemNode.Min == glm::vec2(10.0f, 30.0f));
			CHECK(itemNode.Max == glm::vec2(110.0f, 50.0f));
			CHECK(itemNode.Clipped);
			CHECK(itemNode.Clip == UiClipRect{ .MinX = 10, .MinY = 10, .MaxX = 110, .MaxY = 50 });
			CHECK_FALSE(RequireNode(table, view).Clipped);

			// Past the overflow (200 - 40): the content stops at its bottom.
			view.GetComponent<UiScrollViewComponent>().ScrollPosition = glm::vec2(0.0f, 500.0f);
			table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			CHECK(RequireNode(table, content).Min == glm::vec2(10.0f, -150.0f));
		}

		// Decision 26: the offset is in canvas units times the canvas's scale, so at 1.5 scene pixels per unit a scroll of 33
		// moves the content 49.5 pixels, and PixelSnap snaps the moved corners again (keeping the content's size); without
		// PixelSnap they stay where they land.
		TEST_CASE("UiLayout: a scrolled content's corners snap again after the move at a fractional scale")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", UiCanvasComponent{ .ScaleFactor = 1.5f });
			Entity view = AddElement(scene, "View", canvas, MakeTopLeftRect(glm::vec2(10.0f), glm::vec2(100.0f, 40.0f)));
			Entity content = AddElement(scene, "Content", view, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(300.0f, 40.0f)));
			Entity cell = AddElement(scene, "Cell", content, MakeTopLeftRect(glm::vec2(10.0f, 0.0f), glm::vec2(20.0f, 40.0f)));
			// Horizontal only: y does not scroll, and x reads 33 of an overflow of 200.
			view.AddComponent<UiScrollViewComponent>(
				UiScrollViewComponent{ .Content = content.GetUUID(),
					.Horizontal = true,
					.Vertical = false,
					.ScrollPosition = glm::vec2(33.0f, 10.0f) });

			UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(640, 360));
			CHECK(RequireNode(table, view).Min == glm::vec2(15.0f, 15.0f));
			CHECK(RequireNode(table, view).Max == glm::vec2(165.0f, 75.0f));
			// 15 - 49.5 = -34.5 and 465 - 49.5 = 415.5 snap to -34 and 416.
			CHECK(RequireNode(table, content).Min == glm::vec2(-34.0f, 15.0f));
			CHECK(RequireNode(table, content).Max == glm::vec2(416.0f, 75.0f));
			const UiLayoutNode& cellNode = RequireNode(table, cell);
			CHECK(cellNode.Min == glm::vec2(-19.0f, 15.0f));
			CHECK(cellNode.Max == glm::vec2(11.0f, 75.0f));
			CHECK(cellNode.Clip == UiClipRect{ .MinX = 15, .MinY = 15, .MaxX = 165, .MaxY = 75 });

			canvas.GetComponent<UiCanvasComponent>().PixelSnap = false;
			table = ComputeUiLayout(scene, glm::uvec2(640, 360));
			CHECK(RequireNode(table, content).Min == glm::vec2(-34.5f, 15.0f));
			CHECK(RequireNode(table, content).Max == glm::vec2(415.5f, 75.0f));
		}

		// C22.2 (decision 26; streams B and C, the review's m9): a Fill, Handle, Graphic or Content that is not an element below
		// its widget in the same canvas (a sibling, an element of another canvas, the widget itself, an ancestor) is ignored:
		// it keeps its own placement and stays shown. Live: it holds on the contract tree, whose layout applies no widget rule
		// yet, and keeps holding once streams B and C add the rules.
		TEST_CASE("UiLayout: a widget part that is not an element below its widget in the same canvas is ignored")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", UiCanvasComponent{});
			Entity outside = AddElement(scene, "Outside", canvas, MakeTopLeftRect(glm::vec2(150.0f, 0.0f), glm::vec2(40.0f, 10.0f)));
			Entity other = AddCanvas(scene, "Other", UiCanvasComponent{ .SortOrder = 1 });
			Entity elsewhere = AddElement(scene, "Elsewhere", other, MakeTopLeftRect(glm::vec2(50.0f, 150.0f), glm::vec2(10.0f)));
			// A slider at a half whose Fill is a sibling and whose Handle is in another canvas.
			Entity slider = AddElement(scene, "Slider", canvas, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(100.0f, 20.0f)));
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Value = 0.5f, .Fill = outside.GetUUID(), .Handle = elsewhere.GetUUID() });
			// Off toggles whose Graphic is a sibling, and the toggle itself.
			Entity toggle = AddElement(scene, "Toggle", canvas, MakeTopLeftRect(glm::vec2(0.0f, 30.0f), glm::vec2(20.0f)));
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{ .Graphic = outside.GetUUID() });
			Entity self = AddElement(scene, "Self", canvas, MakeTopLeftRect(glm::vec2(30.0f, 30.0f), glm::vec2(20.0f)));
			self.AddComponent<UiToggleComponent>(UiToggleComponent{ .Graphic = self.GetUUID() });
			// A scroll view whose Content is the element holding it.
			Entity panel = AddElement(scene, "Panel", canvas, MakeTopLeftRect(glm::vec2(0.0f, 60.0f), glm::vec2(200.0f, 60.0f)));
			Entity view = AddElement(scene, "View", panel, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(100.0f, 40.0f)));
			view.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = panel.GetUUID(), .ScrollPosition = glm::vec2(0.0f, 30.0f) });

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(320, 180));
			const UiLayoutNode& outsideNode = RequireNode(table, outside);
			CHECK(outsideNode.Min == glm::vec2(150.0f, 0.0f));
			CHECK(outsideNode.Max == glm::vec2(190.0f, 10.0f));
			CHECK_FALSE(outsideNode.Hidden);
			CHECK(outsideNode.Visible);
			CHECK(RequireNode(table, elsewhere).Min == glm::vec2(50.0f, 150.0f));
			CHECK(RequireNode(table, elsewhere).Max == glm::vec2(60.0f, 160.0f));
			CHECK_FALSE(RequireNode(table, self).Hidden);
			CHECK(RequireNode(table, self).Visible);
			CHECK(RequireNode(table, panel).Min == glm::vec2(0.0f, 60.0f));
			CHECK(RequireNode(table, view).Min == glm::vec2(0.0f, 60.0f));
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 48; stream T).
		TEST_CASE("UiLayout: a text input clips its descendants and hides its Placeholder while its Text is not empty")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddCanvas(scene, "Canvas", MakeUnsnappedCanvas());
			Entity input = AddElement(scene, "Input", canvas, MakeTopLeftRect(glm::vec2(10.0f, 20.0f), glm::vec2(100.0f, 30.0f)));
			Entity text = AddElement(scene, "Text", input, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(300.0f, 30.0f)));
			Entity hint = AddElement(scene, "Hint", input, MakeTopLeftRect(glm::vec2(0.0f), glm::vec2(100.0f, 30.0f)));
			input.AddComponent<UiTextInputComponent>(UiTextInputComponent{ .TextElement = text.GetUUID(), .Placeholder = hint.GetUUID() });

			// Empty: the placeholder shows; the text element, wider than the input, is clipped to it (ClipChildren is off).
			UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(400, 300));
			CHECK_FALSE(RequireNode(table, hint).Hidden);
			CHECK(RequireNode(table, hint).Visible);
			CHECK(RequireNode(table, text).Clipped);
			CHECK(RequireNode(table, text).Clip == UiClipRect{ .MinX = 10, .MinY = 20, .MaxX = 110, .MaxY = 50 });
			CHECK_FALSE(RequireNode(table, input).Clipped);

			// With text: the placeholder is hidden, laid out as before.
			input.GetComponent<UiTextInputComponent>().Text = "abc";
			table = ComputeUiLayout(scene, glm::uvec2(400, 300));
			CHECK(RequireNode(table, hint).Hidden);
			CHECK_FALSE(RequireNode(table, hint).Visible);
			CHECK(RequireNode(table, hint).Min == glm::vec2(10.0f, 20.0f));
		}

		// C22.3 (decision 49; stream W).
		TEST_CASE("UiLayout: a World canvas lays out over its own size and carries its world matrix")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A MatchHeight World canvas 400 x 200 canvas pixels with a reference height of 100 (2 pixels per unit) at 100
			// pixels per world unit, translated to (1, 2, 3); the scene image is 1920 x 1080 and changes nothing.
			Entity canvas = AddCanvas(scene, "Board",
				UiCanvasComponent{ .ScaleMode = UiScaleMode::MatchHeight,
					.ReferenceHeight = 100.0f,
					.RenderMode = UiCanvasRenderMode::World,
					.WorldWidth = 400,
					.WorldHeight = 200,
					.PixelsPerUnit = 100.0f });
			canvas.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			TransformSystem::Update(scene);
			Entity panel = AddElement(scene, "Panel", canvas, MakeTopLeftRect(glm::vec2(10.0f, 5.0f), glm::vec2(50.0f, 25.0f)));
			// Beyond the canvas's 400-pixel width: laid out but not visible, whatever the scene image.
			Entity outside = AddElement(scene, "Outside", canvas, MakeTopLeftRect(glm::vec2(250.0f, 0.0f), glm::vec2(10.0f, 10.0f)));

			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(1920, 1080));
			REQUIRE(table.Canvases.size() == 1);
			const UiCanvasLayout& layout = table.Canvases[0];
			CHECK(layout.RenderMode == UiCanvasRenderMode::World);
			CHECK(layout.SceneSize == glm::uvec2(400, 200));
			CHECK(layout.Scale == 2.0f);
			CHECK(layout.Size == glm::vec2(200.0f, 100.0f));
			CHECK(RequireNode(table, panel).Min == glm::vec2(20.0f, 10.0f));
			CHECK(RequireNode(table, panel).Max == glm::vec2(120.0f, 60.0f));
			CHECK(RequireNode(table, panel).Visible);
			CHECK_FALSE(RequireNode(table, outside).Visible);
			// Canvas pixel (0, 0) is the top-left corner: (-2, 1) units from the canvas's centre, at (1, 2, 3); pixel (400, 200)
			// the bottom-right one.
			CHECK(glm::vec3(layout.World * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)) == glm::vec3(-1.0f, 3.0f, 3.0f));
			CHECK(glm::vec3(layout.World * glm::vec4(400.0f, 200.0f, 0.0f, 1.0f)) == glm::vec3(3.0f, 1.0f, 3.0f));
		}
	}

}
