#include "TestsPCH.h"

#include "Engine/Scene/UiSystem.h"

#include "Engine/Core/Hash.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The play session's UI (M22, C22.1; Docs/Decisions/0033-m22-contract.md decision 3): the step's and the frame's layouts and
// which one is current (ADR 0028's phase-2 ledger: UiInput hit-tests the later of the two). C22.2 (decisions 23, 24, 28 and
// 29): the UiInput phase's state machine, Ui.SetFocus and the ui state-hash block (stream A); the cases that activate, step
// or scroll widgets also run streams B's and C's widget behaviours. C22.3 (decisions 47 and 50): the text input's step and a
// World canvas's pointer through the camera.

namespace Engine {

	namespace {

		UiRectComponent MakeUiSystemRect(glm::vec2 position, glm::vec2 size)
		{
			return UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = position, .Size = size };
		}

		Entity AddUiSystemElement(Scene& scene, std::string_view name, Entity parent, glm::vec2 position, glm::vec2 size)
		{
			Entity entity = scene.CreateEntity(name, parent);
			entity.AddComponent<UiRectComponent>(MakeUiSystemRect(position, size));
			entity.AddComponent<UiImageComponent>(UiImageComponent{});
			return entity;
		}

		UiEvent MakeUiEvent(UiEventKind kind, Entity target, bool flag = false)
		{
			return UiEvent{ .Kind = kind, .Target = target.GetUUID(), .Flag = flag };
		}

