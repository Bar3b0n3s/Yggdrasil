#include "TestsPCH.h"

#include "Engine/Scene/UiInteraction.h"

#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiLayout.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <string_view>

// UI interaction's pure functions (M22, C22.2, R-UI-INPUT; Docs/Decisions/0033-m22-contract.md decisions 22 and 23): the hit
// test of pointer targets, directional and sequential navigation and the stick's direction, on tables ComputeUiLayout lays out
// at one scene pixel per canvas unit, so every coordinate below is exact.

namespace Engine {

	namespace {

		// A rectangle at `position` of `size` from its parent's top-left corner.
		UiRectComponent MakeInteractionRect(glm::vec2 position, glm::vec2 size)
		{
			return UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = position, .Size = size };
		}

		Entity AddInteractionElement(Scene& scene, std::string_view name, Entity parent, glm::vec2 position, glm::vec2 size)
		{
			Entity entity = scene.CreateEntity(name, parent);
			entity.AddComponent<UiRectComponent>(MakeInteractionRect(position, size));
			return entity;
		}

		// A button with an image at `position` of `size`.
		Entity AddInteractionButton(Scene& scene, std::string_view name, Entity parent, glm::vec2 position, glm::vec2 size, UiButtonComponent button = {})
		{
			Entity entity = AddInteractionElement(scene, name, parent, position, size);
			entity.AddComponent<UiImageComponent>(UiImageComponent{});
			entity.AddComponent<UiButtonComponent>(button);
			return entity;
		}

