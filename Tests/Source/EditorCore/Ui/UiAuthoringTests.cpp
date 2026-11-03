#include "TestsPCH.h"

#include "EditorCore/Ui/UiAuthoring.h"

#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiLayout.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <optional>
#include <string_view>

// UI authoring (M22, C22.2, R-UI-EDITOR; Docs/Decisions/0033-m22-contract.md decisions 33 and 34): the anchor presets of the
// UiRect drawer, the rect tool's handles and drags, the preview's selection and its frame in the panel, as pure functions.

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		// Decision 34: a preset sets the anchors, and keeps the element's rectangle in its parent unless SetPosition moves it
		// to the anchors; SetPivot moves the pivot with them.
		TEST_CASE("UiAuthoring: anchor presets set the anchors and keep the rectangle unless asked to move it")
		{
			// A 100 x 40 element whose top-left corner is at (10, 20) in a 400 x 300 parent, anchored at its top-left corner.
			const UiRectComponent rect{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = glm::vec2(10.0f, 20.0f), .Size = glm::vec2(100.0f, 40.0f) };
			const glm::vec2 parent(400.0f, 300.0f);

			// Bottom-right, keeping the rectangle: the pivot stays at the top-left corner, now 390 and 280 units from the anchor.
			const UiRectComponent kept = ApplyUiAnchorPreset(rect, UiAnchorPreset{ UiAnchorHorizontal::Right, UiAnchorVertical::Bottom }, parent, {});
			CHECK(kept.AnchorMin == glm::vec2(1.0f));
			CHECK(kept.AnchorMax == glm::vec2(1.0f));
			CHECK(kept.Pivot == glm::vec2(0.0f));
			CHECK(kept.Position == glm::vec2(-390.0f, -280.0f));
			CHECK(kept.Size == glm::vec2(100.0f, 40.0f));

			// Stretched horizontally, keeping the rectangle: Size insets the parent's width by the margins (10 and 290).
			const UiRectComponent stretched = ApplyUiAnchorPreset(rect, UiAnchorPreset{ UiAnchorHorizontal::Stretch, UiAnchorVertical::Top }, parent, {});
			CHECK(stretched.AnchorMin == glm::vec2(0.0f));
			CHECK(stretched.AnchorMax == glm::vec2(1.0f, 0.0f));
			CHECK(stretched.Size == glm::vec2(-300.0f, 40.0f));

			// Centre with the pivot and position: it sits at the parent's centre.
			const UiRectComponent centred = ApplyUiAnchorPreset(rect, UiAnchorPreset{ UiAnchorHorizontal::Center, UiAnchorVertical::Middle }, parent,
				UiAnchorPresetOptions{ .SetPivot = true, .SetPosition = true });
			CHECK(centred.AnchorMin == glm::vec2(0.5f));
			CHECK(centred.Pivot == glm::vec2(0.5f));
			CHECK(centred.Position == glm::vec2(0.0f));
			CHECK(centred.Size == glm::vec2(100.0f, 40.0f));

			// Stretched both ways with the position: it fills the parent.
			const UiRectComponent filled = ApplyUiAnchorPreset(rect, UiAnchorPreset{ UiAnchorHorizontal::Stretch, UiAnchorVertical::Stretch }, parent,
				UiAnchorPresetOptions{ .SetPosition = true });
			CHECK(filled.AnchorMin == glm::vec2(0.0f));
			CHECK(filled.AnchorMax == glm::vec2(1.0f));
			CHECK(filled.Position == glm::vec2(0.0f));
			CHECK(filled.Size == glm::vec2(0.0f));

			// A header stretched across the parent, inset 8 units on each side (384 wide), moved to the centre: it keeps its
			// laid-out width instead of collapsing to its inset of -16.
			const UiRectComponent header{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(1.0f, 0.0f), .Pivot = glm::vec2(0.5f, 0.0f), .Position = glm::vec2(0.0f, 8.0f), .Size = glm::vec2(-16.0f, 40.0f) };
			const UiRectComponent centredHeader = ApplyUiAnchorPreset(header, UiAnchorPreset{ UiAnchorHorizontal::Center, UiAnchorVertical::Middle }, parent,
				UiAnchorPresetOptions{ .SetPosition = true });
			CHECK(centredHeader.AnchorMin == glm::vec2(0.5f));
			CHECK(centredHeader.AnchorMax == glm::vec2(0.5f));
			CHECK(centredHeader.Position == glm::vec2(0.0f));
			CHECK(centredHeader.Size == glm::vec2(384.0f, 40.0f));

			CHECK(FindUiAnchorPreset(rect) == UiAnchorPreset{ UiAnchorHorizontal::Left, UiAnchorVertical::Top });
			CHECK(FindUiAnchorPreset(filled) == UiAnchorPreset{ UiAnchorHorizontal::Stretch, UiAnchorVertical::Stretch });
			CHECK_FALSE(FindUiAnchorPreset(UiRectComponent{ .AnchorMin = glm::vec2(0.25f), .AnchorMax = glm::vec2(0.25f) }).has_value());
		}

		// Decision 34: the handles of a rectangle, and drags that move one edge and keep the opposite one in place.
		TEST_CASE("UiAuthoring: the rect tool moves an edge and keeps the opposite one in place")
		{
			const glm::vec2 min(100.0f, 100.0f);
			const glm::vec2 max(200.0f, 150.0f);
			CHECK(FindUiRectHandle(min, max, glm::vec2(101.0f, 102.0f), 4.0f) == UiRectHandle::TopLeft);
			CHECK(FindUiRectHandle(min, max, glm::vec2(198.0f, 150.0f), 4.0f) == UiRectHandle::BottomRight);
			CHECK(FindUiRectHandle(min, max, glm::vec2(150.0f, 99.0f), 4.0f) == UiRectHandle::Top);
			CHECK(FindUiRectHandle(min, max, glm::vec2(201.0f, 125.0f), 4.0f) == UiRectHandle::Right);
			CHECK(FindUiRectHandle(min, max, glm::vec2(150.0f, 125.0f), 4.0f) == UiRectHandle::Move);
			CHECK_FALSE(FindUiRectHandle(min, max, glm::vec2(300.0f, 125.0f), 4.0f).has_value());

			// A centred 100 x 50 element: its right edge moved 20 units right grows it to 120 and moves the pivot by half.
			const UiRectComponent start{ .Size = glm::vec2(100.0f, 50.0f) };
			const UiRectComponent right = DragUiRect(start, glm::vec2(100.0f, 50.0f), UiRectHandle::Right, glm::vec2(20.0f, 7.0f));
			CHECK(right.Size == glm::vec2(120.0f, 50.0f));
			CHECK(right.Position == glm::vec2(10.0f, 0.0f));
			// Its top edge moved 10 units down shrinks it to 40, the pivot following by half.
			const UiRectComponent top = DragUiRect(start, glm::vec2(100.0f, 50.0f), UiRectHandle::Top, glm::vec2(3.0f, 10.0f));
			CHECK(top.Size == glm::vec2(100.0f, 40.0f));
			CHECK(top.Position == glm::vec2(0.0f, 5.0f));
			// Dragged past the opposite edge, the size stops at 0.
			const UiRectComponent collapsed = DragUiRect(start, glm::vec2(100.0f, 50.0f), UiRectHandle::Left, glm::vec2(500.0f, 0.0f));
			CHECK(collapsed.Size == glm::vec2(0.0f, 50.0f));
			CHECK(collapsed.Position == glm::vec2(50.0f, 0.0f));
			const UiRectComponent moved = DragUiRect(start, glm::vec2(100.0f, 50.0f), UiRectHandle::Move, glm::vec2(-5.0f, 6.0f));
			CHECK(moved.Position == glm::vec2(-5.0f, 6.0f));
			CHECK(moved.Size == start.Size);
		}

		// Decision 33: the preview shows the game frame fitted in the panel; its points map to the frame's scene pixels, where
		// the topmost element holding a point is selected.
		TEST_CASE("UiAuthoring: the UI preview fits the game frame in the panel and selects the topmost element at a point")
		{
			CHECK(GetUiPreviewFrameSize(glm::uvec2(0), WindowSettings{ .Width = 1280, .Height = 720 }) == glm::uvec2(1280, 720));
			CHECK(GetUiPreviewFrameSize(glm::uvec2(320, 180), WindowSettings{ .Width = 1280, .Height = 720 }) == glm::uvec2(320, 180));

			// A 1600 x 900 frame in a 400 x 400 panel at (10, 20): a quarter scale, letterboxed above and below.
			const UiPreviewFrame frame = ComputeUiPreviewFrame(glm::uvec2(1600, 900), glm::vec2(10.0f, 20.0f), glm::vec2(400.0f));
			CHECK(frame.Scale == 0.25f);
			CHECK(frame.Size == glm::vec2(400.0f, 225.0f));
			CHECK(frame.Min == glm::vec2(10.0f, 107.5f));
			CHECK(MapUiPreviewPointToScene(frame, glm::vec2(110.0f, 132.5f)) == glm::vec2(400.0f, 100.0f));
			CHECK(ComputeUiPreviewFrame(glm::uvec2(1600, 900), glm::vec2(0.0f), glm::vec2(0.0f)).Scale == 0.0f);

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity panel = scene.CreateEntity("Panel", canvas);
			panel.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Size = glm::vec2(200.0f) });
			Entity group = scene.CreateEntity("Group", panel);
			group.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = glm::vec2(50.0f), .Size = glm::vec2(20.0f) });
			// An off toggle at (100, 10), 40 x 40, whose Graphic "Check" covers (105, 15) to (135, 45), and a scroll view at
			// (100, 100), 50 x 50, whose 150-tall content is clipped to it (decisions 26 and 34).
			const auto addElement = [&scene](std::string_view name, Entity parent, glm::vec2 position, glm::vec2 size)
			{
				Entity element = scene.CreateEntity(name, parent);
				element.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
					.AnchorMax = glm::vec2(0.0f),
					.Pivot = glm::vec2(0.0f),
					.Position = position,
					.Size = size });
				return element;
			};
			Entity toggle = addElement("Toggle", panel, glm::vec2(100.0f, 10.0f), glm::vec2(40.0f));
			Entity check = addElement("Check", toggle, glm::vec2(5.0f), glm::vec2(30.0f));
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{ .Graphic = check.GetUUID() });
			Entity list = addElement("List", panel, glm::vec2(100.0f, 100.0f), glm::vec2(50.0f));
			Entity content = addElement("Content", list, glm::vec2(0.0f), glm::vec2(50.0f, 150.0f));
			list.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = content.GetUUID() });
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(400, 300));
			// The group draws nothing, and is still selected over the panel.
			CHECK(PickUiElement(table, glm::vec2(55.0f)) == group.GetUUID());
			CHECK(PickUiElement(table, glm::vec2(10.0f)) == panel.GetUUID());
			CHECK_FALSE(PickUiElement(table, glm::vec2(300.0f)).IsValid());
			// The hidden Graphic is passed over for the toggle below it; the content is picked inside its view only, and its
			// clipped part leaves the click to the panel.
			REQUIRE(FindUiLayoutNode(table, check.GetUUID()) != nullptr);
			CHECK(FindUiLayoutNode(table, check.GetUUID())->Hidden);
			CHECK(PickUiElement(table, glm::vec2(110.0f, 20.0f)) == toggle.GetUUID());
			CHECK(PickUiElement(table, glm::vec2(110.0f, 120.0f)) == content.GetUUID());
			CHECK(PickUiElement(table, glm::vec2(110.0f, 170.0f)) == panel.GetUUID());
		}
	}

}