		// One UiInput with the pointer at `pointer` and the given button edges, returning the events it queued.
		std::vector<UiEvent> ProcessPointer(UiSystem& ui, Scene& scene, glm::vec2 pointer, bool down, bool pressed, bool released)
		{
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = pointer, .PointerDown = down, .PointerPressed = pressed, .PointerReleased = released });
			return ui.TakeEvents();
		}

		// One UiInput with the pointer away from the UI and `navigation`, returning the events it queued.
		std::vector<UiEvent> ProcessNavigation(UiSystem& ui, Scene& scene, UiNavigation navigation, glm::vec2 stick = glm::vec2(0.0f))
		{
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .Navigation = navigation, .Stick = stick });
			return ui.TakeEvents();
		}

		// A vertical list on its own canvas: the scroll view "List" at (0, 0), 100 x 100 scene pixels, whose Content holds ten
		// focusable buttons "Item0" to "Item9", each 100 x 40 at y = 40 i in the content (400 tall, so it scrolls up to 300),
		// with a ScrollStep of 40. Returns the list; the items are its content's children in order.
		Entity AddUiSystemList(Scene& scene)
		{
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity list = AddUiSystemElement(scene, "List", canvas, glm::vec2(0.0f), glm::vec2(100.0f));
			Entity content = scene.CreateEntity("Content", list);
			content.AddComponent<UiRectComponent>(MakeUiSystemRect(glm::vec2(0.0f), glm::vec2(100.0f, 400.0f)));
			list.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = content.GetUUID(), .ScrollStep = 40.0f });
			for (int index = 0; index < 10; ++index)
			{
				Entity item = AddUiSystemElement(scene, std::format("Item{}", index), content, glm::vec2(0.0f, 40.0f * static_cast<float>(index)),
					glm::vec2(100.0f, 40.0f));
				item.AddComponent<UiButtonComponent>(UiButtonComponent{});
			}
			return list;
		}

		// The list's item `index` (AddUiSystemList).
		Entity FindUiSystemItem(Scene& scene, int index)
		{
			return scene.FindEntityByPath(std::format("/Canvas/List/Content/Item{}", index));
		}

		glm::vec2 GetUiSystemScroll(Entity list)
		{
			return list.GetComponent<UiScrollViewComponent>().ScrollPosition;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("UiSystem: the current layout is the frame's after a FrameUiLayout and the step's after a UiLayout")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity button = scene.CreateEntity("Button", canvas);
			button.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Size = glm::vec2(10.0f) });

			UiSystem ui;
			CHECK(ui.GetLayout().Nodes.empty());
			ui.LayoutStep(scene, glm::uvec2(100, 50));
			CHECK_FALSE(ui.IsFrameLayoutCurrent());
			CHECK(&ui.GetLayout() == &ui.GetStepLayout());
			CHECK(ui.GetStepLayout().SceneSize == glm::uvec2(100, 50));
			REQUIRE(ui.GetStepLayout().Nodes.size() == 1);
			CHECK(ui.GetStepLayout().Nodes[0].Min == glm::vec2(0.0f));

			// OnUpdate moves the button: the frame's layout follows it and becomes current; the step's keeps the step's state.
			button.GetComponent<UiRectComponent>().Position = glm::vec2(5.0f, 0.0f);
			ui.LayoutFrame(scene, glm::uvec2(100, 50));
			CHECK(ui.IsFrameLayoutCurrent());
			CHECK(&ui.GetLayout() == &ui.GetFrameLayout());
			REQUIRE(ui.GetFrameLayout().Nodes.size() == 1);
			CHECK(ui.GetFrameLayout().Nodes[0].Min == glm::vec2(5.0f, 0.0f));
			CHECK(ui.GetStepLayout().Nodes[0].Min == glm::vec2(0.0f));

			// The next step's layout is current again.
			ui.LayoutStep(scene, glm::uvec2(100, 50));
			CHECK_FALSE(ui.IsFrameLayoutCurrent());
			CHECK(&ui.GetLayout() == &ui.GetStepLayout());
			CHECK(ui.GetStepLayout().Nodes[0].Min == glm::vec2(5.0f, 0.0f));
		}

		TEST_CASE("UiSystem: presentation texts stay out of the state hash and go with their element, its UiText or the scene")
		{
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 5).
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity fps = scene.CreateEntity("Fps", canvas);
			fps.AddComponent<UiRectComponent>(UiRectComponent{});
			fps.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "--" });
			Entity count = scene.CreateEntity("Count", canvas);
			count.AddComponent<UiRectComponent>(UiRectComponent{});
			count.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "0" });

			UiSystem ui;
			const auto digest = [&ui]()
			{
				XXH64Hasher hasher(0);
				ui.AppendStateHash(hasher);
				return hasher.Digest();
			};
			const uint64_t before = digest();
			ui.SetPresentationText(fps.GetUUID(), std::string("16.7 ms"));
			ui.SetPresentationText(count.GetUUID(), std::string("42"));
			CHECK(digest() == before);
			REQUIRE(ui.FindPresentationText(fps.GetUUID()) != nullptr);
			CHECK(*ui.FindPresentationText(fps.GetUUID()) == "16.7 ms");
			ui.SetPresentationText(fps.GetUUID(), std::string("33.3 ms"));
			CHECK(*ui.FindPresentationText(fps.GetUUID()) == "33.3 ms");
			ui.SetPresentationText(fps.GetUUID(), std::nullopt);
			CHECK(ui.FindPresentationText(fps.GetUUID()) == nullptr);

			// The frame's layout drops a text whose element lost its UiText or is gone.
			ui.SetPresentationText(fps.GetUUID(), std::string("60 fps"));
			count.RemoveComponent<UiTextComponent>();
			ui.LayoutFrame(scene, glm::uvec2(100, 50));
			CHECK(ui.FindPresentationText(count.GetUUID()) == nullptr);
			REQUIRE(ui.FindPresentationText(fps.GetUUID()) != nullptr);
			scene.DestroyEntity(fps);
			ui.LayoutFrame(scene, glm::uvec2(100, 50));
			CHECK(ui.GetPresentationTexts().empty());

			// A scene replacement clears them.
			Entity again = scene.CreateEntity("Again", canvas);
			again.AddComponent<UiRectComponent>(UiRectComponent{});
			again.AddComponent<UiTextComponent>(UiTextComponent{});
			ui.SetPresentationText(again.GetUUID(), std::string("x"));
			ui.ResetInteraction();
			CHECK(ui.GetPresentationTexts().empty());
		}

		// Decision 24: hover follows the pointer, a press focuses and presses the widget under it, and a release over the
		// pressed button clicks it; a release elsewhere clicks nothing.
		TEST_CASE("UiSystem: hover follows the pointer, and a press and release over a button clicks it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity play = AddUiSystemElement(scene, "Play", canvas, glm::vec2(10.0f, 10.0f), glm::vec2(60.0f, 30.0f));
			play.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity quit = AddUiSystemElement(scene, "Quit", canvas, glm::vec2(10.0f, 50.0f), glm::vec2(60.0f, 30.0f));
			quit.AddComponent<UiButtonComponent>(UiButtonComponent{});
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 100));

			CHECK(ProcessPointer(ui, scene, glm::vec2(20.0f), false, false, false) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, play, true) });
			CHECK(ui.GetInteraction().Hovered == play.GetUUID());
			CHECK(ui.GetInteraction().PointerOverUi);
			CHECK(ProcessPointer(ui, scene, glm::vec2(20.0f), true, true, false) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, play, true) });
			CHECK(ui.GetInteraction().Pressed == play.GetUUID());
			CHECK(ui.GetInteraction().Focused == play.GetUUID());
			CHECK(ProcessPointer(ui, scene, glm::vec2(20.0f), false, false, true) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Click, play) });
			CHECK_FALSE(ui.GetInteraction().Pressed.IsValid());

			CHECK(ProcessPointer(ui, scene, glm::vec2(20.0f, 60.0f), false, false, false)
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, play, false), MakeUiEvent(UiEventKind::Hover, quit, true) });
			CHECK(ProcessPointer(ui, scene, glm::vec2(20.0f, 60.0f), true, true, false)
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, play, false), MakeUiEvent(UiEventKind::Focus, quit, true) });
			// Dragged off while held: the hover leaves, and the release there clicks nothing.
			CHECK(ProcessPointer(ui, scene, glm::vec2(150.0f, 90.0f), true, false, false) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, quit, false) });
			CHECK(ui.GetInteraction().Pressed == quit.GetUUID());
			CHECK_FALSE(ui.GetInteraction().PointerOverUi);
			CHECK(ProcessPointer(ui, scene, glm::vec2(150.0f, 90.0f), false, false, true).empty());
			CHECK_FALSE(ui.GetInteraction().Pressed.IsValid());
			CHECK(ui.GetInteraction().Focused == quit.GetUUID());
		}

		// Roadmap M22's acceptance "every widget works by keyboard and gamepad" (decisions 23, 24 and 26): Next focuses the
		// canvas's DefaultFocus, the button, and Submit clicks it; Down reaches the toggle, which Submit flips; the slider
		// steps with the arrows and the stick's edges; the focusable scroll view scrolls with its axis's directions; Cancel
		// goes to the focused element.
		TEST_CASE("UiSystem: every widget works by keyboard and gamepad")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity button = AddUiSystemElement(scene, "Button", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f)); // centre (50, 10)
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = button.GetUUID();
			Entity toggle = AddUiSystemElement(scene, "Toggle", canvas, glm::vec2(40.0f, 40.0f), glm::vec2(20.0f)); // (50, 50)
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{});
			Entity slider = AddUiSystemElement(scene, "Slider", canvas, glm::vec2(0.0f, 80.0f), glm::vec2(100.0f, 20.0f)); // (50, 90)
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Max = 10.0f, .WholeNumbers = true });
			Entity scroll = AddUiSystemElement(scene, "Scroll", canvas, glm::vec2(0.0f, 120.0f), glm::vec2(100.0f, 40.0f)); // (50, 140)
			Entity content = scene.CreateEntity("Content", scroll);
			content.AddComponent<UiRectComponent>(MakeUiSystemRect(glm::vec2(0.0f), glm::vec2(100.0f, 200.0f)));
			scroll.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Focusable = true, .Content = content.GetUUID() });
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 200));

			CHECK(ProcessNavigation(ui, scene, UiNavigation::Next) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, button, true) });
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Submit) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Click, button) });
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Down)
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, button, false), MakeUiEvent(UiEventKind::Focus, toggle, true) });
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Submit) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::ValueChanged, toggle, true) });
			CHECK(toggle.GetComponent<UiToggleComponent>().IsOn);
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Down).size() == 2);
			CHECK(ui.GetInteraction().Focused == slider.GetUUID());
			// The slider takes Right as a step of a tenth of its range, at least 1 with whole numbers.
			std::vector<UiEvent> events = ProcessNavigation(ui, scene, UiNavigation::Right);
			REQUIRE(events.size() == 1);
			CHECK(events[0].Kind == UiEventKind::ValueChanged);
			CHECK(events[0].HasNumber);
			CHECK(events[0].Number == 1.0f);
			// The stick steps once per edge: held right is one press, released and pushed again another.
			CHECK(ProcessNavigation(ui, scene, UiNavigation::None, glm::vec2(1.0f, 0.0f)).size() == 1);
			CHECK(ProcessNavigation(ui, scene, UiNavigation::None, glm::vec2(1.0f, 0.0f)).empty());
			CHECK(ui.GetInteraction().Stick == UiStickDirection::Right);
			CHECK(ProcessNavigation(ui, scene, UiNavigation::None, glm::vec2(0.0f)).empty());
			CHECK(ProcessNavigation(ui, scene, UiNavigation::None, glm::vec2(0.9f, 0.1f)).size() == 1);
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 3.0f);
			// Down leaves the slider (not its axis) for the scroll view, which scrolls along y by its step.
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Down).size() == 2);
			CHECK(ui.GetInteraction().Focused == scroll.GetUUID());
			events = ProcessNavigation(ui, scene, UiNavigation::Down);
			REQUIRE(events.size() == 1);
			CHECK(events[0].Kind == UiEventKind::Scroll);
			CHECK(events[0].Vector == glm::vec2(0.0f, 40.0f));
			CHECK(scroll.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 40.0f));
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Cancel) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Cancel, scroll) });
		}

		// Decision 24 step 1: state for an element that can no longer take it is cleared, and an element the scene still holds
		// hears that it lost focus.
		TEST_CASE("UiSystem: interaction state for an element that can no longer take it is cleared")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity button = AddUiSystemElement(scene, "Button", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = button.GetUUID();
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 100));
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Next).size() == 1);
			REQUIRE(ui.GetInteraction().Focused == button.GetUUID());

			button.GetComponent<UiButtonComponent>().Interactable = false;
			ui.LayoutStep(scene, glm::uvec2(200, 100));
			CHECK(ProcessNavigation(ui, scene, UiNavigation::None) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, button, false) });
			CHECK(ui.GetInteraction().IsEmpty());
		}

		// Decision 23 (the lead's decision on the C22.2 review): with nothing focused, the navigation commands do nothing
		// until a canvas names its DefaultFocus, which the first of them then focuses, whatever its direction; Submit acts
		// only on the focused widget; and a pointer press focuses only a focusable widget, so a HUD button that is not
		// Focusable is clicked without taking keyboard focus, and a game whose play uses the arrows and Space keeps them.
		TEST_CASE("UiSystem: with nothing focused, navigation focuses only a canvas's DefaultFocus, and a press focuses only a focusable widget")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity hud = AddUiSystemElement(scene, "Hud", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			hud.AddComponent<UiButtonComponent>(UiButtonComponent{ .Focusable = false });
			Entity menu = AddUiSystemElement(scene, "Menu", canvas, glm::vec2(0.0f, 40.0f), glm::vec2(100.0f, 20.0f));
			menu.AddComponent<UiButtonComponent>(UiButtonComponent{});
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 100));

			// No DefaultFocus: the directions, Next, Previous, the stick and Submit do nothing.
			for (const UiNavigation navigation : { UiNavigation::Down, UiNavigation::Right, UiNavigation::Next, UiNavigation::Previous, UiNavigation::Submit })
				CHECK(ProcessNavigation(ui, scene, navigation).empty());
			CHECK(ProcessNavigation(ui, scene, UiNavigation::None, glm::vec2(0.0f, -1.0f)).empty());
			CHECK(ProcessNavigation(ui, scene, UiNavigation::None).empty());
			CHECK_FALSE(ui.GetInteraction().Focused.IsValid());

			// A click on the HUD button clicks it without focusing it, so Submit still does nothing.
			CHECK(ProcessPointer(ui, scene, glm::vec2(10.0f), true, true, false) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, hud, true) });
			CHECK(ProcessPointer(ui, scene, glm::vec2(10.0f), false, false, true) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Click, hud) });
			CHECK_FALSE(ui.GetInteraction().Focused.IsValid());
			CHECK(ProcessPointer(ui, scene, glm::vec2(150.0f, 90.0f), false, false, false) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, hud, false) });
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Submit).empty());

			// A DefaultFocus that is not focusable is passed over; a focusable one takes focus at the first command.
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = hud.GetUUID();
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Down).empty());
			canvas.GetComponent<UiCanvasComponent>().DefaultFocus = menu.GetUUID();
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Right) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, menu, true) });
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Submit) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Click, menu) });
			// From the focused widget navigation moves as usual, never to the button that is not focusable.
			CHECK(ProcessNavigation(ui, scene, UiNavigation::Up).empty());
			CHECK(ui.GetInteraction().Focused == menu.GetUUID());
		}

		// Decision 24's two rules stream A chose (accepted at W22.2's integration): the step view's edges carry no order, so a
		// press and a release of one step that the pointer no longer holds are a tap, which clicks, while a release edge never
		// releases a press of its own step that the pointer still holds; and an element the scene no longer holds, or holds
		// pending destruction, hears no Focus or Hover (false) when the state drops it.
		TEST_CASE("UiSystem: a tap within a step clicks, a held press outlives its step's release edge, and a destroyed element hears nothing")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity play = AddUiSystemElement(scene, "Play", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			play.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity quit = AddUiSystemElement(scene, "Quit", canvas, glm::vec2(0.0f, 30.0f), glm::vec2(100.0f, 20.0f));
			quit.AddComponent<UiButtonComponent>(UiButtonComponent{});
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 100));

			// Down and up within one step, the pointer up at its end: the hover, the focus and the click.
			CHECK(ProcessPointer(ui, scene, glm::vec2(10.0f), false, true, true)
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, play, true), MakeUiEvent(UiEventKind::Focus, play, true),
					MakeUiEvent(UiEventKind::Click, play) });
			CHECK_FALSE(ui.GetInteraction().Pressed.IsValid());

			// Held, then up and down again within one step, the pointer down at its end: the step's press stands, nothing is
			// clicked until the release.
			CHECK(ProcessPointer(ui, scene, glm::vec2(10.0f), true, true, false).empty());
			CHECK(ProcessPointer(ui, scene, glm::vec2(10.0f), true, true, true).empty());
			CHECK(ui.GetInteraction().Pressed == play.GetUUID());
			CHECK(ProcessPointer(ui, scene, glm::vec2(10.0f), false, false, true) == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Click, play) });

			// Quit focused and hovered, then destroyed (pending until the flush): it leaves the layout, and the state drops it
			// without telling it.
			REQUIRE(ui.SetFocus(scene, quit.GetUUID()).has_value());
			static_cast<void>(ui.TakeEvents());
			CHECK(ProcessPointer(ui, scene, glm::vec2(10.0f, 40.0f), false, false, false)
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, play, false), MakeUiEvent(UiEventKind::Hover, quit, true) });
			REQUIRE(ui.GetInteraction().Focused == quit.GetUUID());
			const UUID quitId = quit.GetUUID();
			scene.DestroyEntity(quit);
			REQUIRE(scene.FindReadableEntityByID(quitId).IsReadable());
			ui.LayoutStep(scene, glm::uvec2(200, 100));
			CHECK(ProcessPointer(ui, scene, glm::vec2(10.0f, 40.0f), false, false, false).empty());
			CHECK(ui.GetInteraction().IsEmpty());
		}

		// Decision 24 steps 3 to 6: a press on a slider sets its value at the pointer and drags it; a press on a scroll view's
		// content drags the content with the pointer; the wheel scrolls the innermost scroll view under the pointer (y
		// up-positive: a step down scrolls down by ScrollStep). Neither widget is focusable here, so once released and left
		// the state is empty again: DragOrigin and DragStart return to zero with Dragged.
		TEST_CASE("UiSystem: the pointer drags a slider and a scroll view, and the wheel scrolls the scroll view under it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity slider = AddUiSystemElement(scene, "Slider", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Focusable = false, .Max = 10.0f });
			Entity scroll = AddUiSystemElement(scene, "Scroll", canvas, glm::vec2(0.0f, 40.0f), glm::vec2(100.0f, 40.0f));
			// The content is an image, so the pointer over it hits it and reaches the scroll view, its nearest widget.
			Entity content = AddUiSystemElement(scene, "Content", scroll, glm::vec2(0.0f), glm::vec2(100.0f, 200.0f));
			scroll.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = content.GetUUID() });
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 200));
			const auto scrolled = [&scroll](glm::vec2 position)
			{
				return UiEvent{ .Kind = UiEventKind::Scroll, .Target = scroll.GetUUID(), .Vector = position };
			};
			const auto valued = [&slider](float value)
			{
				return UiEvent{ .Kind = UiEventKind::ValueChanged, .Target = slider.GetUUID(), .HasNumber = true, .Number = value };
			};

			// The wheel over the content: a step down scrolls down by 40, a step up back to 0.
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(50.0f, 60.0f), .Scroll = glm::vec2(0.0f, -1.0f) });
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, scroll, true), scrolled(glm::vec2(0.0f, 40.0f)) });
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(50.0f, 60.0f), .Scroll = glm::vec2(0.0f, 1.0f) });
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ scrolled(glm::vec2(0.0f)) });
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(50.0f, 60.0f), .Scroll = glm::vec2(0.0f, -1.0f) });
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ scrolled(glm::vec2(0.0f, 40.0f)) });

			// A press on the content drags the scroll view: from 40, the pointer 20 pixels up moves the content up, to 60.
			CHECK(ProcessPointer(ui, scene, glm::vec2(50.0f, 70.0f), true, true, false).empty());
			CHECK(ui.GetInteraction().Pressed == scroll.GetUUID());
			CHECK(ui.GetInteraction().Dragged == scroll.GetUUID());
			CHECK(ui.GetInteraction().DragOrigin == glm::vec2(50.0f, 70.0f));
			CHECK(ui.GetInteraction().DragStart == glm::vec2(0.0f, 40.0f));
			CHECK_FALSE(ui.GetInteraction().Focused.IsValid());
			CHECK(ProcessPointer(ui, scene, glm::vec2(50.0f, 50.0f), true, false, false) == std::vector<UiEvent>{ scrolled(glm::vec2(0.0f, 60.0f)) });
			CHECK(scroll.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 60.0f));
			CHECK(ProcessPointer(ui, scene, glm::vec2(50.0f, 50.0f), false, false, true).empty());
			CHECK_FALSE(ui.GetInteraction().Pressed.IsValid());
			CHECK_FALSE(ui.GetInteraction().Dragged.IsValid());
			CHECK(ui.GetInteraction().DragOrigin == glm::vec2(0.0f));
			CHECK(ui.GetInteraction().DragStart == glm::vec2(0.0f));

			// A press on the slider sets the value at the pointer and drags it; past its end it stops at Max, and the release
			// away from it activates nothing.
			CHECK(ProcessPointer(ui, scene, glm::vec2(25.0f, 10.0f), true, true, false)
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, scroll, false), MakeUiEvent(UiEventKind::Hover, slider, true), valued(2.5f) });
			CHECK(ui.GetInteraction().Dragged == slider.GetUUID());
			CHECK(ui.GetInteraction().DragOrigin == glm::vec2(25.0f, 10.0f));
			CHECK(ui.GetInteraction().DragStart == glm::vec2(0.0f));
			CHECK(ProcessPointer(ui, scene, glm::vec2(75.0f, 10.0f), true, false, false) == std::vector<UiEvent>{ valued(7.5f) });
			CHECK(ProcessPointer(ui, scene, glm::vec2(150.0f, 10.0f), true, false, false)
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, slider, false), valued(10.0f) });
			CHECK(ProcessPointer(ui, scene, glm::vec2(150.0f, 10.0f), false, false, true).empty());
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 10.0f);
			CHECK(ui.GetInteraction().IsEmpty());
		}

		// Decisions 21 and 26 (W22.2's integration: the drag start reads the scroll functions' rule): a Content that is not an
		// element below its scroll view leaves nothing to scroll, so a press on the scroll view starts its drag at the
		// position as read, zero, and neither the drag nor the wheel moves it or raises Scroll, whatever ScrollPosition holds.
		TEST_CASE("UiSystem: a scroll view whose Content is not below it starts its drag at zero and scrolls nothing")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity scroll = AddUiSystemElement(scene, "Scroll", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 40.0f));
			// A sibling taller than the view, so it would overflow by 160 if it were the content.
			Entity sibling = AddUiSystemElement(scene, "Sibling", canvas, glm::vec2(0.0f, 100.0f), glm::vec2(100.0f, 200.0f));
			scroll.AddComponent<UiScrollViewComponent>(
				UiScrollViewComponent{ .Content = sibling.GetUUID(), .ScrollPosition = glm::vec2(0.0f, 30.0f) });
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 400));

			CHECK(ProcessPointer(ui, scene, glm::vec2(50.0f, 20.0f), true, true, false)
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, scroll, true) });
			CHECK(ui.GetInteraction().Dragged == scroll.GetUUID());
			CHECK(ui.GetInteraction().DragOrigin == glm::vec2(50.0f, 20.0f));
			CHECK(ui.GetInteraction().DragStart == glm::vec2(0.0f));
			CHECK(ProcessPointer(ui, scene, glm::vec2(50.0f, 0.0f), true, false, false).empty());
			CHECK(ProcessPointer(ui, scene, glm::vec2(50.0f, 0.0f), false, false, true).empty());
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(50.0f, 20.0f), .Scroll = glm::vec2(0.0f, -1.0f) });
			CHECK(ui.TakeEvents().empty());
			CHECK(scroll.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 30.0f));
		}

		// Decision 24 step 1 (the C22.2 review's major): without a canvas UiInput reads no input, so the state and the ui
		// block stay empty whatever the pointer, the wheel, the stick and the bindings do; a scene that loses its last canvas
		// drops its state, telling the widget it still holds that it lost focus and hover.
		TEST_CASE("UiSystem: without a canvas the interaction state stays empty whatever the input")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(scene.CreateEntity("Player"));
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 100));
			const UiInputFrame busy{ .Pointer = glm::vec2(10.0f),
				.PointerDown = true,
				.PointerPressed = true,
				.Scroll = glm::vec2(0.0f, -1.0f),
				.Navigation = UiNavigation::Next | UiNavigation::Down | UiNavigation::Submit | UiNavigation::Cancel,
				.Stick = glm::vec2(0.0f, 0.75f) };
			const XXH64Hasher untouched(0);
			for (int tick = 0; tick < 2; ++tick)
			{
				ui.ProcessInput(scene, busy);
				CHECK(ui.GetQueuedEventCount() == 0);
				CHECK(ui.GetInteraction().IsEmpty());
				XXH64Hasher hasher(0);
				ui.AppendStateHash(hasher);
				CHECK(hasher.Digest() == untouched.Digest());
			}

			// A canvas whose button is focused and hovered with the stick held, then removed.
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity button = AddUiSystemElement(scene, "Button", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			ui.LayoutStep(scene, glm::uvec2(200, 100));
			REQUIRE(ui.SetFocus(scene, button.GetUUID()).has_value());
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(10.0f), .Stick = glm::vec2(0.0f, 0.75f) });
			static_cast<void>(ui.TakeEvents());
			REQUIRE(ui.GetInteraction().Hovered == button.GetUUID());
			REQUIRE(ui.GetInteraction().Stick == UiStickDirection::Up);
			canvas.RemoveComponent<UiCanvasComponent>();
			ui.LayoutStep(scene, glm::uvec2(200, 100));
			ui.ProcessInput(scene, busy);
			const std::vector<UiEvent> events = ui.TakeEvents();
			CHECK(events.size() == 2);
			CHECK(std::ranges::find(events, MakeUiEvent(UiEventKind::Focus, button, false)) != events.end());
			CHECK(std::ranges::find(events, MakeUiEvent(UiEventKind::Hover, button, false)) != events.end());
			CHECK(ui.GetInteraction().IsEmpty());
		}

		// Decision 28: Ui.SetFocus takes a focusable element or null, and refuses any other element without a change.
		TEST_CASE("UiSystem: SetFocus focuses a focusable element and refuses any other")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity play = AddUiSystemElement(scene, "Play", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			play.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity disabled = AddUiSystemElement(scene, "Disabled", canvas, glm::vec2(0.0f, 30.0f), glm::vec2(100.0f, 20.0f));
			disabled.AddComponent<UiButtonComponent>(UiButtonComponent{ .Interactable = false });
			Entity label = AddUiSystemElement(scene, "Label", canvas, glm::vec2(0.0f, 60.0f), glm::vec2(100.0f, 20.0f));
			// A button without an area, and one hidden in an off toggle's Graphic (decision 26).
			Entity flat = AddUiSystemElement(scene, "Flat", canvas, glm::vec2(120.0f, 0.0f), glm::vec2(40.0f, 0.0f));
			flat.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity toggle = AddUiSystemElement(scene, "Toggle", canvas, glm::vec2(120.0f, 30.0f), glm::vec2(40.0f, 20.0f));
			Entity hidden = AddUiSystemElement(scene, "Hidden", toggle, glm::vec2(0.0f), glm::vec2(20.0f));
			hidden.AddComponent<UiButtonComponent>(UiButtonComponent{});
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{ .Graphic = hidden.GetUUID() });
			// A button in a text input's Placeholder, hidden while the input's Text is not empty (decision 48): the refusal names
			// both rules that hide an element (W22.3's review).
			Entity field = AddUiSystemElement(scene, "Field", canvas, glm::vec2(120.0f, 60.0f), glm::vec2(60.0f, 20.0f));
			Entity placeholder = AddUiSystemElement(scene, "Placeholder", field, glm::vec2(0.0f), glm::vec2(60.0f, 20.0f));
			Entity clear = AddUiSystemElement(scene, "Clear", placeholder, glm::vec2(0.0f), glm::vec2(20.0f));
			clear.AddComponent<UiButtonComponent>(UiButtonComponent{});
			field.AddComponent<UiTextInputComponent>(UiTextInputComponent{ .Text = "typed", .Placeholder = placeholder.GetUUID() });
			UiSystem ui;
			// Before the first layout there is no scene size, so nothing is laid out and nothing takes focus.
			const Status unlaid = ui.SetFocus(scene, play.GetUUID());
			REQUIRE_FALSE(unlaid.has_value());
			CHECK(unlaid.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(ui.CheckFocus(scene, play.GetUUID()).has_value());
			CHECK(ui.GetInteraction().IsEmpty());
			ui.LayoutStep(scene, glm::uvec2(200, 100));

			REQUIRE(ui.SetFocus(scene, play.GetUUID()).has_value());
			CHECK(ui.GetInteraction().Focused == play.GetUUID());
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, play, true) });
			for (const Entity refused : { disabled, label, canvas, flat, hidden, clear })
			{
				CAPTURE(refused.GetName());
				const Status status = ui.SetFocus(scene, refused.GetUUID());
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(ui.GetInteraction().Focused == play.GetUUID());
				// CheckFocus, the Ui module's question first, refuses it alike and changes nothing.
				const Result<bool> checked = ui.CheckFocus(scene, refused.GetUUID());
				REQUIRE_FALSE(checked.has_value());
				CHECK(checked.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const Result<bool> same = ui.CheckFocus(scene, play.GetUUID());
			REQUIRE(same.has_value());
			CHECK_FALSE(*same);
			const Result<bool> cleared = ui.CheckFocus(scene, UUID{});
			REQUIRE(cleared.has_value());
			CHECK(*cleared);
			CHECK(ui.GetQueuedEventCount() == 0);
			REQUIRE(ui.SetFocus(scene, UUID{}).has_value());
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, play, false) });
			CHECK(ui.GetInteraction().IsEmpty());
			// With the input's Text empty its Placeholder shows, and the button in it takes focus (laid out at the call).
			field.GetComponent<UiTextInputComponent>().Text.clear();
			REQUIRE(ui.SetFocus(scene, clear.GetUUID()).has_value());
			CHECK(ui.GetInteraction().Focused == clear.GetUUID());
		}

		// Decision 28, amended by the review of W22.2 (the lead's decision 2): Ui.SetFocus checks against the scene as it is at
		// the call, never the last layout, so a pause menu a callback has just shown, or a button it has just created, takes
		// focus at once. The current layout stays the last one made, which UiInput hit-tests (what was drawn).
		TEST_CASE("UiSystem: SetFocus focuses a menu a callback has just shown or created, before any layout")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(AddUiSystemList(scene));
			Entity menu = scene.CreateEntity("Menu");
			menu.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .SortOrder = 1 });
			Entity resume = AddUiSystemElement(scene, "Resume", menu, glm::vec2(150.0f, 0.0f), glm::vec2(40.0f, 20.0f));
			resume.AddComponent<UiButtonComponent>(UiButtonComponent{});
			menu.SetActive(false);
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 200));
			REQUIRE(FindUiLayoutNode(ui.GetLayout(), resume.GetUUID()) == nullptr);

			menu.SetActive(true);
			REQUIRE(ui.SetFocus(scene, resume.GetUUID()).has_value());
			CHECK(ui.GetInteraction().Focused == resume.GetUUID());
			Entity quit = AddUiSystemElement(scene, "Quit", menu, glm::vec2(150.0f, 30.0f), glm::vec2(40.0f, 20.0f));
			quit.AddComponent<UiButtonComponent>(UiButtonComponent{});
			REQUIRE(ui.SetFocus(scene, quit.GetUUID()).has_value());
			CHECK(ui.TakeEvents()
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, resume, true), MakeUiEvent(UiEventKind::Focus, resume, false),
					MakeUiEvent(UiEventKind::Focus, quit, true) });
			CHECK(FindUiLayoutNode(ui.GetLayout(), resume.GetUUID()) == nullptr);
		}

		// Decisions 23 and 28, amended by the review of W22.2 (the lead's decision 2): Ui.SetFocus reveals against the scene as
		// it is at the call, so the scroll position the reveal starts from is the one the layout of that scene used: after a
		// wheel scroll of the same step it scrolls no further than needed, and after a script reset the position it scrolls
		// back to show the item.
		TEST_CASE("UiSystem: SetFocus reveals from the scroll position the scene has at the call")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity list = AddUiSystemList(scene);
			UiSystem ui;

			// The step's layout, then the wheel scrolls the list two steps, to 80, against it; Item2 (content 80 to 120) is then
			// at 0 to 40, in view, so focusing it from the step's events scrolls nothing (from the layout's rectangle, 80 to
			// 120, it would scroll 20 more and clip half of it).
			ui.LayoutStep(scene, glm::uvec2(200, 200));
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(50.0f, 50.0f), .Scroll = glm::vec2(0.0f, -2.0f) });
			static_cast<void>(ui.TakeEvents());
			REQUIRE(GetUiSystemScroll(list) == glm::vec2(0.0f, 80.0f));
			const Entity item2 = FindUiSystemItem(scene, 2);
			REQUIRE(ui.SetFocus(scene, item2.GetUUID()).has_value());
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, item2, true) });
			CHECK(GetUiSystemScroll(list) == glm::vec2(0.0f, 80.0f));

			// Laid out at 80, then reset to 0 by a script: Item6 (content 240 to 280) needs a scroll of 180, the least that
			// shows it (from the layout's rectangle, 160 to 200, the reveal would stop at 100 and leave it hidden).
			list.GetComponent<UiScrollViewComponent>().ScrollPosition = glm::vec2(0.0f, 80.0f);
			ui.LayoutStep(scene, glm::uvec2(200, 200));
			list.GetComponent<UiScrollViewComponent>().ScrollPosition = glm::vec2(0.0f);
			const Entity item6 = FindUiSystemItem(scene, 6);
			REQUIRE(ui.SetFocus(scene, item6.GetUUID()).has_value());
			CHECK(ui.TakeEvents()
				== std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Focus, item2, false), MakeUiEvent(UiEventKind::Focus, item6, true),
					UiEvent{ .Kind = UiEventKind::Scroll, .Target = list.GetUUID(), .Vector = glm::vec2(0.0f, 180.0f) } });
			ui.LayoutStep(scene, glm::uvec2(200, 200));
			const UiLayoutNode* shown = FindUiLayoutNode(ui.GetLayout(), item6.GetUUID());
			REQUIRE(shown != nullptr);
			CHECK(shown->Min.y == 60.0f);
			CHECK(shown->Max.y == 100.0f);
		}

		// Decision 24 step 8 (the review of W22.2): a focus that navigation moves is revealed against the scene as it is then,
		// so a scroll of the same UiInput (the wheel, or the reveal of an earlier command) is where the reveal starts: each
		// reveal scrolls the least that shows its element.
		TEST_CASE("UiSystem: navigation reveals the new focus from the scroll position of its own step")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity list = AddUiSystemList(scene);
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 200));
			REQUIRE(ui.SetFocus(scene, FindUiSystemItem(scene, 0).GetUUID()).has_value());
			static_cast<void>(ui.TakeEvents());

			// The wheel scrolls to 80, which hides Item1 (content 40 to 80, now -40 to 0); Down then focuses it and scrolls
			// back to 40, the least that shows it (from the layout's rectangle, inside the view, it would stay hidden at 80).
			ui.ProcessInput(scene,
				UiInputFrame{ .Pointer = glm::vec2(50.0f, 50.0f), .Scroll = glm::vec2(0.0f, -2.0f), .Navigation = UiNavigation::Down });
			CHECK(ui.GetInteraction().Focused == FindUiSystemItem(scene, 1).GetUUID());
			CHECK(GetUiSystemScroll(list) == glm::vec2(0.0f, 40.0f));
			static_cast<void>(ui.TakeEvents());

			// From Item1 with the list back at 0: Next reveals Item2 (content 80 to 120) by 20, then Down from it reveals Item3
			// (120 to 160) by 40 more, to 60, its bottom on the view's (from the layout's rectangles of the step, Down would
			// scroll on to 80).
			list.GetComponent<UiScrollViewComponent>().ScrollPosition = glm::vec2(0.0f);
			ui.LayoutStep(scene, glm::uvec2(200, 200));
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .Navigation = UiNavigation::Next | UiNavigation::Down });
			CHECK(ui.GetInteraction().Focused == FindUiSystemItem(scene, 3).GetUUID());
			CHECK(GetUiSystemScroll(list) == glm::vec2(0.0f, 60.0f));
		}

		// Decision 29 (R-UI-DET): nothing while the interaction state and the event queue are empty, so a scene that holds
		// no interaction keeps its hash; otherwise the marker, every member of the state in the frozen order, then the queue
		// (events a script queued with Ui.SetFocus after the tick's UiEvents phase cross the tick boundary).
		TEST_CASE("UiSystem: the ui block is empty while the interaction state and the event queue are, and holds them otherwise")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity button = AddUiSystemElement(scene, "Button", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 100));
			const auto appendState = [](XXH64Hasher& hasher, const UiInteractionState& state)
			{
				hasher.UpdateU64(0x3165746174536955ull); // the bytes "UiState1"
				for (const UUID id : { state.Focused, state.Hovered, state.Pressed, state.Dragged })
					hasher.UpdateU64(id.GetValue());
				for (const float value : { state.DragOrigin.x, state.DragOrigin.y, state.DragStart.x, state.DragStart.y })
					hasher.UpdateU64(std::bit_cast<uint32_t>(value));
				hasher.UpdateU64(static_cast<uint64_t>(state.Stick));
				hasher.UpdateU64(state.PointerOverUi ? 1 : 0);
			};

			const XXH64Hasher untouched(0);
			XXH64Hasher empty(0);
			ui.AppendStateHash(empty);
			CHECK(empty.Digest() == untouched.Digest());

			// Hover the button (its event delivered, as the UiEvents phase takes it): the state is no longer empty.
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(10.0f) });
			CHECK(ui.TakeEvents().size() == 1);
			const UiInteractionState& state = ui.GetInteraction();
			REQUIRE(state.Hovered == button.GetUUID());
			XXH64Hasher expected(0);
			appendState(expected, state);
			expected.UpdateU64(0); // no queued event
			XXH64Hasher hovered(0);
			ui.AppendStateHash(hovered);
			CHECK(hovered.Digest() == expected.Digest());
			CHECK(hovered.Digest() != untouched.Digest());

			// A scene replacement resets the state, and the block is empty again.
			ui.ResetInteraction();
			XXH64Hasher reset(0);
			ui.AppendStateHash(reset);
			CHECK(reset.Digest() == untouched.Digest());

			// Focus set and cleared again by a script after the UiEvents phase: the state is empty, but its two Focus events
			// wait for the next UiEvents phase, so the block holds them; once taken, it is empty.
			REQUIRE(ui.SetFocus(scene, button.GetUUID()).has_value());
			REQUIRE(ui.SetFocus(scene, UUID{}).has_value());
			REQUIRE(ui.GetInteraction().IsEmpty());
			REQUIRE(ui.GetQueuedEventCount() == 2);
			XXH64Hasher queuedExpected(0);
			appendState(queuedExpected, UiInteractionState{});
			queuedExpected.UpdateU64(2);
			for (const bool focused : { true, false })
			{
				queuedExpected.UpdateU64(static_cast<uint64_t>(UiEventKind::Focus));
				queuedExpected.UpdateU64(button.GetUUID().GetValue());
				queuedExpected.UpdateU64(focused ? 1 : 0);
				queuedExpected.UpdateU64(0);                   // HasNumber
				for (const float value : { 0.0f, 0.0f, 0.0f }) // Number, Vector.x, Vector.y
					queuedExpected.UpdateU64(std::bit_cast<uint32_t>(value));
			}
			XXH64Hasher queued(0);
			ui.AppendStateHash(queued);
			CHECK(queued.Digest() == queuedExpected.Digest());
			CHECK(ui.TakeEvents().size() == 2);
			XXH64Hasher taken(0);
			ui.AppendStateHash(taken);
			CHECK(taken.Digest() == untouched.Digest());
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 47; stream T): the text step 7a.
		TEST_CASE("UiSystem: a focused text input takes the step's text and editing keys, and keyboard navigation pauses while it has focus")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity field = AddUiSystemElement(scene, "Field", canvas, glm::vec2(0.0f), glm::vec2(200.0f, 30.0f));
			field.AddComponent<UiTextInputComponent>(UiTextInputComponent{});
			Entity button = AddUiSystemElement(scene, "Button", canvas, glm::vec2(0.0f, 40.0f), glm::vec2(200.0f, 30.0f));
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(400, 300));

			// Text typed while nothing is focused goes nowhere.
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .Text = "lost" });
			CHECK(field.GetComponent<UiTextInputComponent>().Text.empty());
			CHECK(ui.TakeEvents().empty());

			// A press on the field focuses it; the next step's text and keys edit it, raising TextChanged.
			static_cast<void>(ProcessPointer(ui, scene, glm::vec2(10.0f), true, true, false));
			static_cast<void>(ProcessPointer(ui, scene, glm::vec2(10.0f), false, false, true));
			CHECK(ui.GetInteraction().Focused == field.GetUUID());
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .Text = "hi!", .TextKeys = UiTextKeys::Backspace });
			CHECK(field.GetComponent<UiTextInputComponent>().Text == "hi");
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Hover, field, false), MakeUiEvent(UiEventKind::TextChanged, field) });

			// The keyboard's Down, Left and Submit, which typing presses as keys too, do not navigate or submit while the field
			// has focus; a gamepad's Down does, and Next (Tab) would.
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .Navigation = UiNavigation::Down | UiNavigation::Left | UiNavigation::Submit });
			CHECK(ui.GetInteraction().Focused == field.GetUUID());
			CHECK(ui.TakeEvents().empty());

			// Enter submits the field; Submit through a gamepad does too.
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .TextKeys = UiTextKeys::Enter });
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Submit, field) });
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .Navigation = UiNavigation::Submit, .GamepadNavigation = UiNavigation::Submit });
			CHECK(ui.TakeEvents() == std::vector<UiEvent>{ MakeUiEvent(UiEventKind::Submit, field) });
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .Navigation = UiNavigation::Down, .GamepadNavigation = UiNavigation::Down });
			CHECK(ui.GetInteraction().Focused == button.GetUUID());
			// The button has focus now: a keyboard command (Up here), which the field ignored, navigates again, and typed text
			// goes nowhere.
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(-1000.0f), .Navigation = UiNavigation::Up, .Text = "x" });
			CHECK(ui.GetInteraction().Focused == field.GetUUID());
			CHECK(field.GetComponent<UiTextInputComponent>().Text == "hi");
		}

		// C22.3 (decisions 50 and 51; stream W): a World canvas takes the pointer through the camera.
		TEST_CASE("UiSystem: a World canvas's button is hovered and clicked through the camera, and its slider drags along its plane")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// The camera at (0, 0, 10) looking down -Z with a 90 degree field of view; the scene image 200 x 200, so a pointer at
			// the image's centre looks straight at the canvas's centre.
			Entity camera = scene.CreateEntity("Camera");
			camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 10.0f);
			camera.AddComponent<CameraComponent>(CameraComponent{ .VerticalFov = 90.0f, .Primary = true });
			// A World canvas of 200 x 100 canvas pixels at 20 pixels per unit (10 x 5 units) at the origin, facing the camera.
			Entity canvas = scene.CreateEntity("Board");
			canvas.AddComponent<UiCanvasComponent>(
				UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 200, .WorldHeight = 100, .PixelsPerUnit = 20.0f });
			Entity button = AddUiSystemElement(scene, "Button", canvas, glm::vec2(80.0f, 30.0f), glm::vec2(40.0f, 40.0f));
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity slider = AddUiSystemElement(scene, "Slider", canvas, glm::vec2(0.0f, 0.0f), glm::vec2(200.0f, 20.0f));
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Focusable = false });
			TransformSystem::Update(scene);
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(200, 200));

			const auto frame = [&scene](glm::vec2 pointer, bool down, bool pressed, bool released)
			{
				return UiInputFrame{ .Pointer = pointer,
					.PointerDown = down,
					.PointerPressed = pressed,
					.PointerReleased = released,
					.Ray = ComputeUiPointerRay(scene, pointer, glm::uvec2(200, 200)) };
			};
			// The image's centre (100, 100) meets the canvas's centre, canvas pixel (100, 50): inside the button.
			ui.ProcessInput(scene, frame(glm::vec2(100.0f), false, false, false));
			CHECK(ui.GetInteraction().Hovered == button.GetUUID());
			CHECK(ui.GetInteraction().PointerOverUi);
			ui.ProcessInput(scene, frame(glm::vec2(100.0f), true, true, false));
			ui.ProcessInput(scene, frame(glm::vec2(100.0f), false, false, true));
			const std::vector<UiEvent> clicked = ui.TakeEvents();
			CHECK(std::ranges::count(clicked, MakeUiEvent(UiEventKind::Click, button)) == 1);

			// The slider's left end is canvas x 0, world x -5; at the camera's distance of 10 a pointer 25 scene pixels right of
			// the centre looks at world x 2.5 (tan 45 degrees: 100 pixels to 10 units), canvas x 150: three quarters along it.
			// The press is on the slider's top row: canvas y 0 to 20 is world y 2.5 to 1.5, scene y 75 to 85.
			ui.ProcessInput(scene, frame(glm::vec2(125.0f, 80.0f), true, true, false));
			CHECK(ui.GetInteraction().Dragged == slider.GetUUID());
			CHECK(slider.GetComponent<UiSliderComponent>().Value == doctest::Approx(0.75f));
			// Dragging leftward along the plane, off the slider's rectangle, follows the plane: canvas x 50, a quarter.
			ui.ProcessInput(scene, frame(glm::vec2(75.0f, 150.0f), true, false, false));
			CHECK(slider.GetComponent<UiSliderComponent>().Value == doctest::Approx(0.25f));
			ui.ProcessInput(scene, frame(glm::vec2(75.0f, 150.0f), false, false, true));
			CHECK_FALSE(ui.GetInteraction().Dragged.IsValid());

			// Without a ray (no primary camera, or a frame without it) the World canvas is not hit.
			ui.ProcessInput(scene, UiInputFrame{ .Pointer = glm::vec2(100.0f) });
			CHECK_FALSE(ui.GetInteraction().Hovered.IsValid());
		}
	}

}