		Entity AddInteractionCanvas(Scene& scene)
		{
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			return canvas;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("UiInteraction: widget kinds and UI events have their names and callbacks")
		{
			CHECK(UiWidgetTypeToString(UiWidgetType::None).empty());
			CHECK(UiWidgetTypeToString(UiWidgetType::Button) == "Button");
			CHECK(UiWidgetTypeToString(UiWidgetType::Toggle) == "Toggle");
			CHECK(UiWidgetTypeToString(UiWidgetType::Slider) == "Slider");
			CHECK(UiWidgetTypeToString(UiWidgetType::ScrollView) == "ScrollView");
			CHECK(UiEventKindToString(UiEventKind::Click) == "Click");
			CHECK(UiEventKindToString(UiEventKind::ValueChanged) == "ValueChanged");
			CHECK(UiEventKindToString(UiEventKind::Focus) == "Focus");
			CHECK(UiEventKindToString(UiEventKind::Hover) == "Hover");
			CHECK(UiEventKindToString(UiEventKind::Cancel) == "Cancel");
			CHECK(UiEventKindToString(UiEventKind::Scroll) == "Scroll");
			CHECK(GetUiEventCallbackName(UiEventKind::Click) == "OnUiClick");
			CHECK(GetUiEventCallbackName(UiEventKind::ValueChanged) == "OnUiValueChanged");
			CHECK(GetUiEventCallbackName(UiEventKind::Focus) == "OnUiFocus");
			CHECK(GetUiEventCallbackName(UiEventKind::Hover) == "OnUiHover");
			CHECK(GetUiEventCallbackName(UiEventKind::Cancel) == "OnUiCancel");
			CHECK(GetUiEventCallbackName(UiEventKind::Scroll) == "OnUiScroll");
			CHECK(UiInteractionState{}.IsEmpty());
			CHECK_FALSE(UiInteractionState{ .PointerOverUi = true }.IsEmpty());
		}

		// Decision 22: the topmost pointer target reaches its nearest widget; a target with RaycastTarget off passes the pointer
		// on, one without a widget blocks it, and a clip limits where a target is hit.
		TEST_CASE("UiInteraction: the topmost pointer target over a point reaches its nearest widget")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddInteractionCanvas(scene);
			const Entity play = AddInteractionButton(scene, "Play", canvas, glm::vec2(10.0f, 10.0f), glm::vec2(60.0f, 30.0f));
			Entity label = scene.CreateEntity("Label", play);
			label.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(1.0f), .Size = glm::vec2(0.0f) });
			label.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "Play" });
			const Entity covered = AddInteractionButton(scene, "Covered", canvas, glm::vec2(100.0f, 10.0f), glm::vec2(60.0f, 30.0f));
			Entity blocker = AddInteractionElement(scene, "Blocker", canvas, glm::vec2(100.0f, 10.0f), glm::vec2(60.0f, 30.0f));
			blocker.AddComponent<UiImageComponent>(UiImageComponent{});
			Entity overlay = AddInteractionElement(scene, "Overlay", canvas, glm::vec2(0.0f), glm::vec2(200.0f, 100.0f));
			overlay.AddComponent<UiImageComponent>(UiImageComponent{ .RaycastTarget = false });
			Entity panel = AddInteractionElement(scene, "Panel", canvas, glm::vec2(0.0f, 50.0f), glm::vec2(50.0f, 20.0f));
			panel.GetComponent<UiRectComponent>().ClipChildren = true;
			const Entity clipped = AddInteractionButton(scene, "Clipped", panel, glm::vec2(40.0f, 0.0f), glm::vec2(40.0f, 20.0f));
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 100));

			// Over the label: the label is the topmost target (the overlay lets the pointer through) and reaches its button.
			UiHit hit = HitTestUi(scene, table, glm::vec2(20.0f, 20.0f));
			CHECK(hit.Target == label.GetUUID());
			CHECK(hit.Widget == play.GetUUID());
			// Over the overlay alone: it is no target.
			hit = HitTestUi(scene, table, glm::vec2(5.0f, 5.0f));
			CHECK_FALSE(hit.Target.IsValid());
			CHECK_FALSE(hit.Widget.IsValid());
			// The blocker draws over the covered button and reaches no widget.
			hit = HitTestUi(scene, table, glm::vec2(110.0f, 20.0f));
			CHECK(hit.Target == blocker.GetUUID());
			CHECK_FALSE(hit.Widget.IsValid());
			CHECK(hit.Target != covered.GetUUID());
			// The right edge is outside (Min <= p < Max).
			CHECK_FALSE(HitTestUi(scene, table, glm::vec2(70.0f, 20.0f)).Target.IsValid());
			CHECK(HitTestUi(scene, table, glm::vec2(69.5f, 39.5f)).Widget == play.GetUUID());
			// The clipped button is hit inside its clip only.
			CHECK(HitTestUi(scene, table, glm::vec2(45.0f, 55.0f)).Widget == clipped.GetUUID());
			CHECK_FALSE(HitTestUi(scene, table, glm::vec2(60.0f, 55.0f)).Widget.IsValid());

			CHECK(GetUiWidgetType(scene, play.GetUUID()) == UiWidgetType::Button);
			CHECK(GetUiWidgetType(scene, label.GetUUID()) == UiWidgetType::None);
			CHECK(IsUiInteractable(scene, play.GetUUID()));
			CHECK_FALSE(IsUiInteractable(scene, blocker.GetUUID()));
		}

		// Decision 23: the smallest along-distance plus twice the across-distance among the focusable elements ahead; a
		// disabled or unfocusable widget is never picked, nothing ahead keeps the focus, and without a focus only a canvas's
		// DefaultFocus is picked.
		TEST_CASE("UiInteraction: directional navigation picks the nearest focusable element along the direction")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddInteractionCanvas(scene);
			const glm::vec2 size(40.0f, 20.0f);
			const Entity a = AddInteractionButton(scene, "A", canvas, glm::vec2(0.0f, 0.0f), size);     // centre (20, 10)
			const Entity b = AddInteractionButton(scene, "B", canvas, glm::vec2(100.0f, 0.0f), size);   // (120, 10)
			const Entity c = AddInteractionButton(scene, "C", canvas, glm::vec2(0.0f, 100.0f), size);   // (20, 110)
			const Entity d = AddInteractionButton(scene, "D", canvas, glm::vec2(100.0f, 100.0f), size); // (120, 110)
			const Entity f = AddInteractionButton(scene, "F", canvas, glm::vec2(300.0f, 0.0f), size);   // (320, 10)
			const Entity e = AddInteractionButton(scene, "E", canvas, glm::vec2(60.0f, 0.0f), glm::vec2(20.0f), UiButtonComponent{ .Focusable = false });
			const Entity g = AddInteractionButton(scene, "G", canvas, glm::vec2(200.0f, 100.0f), size, UiButtonComponent{ .Interactable = false });
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(400, 200));

			// From A rightward: B (100 + 0) beats D (100 + 200) and F (300 + 0); E, between them, is not focusable.
			CHECK(FindUiNavigationTarget(scene, table, a.GetUUID(), UiDirection::Right) == b.GetUUID());
			CHECK(FindUiNavigationTarget(scene, table, a.GetUUID(), UiDirection::Down) == c.GetUUID());
			CHECK(FindUiNavigationTarget(scene, table, b.GetUUID(), UiDirection::Right) == f.GetUUID());
			CHECK(FindUiNavigationTarget(scene, table, d.GetUUID(), UiDirection::Up) == b.GetUUID());
			CHECK(FindUiNavigationTarget(scene, table, c.GetUUID(), UiDirection::Right) == d.GetUUID());
			// G, straight right of D, is disabled: F (200 + 200) is next.
			CHECK(FindUiNavigationTarget(scene, table, d.GetUUID(), UiDirection::Right) == f.GetUUID());
			// Nothing above A: focus stays.
			CHECK_FALSE(FindUiNavigationTarget(scene, table, a.GetUUID(), UiDirection::Up).IsValid());
			// Without a focus: nothing until the canvas names its DefaultFocus, then that element whatever the direction; a
			// DefaultFocus that is not focusable is passed over.
			CHECK_FALSE(FindUiNavigationTarget(scene, table, UUID{}, UiDirection::Left).IsValid());
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = d.GetUUID();
			CHECK(FindUiNavigationTarget(scene, table, UUID{}, UiDirection::Left) == d.GetUUID());
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = e.GetUUID();
			CHECK_FALSE(FindUiNavigationTarget(scene, table, UUID{}, UiDirection::Down).IsValid());
			// From E (laid out, though not focusable), centre (70, 10): C and D below score 100 + 2 * 50 each; the tie goes to C,
			// the first in draw order.
			CHECK(FindUiNavigationTarget(scene, table, e.GetUUID(), UiDirection::Down) == c.GetUUID());
			CHECK_FALSE(IsUiFocusable(scene, table, e.GetUUID()));
			CHECK_FALSE(IsUiFocusable(scene, table, g.GetUUID()));
			CHECK(IsUiFocusable(scene, table, a.GetUUID()));
		}

		// Decision 23: Next and Previous follow draw order and wrap; an element outside the scene image stays focusable, one
		// without an area does not. Without a focus they give the default focus: the DefaultFocus of the last canvas in draw
		// order whose DefaultFocus is focusable, or nothing.
		TEST_CASE("UiInteraction: Next and Previous cycle the focusable elements in draw order")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddInteractionCanvas(scene);
			const Entity a = AddInteractionButton(scene, "A", canvas, glm::vec2(0.0f), glm::vec2(40.0f, 20.0f));
			AddInteractionButton(scene, "Flat", canvas, glm::vec2(0.0f, 30.0f), glm::vec2(0.0f, 20.0f));
			const Entity b = AddInteractionButton(scene, "B", canvas, glm::vec2(0.0f, 60.0f), glm::vec2(40.0f, 20.0f));
			AddInteractionButton(scene, "Disabled", canvas, glm::vec2(0.0f, 90.0f), glm::vec2(40.0f, 20.0f), UiButtonComponent{ .Interactable = false });
			const Entity off = AddInteractionButton(scene, "Off", canvas, glm::vec2(-100.0f), glm::vec2(20.0f));
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 200));

			CHECK(FindNextUiFocus(scene, table, a.GetUUID(), true) == b.GetUUID());
			CHECK(FindNextUiFocus(scene, table, b.GetUUID(), true) == off.GetUUID());
			CHECK(FindNextUiFocus(scene, table, off.GetUUID(), true) == a.GetUUID());
			CHECK(FindNextUiFocus(scene, table, a.GetUUID(), false) == off.GetUUID());
			CHECK(FindNextUiFocus(scene, table, b.GetUUID(), false) == a.GetUUID());
			CHECK(IsUiFocusable(scene, table, off.GetUUID()));

			// Without a focus: nothing, then the canvas's DefaultFocus either way.
			CHECK_FALSE(FindNextUiFocus(scene, table, UUID{}, true).IsValid());
			CHECK_FALSE(FindNextUiFocus(scene, table, UUID{}, false).IsValid());
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = b.GetUUID();
			CHECK(FindNextUiFocus(scene, table, UUID{}, true) == b.GetUUID());
			CHECK(FindNextUiFocus(scene, table, UUID{}, false) == b.GetUUID());
			// A menu canvas drawn over the first names its own DefaultFocus, which wins until it is no longer focusable.
			Entity menu = scene.CreateEntity("Menu");
			menu.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .SortOrder = 1 });
			Entity resume = AddInteractionButton(scene, "Resume", menu, glm::vec2(100.0f, 0.0f), glm::vec2(40.0f, 20.0f));
			menu.GetComponent<UiCanvasComponent>().DefaultFocus = resume.GetUUID();
			const UiLayoutTable menuTable = ComputeUiLayout(scene, glm::uvec2(200, 200));
			CHECK(FindNextUiFocus(scene, menuTable, UUID{}, true) == resume.GetUUID());
			resume.GetComponent<UiButtonComponent>().Focusable = false;
			CHECK(FindNextUiFocus(scene, menuTable, UUID{}, true) == b.GetUUID());
		}

		// Decision 26: an off toggle's Graphic and its subtree are Hidden, laid out but never hit or focusable, so a button
		// inside it is passed over by the hit test, by Next and Previous and by the directions, and is not focusable; once
		// the toggle is on, it takes part again.
		TEST_CASE("UiInteraction: an element hidden in an off toggle's Graphic is neither hit nor focusable")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddInteractionCanvas(scene);
			const Entity a = AddInteractionButton(scene, "A", canvas, glm::vec2(0.0f), glm::vec2(40.0f, 20.0f)); // centre (20, 10)
			Entity toggle = AddInteractionElement(scene, "Toggle", canvas, glm::vec2(0.0f, 40.0f), glm::vec2(100.0f, 40.0f));
			const Entity inner = AddInteractionButton(scene, "Inner", toggle, glm::vec2(0.0f), glm::vec2(40.0f, 20.0f)); // (20, 50)
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{ .Focusable = false, .Graphic = inner.GetUUID() });
			const Entity b = AddInteractionButton(scene, "B", canvas, glm::vec2(0.0f, 100.0f), glm::vec2(40.0f, 20.0f)); // (20, 110)
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 200));
			const UiLayoutNode* hidden = FindUiLayoutNode(table, inner.GetUUID());
			REQUIRE(hidden != nullptr);
			REQUIRE(hidden->Hidden);

			CHECK_FALSE(IsUiFocusable(scene, table, inner.GetUUID()));
			CHECK(FindNextUiFocus(scene, table, a.GetUUID(), true) == b.GetUUID());
			CHECK(FindNextUiFocus(scene, table, b.GetUUID(), false) == a.GetUUID());
			CHECK(FindUiNavigationTarget(scene, table, a.GetUUID(), UiDirection::Down) == b.GetUUID());
			// Over the hidden button the toggle below it, a widget and so a pointer target, is the topmost target.
			CHECK(HitTestUi(scene, table, glm::vec2(10.0f, 50.0f)).Target == toggle.GetUUID());
			CHECK(HitTestUi(scene, table, glm::vec2(10.0f, 50.0f)).Widget == toggle.GetUUID());

			toggle.GetComponent<UiToggleComponent>().IsOn = true;
			const UiLayoutTable shown = ComputeUiLayout(scene, glm::uvec2(200, 200));
			CHECK(IsUiFocusable(scene, shown, inner.GetUUID()));
			CHECK(FindNextUiFocus(scene, shown, a.GetUUID(), true) == inner.GetUUID());
			CHECK(FindUiNavigationTarget(scene, shown, a.GetUUID(), UiDirection::Down) == inner.GetUUID());
			CHECK(HitTestUi(scene, shown, glm::vec2(10.0f, 50.0f)).Widget == inner.GetUUID());
		}

		TEST_CASE("UiInteraction: the stick's direction is its larger axis at one half or more")
		{
			CHECK(GetUiStickDirection(glm::vec2(0.6f, 0.2f)) == UiStickDirection::Right);
			CHECK(GetUiStickDirection(glm::vec2(-0.2f, 0.7f)) == UiStickDirection::Up);
			CHECK(GetUiStickDirection(glm::vec2(0.1f, -0.5f)) == UiStickDirection::Down);
			CHECK(GetUiStickDirection(glm::vec2(-0.7f, 0.7f)) == UiStickDirection::Left);
			CHECK(GetUiStickDirection(glm::vec2(0.4f, -0.4f)) == UiStickDirection::None);
			CHECK(GetUiStickDirection(glm::vec2(0.0f)) == UiStickDirection::None);
		}
	}

}
