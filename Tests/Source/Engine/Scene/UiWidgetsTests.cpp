#include "TestsPCH.h"

#include "Engine/Scene/UiWidgets.h"

#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiLayout.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <string_view>
#include <vector>

// The widgets' behaviours (M22, C22.2, R-UI-WIDGETS and R-UI-SCROLL; Docs/Decisions/0033-m22-contract.md decisions 24 and
// 26): activation, the slider's value, step and pointer, and the scroll view's clamp, wheel, drag, step and reveal, on
// tables ComputeUiLayout lays out at one scene pixel per canvas unit (streams B: Button, Toggle and Slider; C: ScrollView).

namespace Engine {

	namespace {

		UiRectComponent MakeWidgetRect(glm::vec2 position, glm::vec2 size)
		{
			return UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = position, .Size = size };
		}

		Entity AddWidgetElement(Scene& scene, std::string_view name, Entity parent, glm::vec2 position, glm::vec2 size)
		{
			Entity entity = scene.CreateEntity(name, parent);
			entity.AddComponent<UiRectComponent>(MakeWidgetRect(position, size));
			return entity;
		}

		Entity AddWidgetCanvas(Scene& scene)
		{
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			return canvas;
		}

		// A vertical scroll view at the canvas's origin, 100 x 40, over a content of 100 x 200 (an overflow of 160).
		struct ScrollViewScene
		{
			Entity Canvas{};
			Entity View{};
			Entity Content{};
			Entity Item{};
		};

