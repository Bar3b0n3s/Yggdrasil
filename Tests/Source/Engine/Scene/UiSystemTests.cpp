#include "TestsPCH.h"

#include "Engine/Scene/UiSystem.h"

#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

// The play session's UI (M22, C22.1; Docs/Decisions/0033-m22-contract.md decision 3): the step's and the frame's layouts and
// which one is current (ADR 0028's phase-2 ledger: UiInput hit-tests the later of the two).

namespace Engine {

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
	}

}
