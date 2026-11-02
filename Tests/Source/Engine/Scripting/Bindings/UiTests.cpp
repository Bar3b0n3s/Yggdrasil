#include "TestsPCH.h"

#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiSystem.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

// The Ui module (M22, C22.2, R-UI-EVENTS; Docs/Decisions/0033-m22-contract.md decision 28): the game UI's interaction state
// and layout, read through the session's UiSystem (IScriptHost::GetUi), and Ui.SetFocus. Skipped by the contract until
// stream B registers the module.

namespace Engine {

	TEST_SUITE("Scripting")
	{
		// Decision 28: the module reads the session's interaction state and layout; SetFocus moves focus to a focusable
		// widget and raises for any other element; without a session's UI (an edit evaluation) it reports nothing.
		TEST_CASE("Ui: the module reads focus, hover, press and rectangles, and SetFocus refuses an element that cannot take focus" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity button = scene.CreateEntity("Button", canvas);
			button.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = glm::vec2(10.0f, 20.0f), .Size = glm::vec2(100.0f, 30.0f) });
			button.AddComponent<UiImageComponent>(UiImageComponent{});
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			Entity label = scene.CreateEntity("Label", canvas);
			label.AddComponent<UiRectComponent>(UiRectComponent{});
			UiSystem ui;
			ui.LayoutStep(scene, glm::uvec2(320, 180));
			fixture.Ui = &ui;
			REQUIRE(fixture.Start());

			const auto result = fixture.Evaluate(R"(
local button = Scene.FindByName("Button")
assert(Ui.GetFocus() == nil and Ui.GetHovered() == nil and Ui.GetPressed() == nil)
assert(Ui.IsPointerOverUi() == false)
local rect = Ui.GetRect(button)
assert(rect.Min == vector.create(10, 20, 0) and rect.Max == vector.create(110, 50, 0))
assert(Ui.GetRect(Scene.FindByName("Canvas")) == nil)
Ui.SetFocus(button)
assert(Ui.GetFocus() == button)
assert(not pcall(Ui.SetFocus, Scene.FindByName("Label")))
assert(Ui.GetFocus() == button)
Ui.SetFocus(nil)
return Ui.GetFocus() == nil
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(ui.GetQueuedEventCount() == 2);

			fixture.Ui = nullptr;
			const auto without = fixture.Evaluate(R"(
assert(Ui.GetFocus() == nil and Ui.GetRect(Scene.FindByName("Button")) == nil and not Ui.IsPointerOverUi())
return not pcall(Ui.SetFocus, Scene.FindByName("Button"))
)");
			REQUIRE_MESSAGE(without.has_value(), (without ? "" : without.error().ToString()));
			CHECK(without->Value.Get() == Json(true));
		}
	}

}