		ScrollViewScene MakeScrollViewScene(Scene& scene)
		{
			ScrollViewScene result;
			result.Canvas = AddWidgetCanvas(scene);
			result.View = AddWidgetElement(scene, "View", result.Canvas, glm::vec2(0.0f), glm::vec2(100.0f, 40.0f));
			result.Content = AddWidgetElement(scene, "Content", result.View, glm::vec2(0.0f), glm::vec2(100.0f, 200.0f));
			result.Item = AddWidgetElement(scene, "Item", result.Content, glm::vec2(0.0f, 120.0f), glm::vec2(100.0f, 20.0f));
			result.Item.AddComponent<UiButtonComponent>(UiButtonComponent{});
			result.View.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = result.Content.GetUUID() });
			return result;
		}

	}

	TEST_SUITE("Scene")
	{
		// Decision 24: activation clicks a button and flips a toggle; a disabled widget or a plain element does nothing.
		TEST_CASE("UiWidgets: activating a button raises Click, and a toggle flips IsOn and raises ValueChanged")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddWidgetCanvas(scene);
			Entity button = AddWidgetElement(scene, "Button", canvas, glm::vec2(0.0f), glm::vec2(10.0f));
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity toggle = AddWidgetElement(scene, "Toggle", canvas, glm::vec2(0.0f, 20.0f), glm::vec2(10.0f));
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{});
			Entity disabled = AddWidgetElement(scene, "Disabled", canvas, glm::vec2(0.0f, 40.0f), glm::vec2(10.0f));
			disabled.AddComponent<UiToggleComponent>(UiToggleComponent{ .Interactable = false });
			const Entity plain = AddWidgetElement(scene, "Plain", canvas, glm::vec2(0.0f, 60.0f), glm::vec2(10.0f));

			std::vector<UiEvent> events;
			ActivateUiWidget(scene, button.GetUUID(), events);
			CHECK(events == std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Click, .Target = button.GetUUID() } });
			events.clear();
			ActivateUiWidget(scene, toggle.GetUUID(), events);
			CHECK(toggle.GetComponent<UiToggleComponent>().IsOn);
			CHECK(events == std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::ValueChanged, .Target = toggle.GetUUID(), .Flag = true } });
			events.clear();
			ActivateUiWidget(scene, toggle.GetUUID(), events);
			CHECK_FALSE(toggle.GetComponent<UiToggleComponent>().IsOn);
			CHECK(events == std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::ValueChanged, .Target = toggle.GetUUID(), .Flag = false } });
			events.clear();
			ActivateUiWidget(scene, disabled.GetUUID(), events);
			ActivateUiWidget(scene, plain.GetUUID(), events);
			CHECK(events.empty());
			CHECK_FALSE(disabled.GetComponent<UiToggleComponent>().IsOn);
		}

		// UiSliderComponent.h: the value as read is clamped and rounded, and with WholeNumbers kept to the whole numbers of the
		// range, so it never leaves it; the pointer sets it along the slide rectangle (the slider's own here: no handle or
		// fill), from the left or the bottom; the directions along it step it, a whole-number slider by a whole step of at
		// least 1 whatever its Step.
		TEST_CASE("UiWidgets: a slider reads its value clamped, follows the pointer and steps along its direction")
		{
			CHECK(ReadUiSliderValue(UiSliderComponent{ .Value = 12.0f, .Max = 10.0f }) == 10.0f);
			CHECK(ReadUiSliderValue(UiSliderComponent{ .Value = -1.0f, .Max = 10.0f }) == 0.0f);
			CHECK(ReadUiSliderValue(UiSliderComponent{ .Value = 2.4f, .Max = 10.0f, .WholeNumbers = true }) == 2.0f);
			CHECK(ReadUiSliderValue(UiSliderComponent{ .Value = 2.5f, .Max = 10.0f, .WholeNumbers = true }) == 3.0f);
			CHECK(GetUiSliderPosition(UiSliderComponent{ .Value = 2.5f, .Max = 10.0f }) == 0.25f);
			// Ends that are not whole numbers: the value stays within the whole numbers 1 to 10 of [0.5, 10.5], and its
			// position within [0, 1]; a range without a whole number reads its Min.
			const UiSliderComponent halves{ .Value = 10.5f, .Min = 0.5f, .Max = 10.5f, .WholeNumbers = true };
			CHECK(ReadUiSliderValue(halves) == 10.0f);
			CHECK(GetUiSliderPosition(halves) == 0.95f);
			CHECK(ReadUiSliderValue(UiSliderComponent{ .Value = 0.5f, .Min = 0.5f, .Max = 10.5f, .WholeNumbers = true }) == 1.0f);
			const UiSliderComponent narrow{ .Value = 0.7f, .Min = 0.2f, .Max = 0.8f, .WholeNumbers = true };
			CHECK(ReadUiSliderValue(narrow) == 0.2f);
			CHECK(GetUiSliderPosition(narrow) == 0.0f);

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddWidgetCanvas(scene);
			Entity slider = AddWidgetElement(scene, "Slider", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Max = 10.0f });
			Entity vertical = AddWidgetElement(scene, "Vertical", canvas, glm::vec2(200.0f, 0.0f), glm::vec2(20.0f, 100.0f));
			vertical.AddComponent<UiSliderComponent>(UiSliderComponent{ .Max = 1.0f, .Direction = UiLayoutDirection::Vertical });
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(400, 200));

			std::vector<UiEvent> events;
			SetUiSliderFromPointer(scene, table, slider.GetUUID(), glm::vec2(25.0f, 10.0f), events);
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 2.5f);
			CHECK(events == std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::ValueChanged, .Target = slider.GetUUID(), .HasNumber = true, .Number = 2.5f } });
			events.clear();
			SetUiSliderFromPointer(scene, table, slider.GetUUID(), glm::vec2(150.0f, 10.0f), events);
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 10.0f);
			events.clear();
			// The same value raises nothing.
			SetUiSliderFromPointer(scene, table, slider.GetUUID(), glm::vec2(100.0f, 10.0f), events);
			CHECK(events.empty());
			// A vertical slider counts from its bottom edge.
			SetUiSliderFromPointer(scene, table, vertical.GetUUID(), glm::vec2(210.0f, 75.0f), events);
			CHECK(vertical.GetComponent<UiSliderComponent>().Value == 0.25f);

			// Steps of a tenth of the range along the slider; across it, the direction is not the slider's.
			events.clear();
			slider.GetComponent<UiSliderComponent>().Value = 2.5f;
			CHECK(StepUiSlider(scene, slider.GetUUID(), UiDirection::Right, events));
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 3.5f);
			CHECK(StepUiSlider(scene, slider.GetUUID(), UiDirection::Left, events));
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 2.5f);
			CHECK(events.size() == 2);
			CHECK_FALSE(StepUiSlider(scene, slider.GetUUID(), UiDirection::Up, events));
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 2.5f);
			CHECK(StepUiSlider(scene, vertical.GetUUID(), UiDirection::Up, events));
			CHECK(vertical.GetComponent<UiSliderComponent>().Value == 0.35f);

			// With WholeNumbers a Step below 1 still moves by 1 (0.4 both ways; 0.5 rounds to 1 downwards too), and a
			// fractional step rounds to a whole one (2.6 moves by 3).
			Entity whole = AddWidgetElement(scene, "Whole", canvas, glm::vec2(0.0f, 50.0f), glm::vec2(100.0f, 20.0f));
			whole.AddComponent<UiSliderComponent>(UiSliderComponent{ .Value = 2.0f, .Max = 10.0f, .WholeNumbers = true, .Step = 0.4f });
			events.clear();
			CHECK(StepUiSlider(scene, whole.GetUUID(), UiDirection::Right, events));
			CHECK(whole.GetComponent<UiSliderComponent>().Value == 3.0f);
			CHECK(StepUiSlider(scene, whole.GetUUID(), UiDirection::Left, events));
			CHECK(whole.GetComponent<UiSliderComponent>().Value == 2.0f);
			CHECK(events.size() == 2);
			whole.GetComponent<UiSliderComponent>().Step = 0.5f;
			CHECK(StepUiSlider(scene, whole.GetUUID(), UiDirection::Left, events));
			CHECK(whole.GetComponent<UiSliderComponent>().Value == 1.0f);
			whole.GetComponent<UiSliderComponent>().Step = 2.6f;
			CHECK(StepUiSlider(scene, whole.GetUUID(), UiDirection::Right, events));
			CHECK(whole.GetComponent<UiSliderComponent>().Value == 4.0f);
		}

		// UiSliderComponent.h: the slide rectangle is the Handle's parent element's, else the Fill's parent's, else the slider's
		// own, and a part that is not an element below the slider is no part; a slider that is not interactable, an element
		// without a slider and a direction across a slider leave the value alone; an inverted range reads its Min.
		TEST_CASE("UiWidgets: a slider's pointer follows its Handle's parent element, else its Fill's, and ignores what is not below it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddWidgetCanvas(scene);
			// The slider spans x 0 to 200; its track, the Handle's parent, spans x 50 to 150; the Fill is the track's sibling
			// inside an area of x 20 to 120.
			Entity slider = AddWidgetElement(scene, "Slider", canvas, glm::vec2(0.0f), glm::vec2(200.0f, 20.0f));
			Entity track = AddWidgetElement(scene, "Track", slider, glm::vec2(50.0f, 0.0f), glm::vec2(100.0f, 20.0f));
			Entity handle = AddWidgetElement(scene, "Handle", track, glm::vec2(0.0f), glm::vec2(10.0f, 20.0f));
			Entity area = AddWidgetElement(scene, "Area", slider, glm::vec2(20.0f, 0.0f), glm::vec2(100.0f, 20.0f));
			Entity fill = AddWidgetElement(scene, "Fill", area, glm::vec2(0.0f), glm::vec2(0.0f, 20.0f));
			Entity sibling = AddWidgetElement(scene, "Sibling", canvas, glm::vec2(0.0f, 100.0f), glm::vec2(40.0f, 20.0f));
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Max = 100.0f, .Fill = fill.GetUUID(), .Handle = handle.GetUUID() });
			UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(400, 200));

			std::vector<UiEvent> events;
			// x 75 is a quarter of the track: the Handle's parent rules.
			SetUiSliderFromPointer(scene, table, slider.GetUUID(), glm::vec2(75.0f, 10.0f), events);
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 25.0f);
			// Without a Handle, x 75 is 55 % of the Fill's parent, the area.
			slider.GetComponent<UiSliderComponent>().Handle = UUID{};
			table = ComputeUiLayout(scene, glm::uvec2(400, 200));
			SetUiSliderFromPointer(scene, table, slider.GetUUID(), glm::vec2(75.0f, 10.0f), events);
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 55.0f);
			// A Handle and a Fill that are no parts (a sibling of the slider, the slider itself): the slider's own rectangle.
			slider.GetComponent<UiSliderComponent>().Handle = sibling.GetUUID();
			slider.GetComponent<UiSliderComponent>().Fill = slider.GetUUID();
			table = ComputeUiLayout(scene, glm::uvec2(400, 200));
			SetUiSliderFromPointer(scene, table, slider.GetUUID(), glm::vec2(150.0f, 10.0f), events);
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 75.0f);
			CHECK(events.size() == 3);

			// Neither a disabled slider, nor an element without one, nor an element outside the table moves.
			events.clear();
			slider.GetComponent<UiSliderComponent>().Interactable = false;
			SetUiSliderFromPointer(scene, table, slider.GetUUID(), glm::vec2(0.0f, 10.0f), events);
			CHECK_FALSE(StepUiSlider(scene, slider.GetUUID(), UiDirection::Left, events));
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 75.0f);
			SetUiSliderFromPointer(scene, table, sibling.GetUUID(), glm::vec2(0.0f, 110.0f), events);
			CHECK_FALSE(StepUiSlider(scene, sibling.GetUUID(), UiDirection::Right, events));
			slider.GetComponent<UiSliderComponent>().Interactable = true;
			SetUiSliderFromPointer(scene, UiLayoutTable{}, slider.GetUUID(), glm::vec2(0.0f, 10.0f), events);
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 75.0f);
			CHECK(events.empty());

			// A stored value outside the range is written as read by the next interaction, which raises nothing when the value
			// as read stays: 120 reads as 100, and the pointer at the right end sets 100.
			slider.GetComponent<UiSliderComponent>().Value = 120.0f;
			SetUiSliderFromPointer(scene, table, slider.GetUUID(), glm::vec2(200.0f, 10.0f), events);
			CHECK(slider.GetComponent<UiSliderComponent>().Value == 100.0f);
			CHECK(events.empty());
			// A range a script left inverted reads its Min, at position 0.
			CHECK(ReadUiSliderValue(UiSliderComponent{ .Value = 3.0f, .Min = 5.0f, .Max = 1.0f }) == 5.0f);
			CHECK(GetUiSliderPosition(UiSliderComponent{ .Value = 3.0f, .Min = 5.0f, .Max = 1.0f }) == 0.0f);
		}

		// UiScrollViewComponent.h: the position as read stays within the content's overflow on the scrolling axes; the wheel,
		// a drag and the steps move it and raise Scroll only when it changes.
		TEST_CASE("UiWidgets: a scroll view stays within its content's overflow and scrolls by the wheel, a drag and its steps")
		{
			const UiScrollViewComponent vertical{};
			CHECK(ClampUiScrollPosition(vertical, glm::vec2(100.0f, 40.0f), glm::vec2(100.0f, 200.0f), glm::vec2(5.0f, 300.0f)) == glm::vec2(0.0f, 160.0f));
			CHECK(ClampUiScrollPosition(vertical, glm::vec2(100.0f, 40.0f), glm::vec2(100.0f, 200.0f), glm::vec2(5.0f, 30.0f)) == glm::vec2(0.0f, 30.0f));
			const UiScrollViewComponent both{ .Horizontal = true };
			CHECK(ClampUiScrollPosition(both, glm::vec2(100.0f, 40.0f), glm::vec2(50.0f, 30.0f), glm::vec2(10.0f)) == glm::vec2(0.0f));
			CHECK(ClampUiScrollPosition(both, glm::vec2(100.0f, 40.0f), glm::vec2(300.0f, 30.0f), glm::vec2(500.0f, 10.0f)) == glm::vec2(200.0f, 0.0f));

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const ScrollViewScene ui = MakeScrollViewScene(scene);
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 200));
			CHECK(FindUiScrollView(scene, table, ui.Item.GetUUID()) == ui.View.GetUUID());
			CHECK(FindUiScrollView(scene, table, ui.View.GetUUID()) == ui.View.GetUUID());
			CHECK_FALSE(FindUiScrollView(scene, table, ui.Canvas.GetUUID()).IsValid());

			std::vector<UiEvent> events;
			CHECK(ScrollUiScrollView(scene, table, ui.View.GetUUID(), glm::vec2(0.0f, 50.0f), events));
			CHECK(ui.View.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 50.0f));
			CHECK(events == std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Scroll, .Target = ui.View.GetUUID(), .Vector = glm::vec2(0.0f, 50.0f) } });
			CHECK(ScrollUiScrollView(scene, table, ui.View.GetUUID(), glm::vec2(30.0f, 500.0f), events));
			CHECK(ui.View.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 160.0f));
			events.clear();
			CHECK_FALSE(ScrollUiScrollView(scene, table, ui.View.GetUUID(), glm::vec2(0.0f, 10.0f), events));
			CHECK(events.empty());

			// A drag that began at 160: the pointer moved 60 pixels down, so the content follows it down.
			DragUiScrollView(scene, table, ui.View.GetUUID(), glm::vec2(0.0f, 160.0f), glm::vec2(0.0f, 60.0f), events);
			CHECK(ui.View.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 100.0f));
			CHECK(StepUiScrollView(scene, table, ui.View.GetUUID(), UiDirection::Up, events));
			CHECK(ui.View.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 60.0f));
			CHECK_FALSE(StepUiScrollView(scene, table, ui.View.GetUUID(), UiDirection::Left, events));
			CHECK(events.size() == 2);
		}

		// Decision 23: focus moved into a scroll view's content scrolls the view the least that shows the focused element.
		TEST_CASE("UiWidgets: focusing an element inside a scroll view's content reveals it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const ScrollViewScene ui = MakeScrollViewScene(scene);
			UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 200));

			// The item spans y 120 to 140 in a view of 0 to 40: scrolling by 100 puts its bottom on the view's.
			std::vector<UiEvent> events;
			RevealInUiScrollViews(scene, table, ui.Item.GetUUID(), events);
			CHECK(ui.View.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 100.0f));
			CHECK(events == std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Scroll, .Target = ui.View.GetUUID(), .Vector = glm::vec2(0.0f, 100.0f) } });

			// Shown already: nothing moves.
			table = ComputeUiLayout(scene, glm::uvec2(200, 200));
			events.clear();
			RevealInUiScrollViews(scene, table, ui.Item.GetUUID(), events);
			CHECK(events.empty());
			CHECK(ui.View.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 100.0f));
		}

		// UiScrollViewComponent.h: a Content that is not an element below the scroll view leaves nothing to scroll, the
		// functions act only on a laid-out scroll view, and the wheel's scroll view is the innermost interactable one.
		TEST_CASE("UiWidgets: a scroll view without a Content below it scrolls nothing, and the wheel passes over one that is not interactable")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddWidgetCanvas(scene);
			Entity outer = AddWidgetElement(scene, "Outer", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 100.0f));
			Entity sibling = AddWidgetElement(scene, "Sibling", outer, glm::vec2(0.0f, 50.0f), glm::vec2(100.0f, 200.0f));
			Entity view = AddWidgetElement(scene, "View", outer, glm::vec2(0.0f), glm::vec2(100.0f, 40.0f));
			Entity item = AddWidgetElement(scene, "Item", view, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			outer.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = sibling.GetUUID() });
			// The view names its sibling as its Content: a stored position of 30 reads as 0.
			view.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = sibling.GetUUID(), .ScrollPosition = glm::vec2(0.0f, 30.0f) });
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 400));

			std::vector<UiEvent> events;
			CHECK_FALSE(ScrollUiScrollView(scene, table, view.GetUUID(), glm::vec2(0.0f, 40.0f), events));
			CHECK(StepUiScrollView(scene, table, view.GetUUID(), UiDirection::Down, events));
			DragUiScrollView(scene, table, view.GetUUID(), glm::vec2(0.0f), glm::vec2(0.0f, -60.0f), events);
			CHECK(events.empty());
			CHECK(view.GetComponent<UiScrollViewComponent>().ScrollPosition == glm::vec2(0.0f, 30.0f));
			view.GetComponent<UiScrollViewComponent>().Content = UUID();
			CHECK_FALSE(ScrollUiScrollView(scene, table, view.GetUUID(), glm::vec2(0.0f, 40.0f), events));
			// An element that is not a scroll view, and a scroll view the table does not hold, do nothing.
			CHECK_FALSE(ScrollUiScrollView(scene, table, item.GetUUID(), glm::vec2(0.0f, 40.0f), events));
			CHECK_FALSE(StepUiScrollView(scene, table, item.GetUUID(), UiDirection::Down, events));
			CHECK_FALSE(ScrollUiScrollView(scene, UiLayoutTable{}, outer.GetUUID(), glm::vec2(0.0f, 40.0f), events));
			CHECK(events.empty());

			// The wheel over the item reaches the view; with the view disabled, the outer scroll view; with both, none.
			CHECK(FindUiScrollView(scene, table, item.GetUUID()) == view.GetUUID());
			view.GetComponent<UiScrollViewComponent>().Interactable = false;
			CHECK(FindUiScrollView(scene, table, item.GetUUID()) == outer.GetUUID());
			outer.GetComponent<UiScrollViewComponent>().Interactable = false;
			CHECK_FALSE(FindUiScrollView(scene, table, item.GetUUID()).IsValid());
		}

		// RevealInUiScrollViews: the scroll views holding the element reveal it from the innermost outward, each moving the
		// element by its change before the next; an element above the view scrolls back, and one larger than the view shows
		// its top-left corner first.
		TEST_CASE("UiWidgets: nested scroll views reveal a focused element from the innermost outward")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = AddWidgetCanvas(scene);
			// An outer view of 100 over a content of 400 holding, at y 200, an inner view of 50 over a content of 300.
			Entity outer = AddWidgetElement(scene, "Outer", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 100.0f));
			Entity outerContent = AddWidgetElement(scene, "OuterContent", outer, glm::vec2(0.0f), glm::vec2(100.0f, 400.0f));
			Entity inner = AddWidgetElement(scene, "Inner", outerContent, glm::vec2(0.0f, 200.0f), glm::vec2(100.0f, 50.0f));
			Entity innerContent = AddWidgetElement(scene, "InnerContent", inner, glm::vec2(0.0f), glm::vec2(100.0f, 300.0f));
			Entity top = AddWidgetElement(scene, "Top", innerContent, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			Entity big = AddWidgetElement(scene, "Big", innerContent, glm::vec2(0.0f, 100.0f), glm::vec2(100.0f, 80.0f));
			Entity bottom = AddWidgetElement(scene, "Bottom", innerContent, glm::vec2(0.0f, 250.0f), glm::vec2(100.0f, 20.0f));
			outer.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = outerContent.GetUUID() });
			inner.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = innerContent.GetUUID() });
			const auto positionOf = [](Entity view)
			{
				return view.GetComponent<UiScrollViewComponent>().ScrollPosition;
			};

			// Bottom spans 450 to 470: the inner view (200 to 250) scrolls by 220, which puts it at 230 to 250, then the outer
			// view (0 to 100) by 150.
			std::vector<UiEvent> events;
			RevealInUiScrollViews(scene, ComputeUiLayout(scene, glm::uvec2(200, 600)), bottom.GetUUID(), events);
			CHECK(positionOf(inner) == glm::vec2(0.0f, 220.0f));
			CHECK(positionOf(outer) == glm::vec2(0.0f, 150.0f));
			CHECK(events
				== std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Scroll, .Target = inner.GetUUID(), .Vector = glm::vec2(0.0f, 220.0f) },
					UiEvent{ .Kind = UiEventKind::Scroll, .Target = outer.GetUUID(), .Vector = glm::vec2(0.0f, 150.0f) } });
			UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 600));
			CHECK(FindUiLayoutNode(table, bottom.GetUUID())->Min == glm::vec2(0.0f, 80.0f));
			CHECK(FindUiLayoutNode(table, bottom.GetUUID())->Max == glm::vec2(100.0f, 100.0f));

			// Top now lies above the inner view (50 to 100): the inner view scrolls back to 0, which shows it in the outer one.
			events.clear();
			RevealInUiScrollViews(scene, table, top.GetUUID(), events);
			CHECK(positionOf(inner) == glm::vec2(0.0f));
			CHECK(positionOf(outer) == glm::vec2(0.0f, 150.0f));
			CHECK(events.size() == 1);

			// Big (80, at 150 to 230) is larger than the inner view (50 to 100): the inner view scrolls by 100 to put its top on
			// the view's, then the outer view (0 to 100), which it overflows at 130, by 30 more.
			events.clear();
			table = ComputeUiLayout(scene, glm::uvec2(200, 600));
			RevealInUiScrollViews(scene, table, big.GetUUID(), events);
			CHECK(positionOf(inner) == glm::vec2(0.0f, 100.0f));
			CHECK(positionOf(outer) == glm::vec2(0.0f, 180.0f));
			table = ComputeUiLayout(scene, glm::uvec2(200, 600));
			CHECK(FindUiLayoutNode(table, big.GetUUID())->Min.y == FindUiLayoutNode(table, inner.GetUUID())->Min.y);
		}
	}

}
