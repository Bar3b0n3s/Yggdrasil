#include "TestsPCH.h"

#include "Engine/Scene/UiInteraction.h"

#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/PixelPerfectCameraComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scene/UiLayout.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <limits>
#include <optional>
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
			// C22.3 (Docs/Decisions/0033-m22-contract.md decisions 45 and 47): the text input and its events.
			CHECK(UiWidgetTypeToString(UiWidgetType::TextInput) == "TextInput");
			CHECK(UiEventKindToString(UiEventKind::TextChanged) == "TextChanged");
			CHECK(UiEventKindToString(UiEventKind::Submit) == "Submit");
			CHECK(GetUiEventCallbackName(UiEventKind::TextChanged) == "OnUiTextChanged");
			CHECK(GetUiEventCallbackName(UiEventKind::Submit) == "OnUiSubmit");
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

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 50; stream W): the pointer's ray and the world canvases' hit test.
		TEST_CASE("UiInteraction: the pointer's ray goes through the primary camera at its simulated pose")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			CHECK_FALSE(ComputeUiPointerRay(scene, glm::vec2(50.0f), glm::uvec2(100, 100)).has_value());

			// A perspective camera at (0, 0, 10) with a 90 degree field of view on a 200 x 100 image (aspect 2): the image's
			// centre looks down -Z; its top-right corner along (2, 1, -1), normalized.
			Entity camera = scene.CreateEntity("Camera");
			camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 10.0f);
			camera.AddComponent<CameraComponent>(CameraComponent{ .VerticalFov = 90.0f, .Primary = true });
			TransformSystem::Update(scene);
			std::optional<UiPointerRay> ray = ComputeUiPointerRay(scene, glm::vec2(100.0f, 50.0f), glm::uvec2(200, 100));
			REQUIRE(ray.has_value());
			CHECK(ray->Origin == glm::vec3(0.0f, 0.0f, 10.0f));
			CHECK(ray->Direction == glm::vec3(0.0f, 0.0f, -1.0f));
			ray = ComputeUiPointerRay(scene, glm::vec2(200.0f, 0.0f), glm::uvec2(200, 100));
			REQUIRE(ray.has_value());
			CHECK(ray->Direction.x == doctest::Approx(2.0f / 2.4494897f));
			CHECK(ray->Direction.y == doctest::Approx(1.0f / 2.4494897f));
			CHECK(ray->Direction.z == doctest::Approx(-1.0f / 2.4494897f));

			// The simulated pose, not a later one: moving the camera without a transform update leaves the ray as it was.
			camera.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, 10.0f);
			CHECK(ComputeUiPointerRay(scene, glm::vec2(100.0f, 50.0f), glm::uvec2(200, 100))->Origin == glm::vec3(0.0f, 0.0f, 10.0f));

			// An orthographic camera's rays are parallel, from the near plane: OrthographicSize 5 is the half height.
			camera.GetComponent<CameraComponent>().Projection = ProjectionType::Orthographic;
			camera.GetComponent<CameraComponent>().OrthographicSize = 5.0f;
			camera.GetComponent<CameraComponent>().NearClip = 1.0f;
			TransformSystem::Update(scene);
			ray = ComputeUiPointerRay(scene, glm::vec2(200.0f, 0.0f), glm::uvec2(200, 100));
			REQUIRE(ray.has_value());
			CHECK(ray->Origin == glm::vec3(15.0f, 5.0f, 9.0f));
			CHECK(ray->Direction == glm::vec3(0.0f, 0.0f, -1.0f));
		}

		// M22's landing (Docs/Decisions/0033-m22-contract.md decision 50, risk 11, "Landing"): a primary camera that holds M21's
		// PixelPerfectCamera is drawn orthographically at its pixel-snapped position whatever its Camera's projection, so the
		// pointer's ray is the inverse of that projection: parallel rays along -Z from the near plane of the snapped view.
		TEST_CASE("UiInteraction: under a PixelPerfectCamera the pointer's ray is the inverse of its pixel-perfect projection")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A perspective Camera at (0.03, 0.02, 10) holding a 200 x 120 PixelPerfectCamera of 16 pixels per unit: the view's
			// half height is 120 / 32 = 3.75 and its half width 6.25, and the snapped position is (0, 0, 10), since
			// round((0.03 - 6.25) 16) / 16 + 6.25 = 0 and round((0.02 - 3.75) 16) / 16 + 3.75 = 0.
			Entity camera = scene.CreateEntity("Camera");
			camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.03f, 0.02f, 10.0f);
			camera.AddComponent<CameraComponent>(CameraComponent{ .VerticalFov = 90.0f, .NearClip = 1.0f, .FarClip = 50.0f, .Primary = true });
			camera.AddComponent<PixelPerfectCameraComponent>(PixelPerfectCameraComponent{ .ReferenceWidth = 200, .ReferenceHeight = 120, .PixelsPerUnit = 16.0f });
			TransformSystem::Update(scene);
			const glm::uvec2 reference(200, 120);

			// The reference image's centre looks down -Z from the snapped view's near plane.
			std::optional<UiPointerRay> ray = ComputeUiPointerRay(scene, glm::vec2(100.0f, 60.0f), reference);
			REQUIRE(ray.has_value());
			CHECK(ray->Origin == glm::vec3(0.0f, 0.0f, 9.0f));
			CHECK(ray->Direction == glm::vec3(0.0f, 0.0f, -1.0f));
			CHECK(ray->MinDistance == 0.0f);
			CHECK(ray->MaxDistance == 49.0f);
			// Its top-right corner: the view's half width and half height, along the same direction.
			ray = ComputeUiPointerRay(scene, glm::vec2(200.0f, 0.0f), reference);
			REQUIRE(ray.has_value());
			CHECK(ray->Origin.x == doctest::Approx(6.25f));
			CHECK(ray->Origin.y == doctest::Approx(3.75f));
			CHECK(ray->Origin.z == 9.0f);
			CHECK(ray->Direction == glm::vec3(0.0f, 0.0f, -1.0f));
			// A scene pixel is 1 / 16 of a world unit: the pixel one to the right moves the ray by that much.
			const std::optional<UiPointerRay> next = ComputeUiPointerRay(scene, glm::vec2(101.0f, 60.0f), reference);
			REQUIRE(next.has_value());
			CHECK(next->Origin.x == doctest::Approx(1.0f / 16.0f));
		}

		TEST_CASE("UiInteraction: the pointer hits a World canvas through its ray, under the Screen canvases and nearest first")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// Two World canvases of 100 x 100 canvas pixels at 10 pixels per unit, facing +Z: the far one at z = 0, the near one
			// at z = 2, each a full-size button. A ray from (0, 0, 10) down -Z meets the near one at distance 8.
			const UiCanvasComponent world{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 100, .WorldHeight = 100, .PixelsPerUnit = 10.0f };
			Entity far = scene.CreateEntity("Far");
			far.AddComponent<UiCanvasComponent>(world);
			Entity farButton = AddInteractionButton(scene, "Button", far, glm::vec2(0.0f), glm::vec2(100.0f));
			Entity near = scene.CreateEntity("Near");
			near.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 2.0f);
			near.AddComponent<UiCanvasComponent>(world);
			Entity nearButton = AddInteractionButton(scene, "Button", near, glm::vec2(25.0f), glm::vec2(50.0f));
			TransformSystem::Update(scene);
			UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(100, 100));
			const UiPointerRay down{ .Origin = glm::vec3(0.0f, 0.0f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, -1.0f) };

			// The near canvas's button is first along the ray, though the far canvas is later in draw order.
			const UiCanvasLayout* nearLayout = FindUiCanvasLayout(table, near.GetUUID());
			REQUIRE(nearLayout != nullptr);
			const std::optional<UiCanvasPointer> mapped = MapUiPointerToCanvas(*nearLayout, glm::vec2(-1.0f), down);
			REQUIRE(mapped.has_value());
			CHECK(mapped->Point == glm::vec2(50.0f));
			CHECK(mapped->Distance == 8.0f);
			UiPointerHit hit = HitTestUiPointer(scene, table, glm::vec2(-1.0f), down);
			CHECK(hit.Hit.Target == nearButton.GetUUID());
			CHECK(hit.Canvas == near.GetUUID());
			CHECK(hit.CanvasPoint == glm::vec2(50.0f));
			// Outside the near button (canvas pixel (10, 10), world (-4, 4)), the ray reaches the far canvas's.
			const UiPointerRay corner{ .Origin = glm::vec3(-4.0f, 4.0f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, -1.0f) };
			hit = HitTestUiPointer(scene, table, glm::vec2(-1.0f), corner);
			CHECK(hit.Hit.Target == farButton.GetUUID());
			CHECK(hit.CanvasPoint == glm::vec2(10.0f));
			// A ray away from the canvases, parallel to them or without one hits nothing.
			CHECK_FALSE(HitTestUiPointer(scene, table, glm::vec2(-1.0f), UiPointerRay{ .Origin = glm::vec3(0.0f, 0.0f, 20.0f), .Direction = glm::vec3(0.0f, 0.0f, 1.0f) }).Hit.Target.IsValid());
			CHECK_FALSE(HitTestUiPointer(scene, table, glm::vec2(-1.0f), UiPointerRay{ .Origin = glm::vec3(0.0f, 0.0f, 5.0f), .Direction = glm::vec3(1.0f, 0.0f, 0.0f) }).Hit.Target.IsValid());
			CHECK_FALSE(HitTestUiPointer(scene, table, glm::vec2(50.0f), std::nullopt).Hit.Target.IsValid());

			// A Screen canvas's target under the pointer wins over every World canvas.
			Entity screen = AddInteractionCanvas(scene);
			Entity screenButton = AddInteractionButton(scene, "Overlay", screen, glm::vec2(0.0f), glm::vec2(20.0f));
			table = ComputeUiLayout(scene, glm::uvec2(100, 100));
			hit = HitTestUiPointer(scene, table, glm::vec2(10.0f), down);
			CHECK(hit.Hit.Target == screenButton.GetUUID());
			CHECK(hit.Canvas == screen.GetUUID());
			CHECK(HitTestUi(scene, table, glm::vec2(50.0f)).Target.IsValid() == false);
		}

		TEST_CASE("UiInteraction: a World canvas takes the pointer only inside its rectangle")
		{
			// A World canvas of 100 x 100 canvas pixels at 10 pixels per unit at the origin, facing +Z, holding a button that
			// starts at its middle and reaches 50 pixels beyond its right edge: the canvas is drawn only inside its rectangle
			// (UiPass.h), so the pointer reaches the button there only, while MapUiPointerToCanvas maps any point of the plane.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity board = scene.CreateEntity("Board");
			board.AddComponent<UiCanvasComponent>(
				UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 100, .WorldHeight = 100, .PixelsPerUnit = 10.0f });
			Entity button = AddInteractionButton(scene, "Button", board, glm::vec2(50.0f, 0.0f), glm::vec2(100.0f));
			TransformSystem::Update(scene);
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(100, 100));
			const auto down = [](float x)
			{
				return UiPointerRay{ .Origin = glm::vec3(x, 0.0f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, -1.0f) };
			};
			// World x 2.5 is canvas pixel (75, 50), inside both; world x 7.5 is (125, 50), on the button beyond the canvas.
			const UiPointerHit inside = HitTestUiPointer(scene, table, glm::vec2(-1.0f), down(2.5f));
			CHECK(inside.Hit.Target == button.GetUUID());
			CHECK(inside.CanvasPoint == glm::vec2(75.0f, 50.0f));
			CHECK_FALSE(HitTestUiPointer(scene, table, glm::vec2(-1.0f), down(7.5f)).Hit.Target.IsValid());
			const UiCanvasLayout* layout = FindUiCanvasLayout(table, board.GetUUID());
			REQUIRE(layout != nullptr);
			const std::optional<UiCanvasPointer> beyond = MapUiPointerToCanvas(*layout, glm::vec2(-1.0f), down(7.5f));
			REQUIRE(beyond.has_value());
			CHECK(beyond->Point == glm::vec2(125.0f, 50.0f));
			CHECK(beyond->Distance == 10.0f);
			// A Screen canvas's pointer is the point itself, with or without a ray.
			Entity hud = AddInteractionCanvas(scene);
			const UiLayoutTable withHud = ComputeUiLayout(scene, glm::uvec2(100, 100));
			const UiCanvasLayout* screen = FindUiCanvasLayout(withHud, hud.GetUUID());
			REQUIRE(screen != nullptr);
			CHECK(MapUiPointerToCanvas(*screen, glm::vec2(12.0f, 34.0f), std::nullopt) == UiCanvasPointer{ .Point = glm::vec2(12.0f, 34.0f) });
		}

		// W22.3's review (decision 50): the pointer reaches a World canvas only where the camera draws it, between the ray's
		// MinDistance and MaxDistance: beyond a perspective camera's near plane (its far plane is at infinity), and between
		// an orthographic camera's near and far planes.
		TEST_CASE("UiInteraction: a World canvas takes the pointer only within the camera's clip range")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A perspective camera at (0, 0, 10) looking down -Z with NearClip 1 and FarClip 5, a 90 degree field of view on a
			// 200 x 100 image; a World canvas of 100 x 100 canvas pixels at 10 pixels per unit, all one button, facing it.
			Entity camera = scene.CreateEntity("Camera");
			camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 10.0f);
			camera.AddComponent<CameraComponent>(CameraComponent{ .VerticalFov = 90.0f, .NearClip = 1.0f, .FarClip = 5.0f, .Primary = true });
			Entity board = scene.CreateEntity("Board");
			board.AddComponent<UiCanvasComponent>(
				UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 100, .WorldHeight = 100, .PixelsPerUnit = 10.0f });
			Entity button = AddInteractionButton(scene, "Button", board, glm::vec2(0.0f), glm::vec2(100.0f));
			const glm::uvec2 image(200, 100);
			const glm::vec2 centre(100.0f, 50.0f);
			const auto hitAt = [&scene, &board, image, centre](float z)
			{
				board.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, z);
				TransformSystem::Update(scene);
				const UiLayoutTable table = ComputeUiLayout(scene, image);
				return HitTestUiPointer(scene, table, centre, ComputeUiPointerRay(scene, centre, image)).Hit.Target;
			};

			// The perspective ray's range: NearClip along -Z is NearClip / c along a ray at the angle whose cosine is c (the
			// top-right corner's ray runs along (2, 1, -1), c = 1 / sqrt(6)), and no end.
			std::optional<UiPointerRay> ray = ComputeUiPointerRay(scene, centre, image);
			REQUIRE(ray.has_value());
			CHECK(ray->MinDistance == 1.0f);
			CHECK(ray->MaxDistance == std::numeric_limits<float>::infinity());
			ray = ComputeUiPointerRay(scene, glm::vec2(200.0f, 0.0f), image);
			REQUIRE(ray.has_value());
			CHECK(ray->MinDistance == doctest::Approx(2.4494897f));
			// A canvas 0.5 in front of the camera lies before its near plane, which clips it: no hit. At 2 and at 20 (beyond
			// FarClip, which a perspective projection does not clip at) the camera draws it, and the pointer takes it.
			CHECK_FALSE(hitAt(9.5f).IsValid());
			CHECK(hitAt(8.0f) == button.GetUUID());
			CHECK(hitAt(-10.0f) == button.GetUUID());

			// An orthographic camera clips at both planes: its rays start on the near plane (z 9) and end on the far plane (z 5),
			// so a canvas at 5.5 takes the pointer and one at 4.5 does not.
			camera.GetComponent<CameraComponent>().Projection = ProjectionType::Orthographic;
			camera.GetComponent<CameraComponent>().OrthographicSize = 5.0f;
			ray = ComputeUiPointerRay(scene, centre, image);
			REQUIRE(ray.has_value());
			CHECK(ray->MinDistance == 0.0f);
			CHECK(ray->MaxDistance == 4.0f);
			CHECK_FALSE(hitAt(9.5f).IsValid());
			CHECK(hitAt(8.0f) == button.GetUUID());
			CHECK(hitAt(5.5f) == button.GetUUID());
			CHECK_FALSE(hitAt(4.5f).IsValid());
			CHECK_FALSE(hitAt(-10.0f).IsValid());

			// MapUiPointerToCanvas applies a hand-made ray's range alike, and a farther canvas within it still takes the pointer
			// that a nearer, clipped one leaves.
			board.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f);
			TransformSystem::Update(scene);
			const UiLayoutTable table = ComputeUiLayout(scene, image);
			const UiCanvasLayout* layout = FindUiCanvasLayout(table, board.GetUUID());
			REQUIRE(layout != nullptr);
			const UiPointerRay down{ .Origin = glm::vec3(0.0f, 0.0f, 10.0f), .Direction = glm::vec3(0.0f, 0.0f, -1.0f) };
			CHECK(MapUiPointerToCanvas(*layout, centre, down).has_value());
			CHECK_FALSE(MapUiPointerToCanvas(*layout, centre, UiPointerRay{ .Origin = down.Origin, .Direction = down.Direction, .MinDistance = 10.5f }).has_value());
			CHECK_FALSE(MapUiPointerToCanvas(*layout, centre, UiPointerRay{ .Origin = down.Origin, .Direction = down.Direction, .MaxDistance = 9.5f }).has_value());
			Entity near = scene.CreateEntity("Near");
			near.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 9.5f);
			near.AddComponent<UiCanvasComponent>(
				UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 100, .WorldHeight = 100, .PixelsPerUnit = 10.0f });
			static_cast<void>(AddInteractionButton(scene, "Blocked", near, glm::vec2(0.0f), glm::vec2(100.0f)));
			TransformSystem::Update(scene);
			const UiLayoutTable both = ComputeUiLayout(scene, image);
			CHECK(HitTestUiPointer(scene, both, centre, UiPointerRay{ .Origin = down.Origin, .Direction = down.Direction, .MinDistance = 1.0f }).Hit.Target
				== button.GetUUID());
		}

		TEST_CASE("UiInteraction: directional navigation stays within a render space, and Next and Previous span every canvas")
		{
			// A Screen canvas holding "Top" at y 0 and "Bottom" at y 100, then a World canvas holding "Near" at y 50 and "Far"
			// at y 200 of its own pixels, all focusable buttons. Down from Top reaches Bottom, never the World canvas's Near
			// (its y is another canvas's pixels); Down from Near reaches Far; Next and Previous walk every canvas in draw order.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity hud = AddInteractionCanvas(scene);
			Entity top = AddInteractionButton(scene, "Top", hud, glm::vec2(0.0f), glm::vec2(10.0f));
			Entity bottom = AddInteractionButton(scene, "Bottom", hud, glm::vec2(0.0f, 100.0f), glm::vec2(10.0f));
			Entity board = scene.CreateEntity("Board");
			board.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 300, .WorldHeight = 300 });
			Entity near = AddInteractionButton(scene, "Near", board, glm::vec2(0.0f, 50.0f), glm::vec2(10.0f));
			Entity far = AddInteractionButton(scene, "Far", board, glm::vec2(0.0f, 200.0f), glm::vec2(10.0f));
			TransformSystem::Update(scene);
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 200));

			CHECK(FindUiNavigationTarget(scene, table, top.GetUUID(), UiDirection::Down) == bottom.GetUUID());
			CHECK_FALSE(FindUiNavigationTarget(scene, table, bottom.GetUUID(), UiDirection::Down).IsValid());
			CHECK(FindUiNavigationTarget(scene, table, near.GetUUID(), UiDirection::Down) == far.GetUUID());
			CHECK_FALSE(FindUiNavigationTarget(scene, table, near.GetUUID(), UiDirection::Up).IsValid());
			CHECK(FindNextUiFocus(scene, table, bottom.GetUUID(), true) == near.GetUUID());
			CHECK(FindNextUiFocus(scene, table, far.GetUUID(), true) == top.GetUUID());
			CHECK(FindNextUiFocus(scene, table, near.GetUUID(), false) == bottom.GetUUID());
		}
	}

}
