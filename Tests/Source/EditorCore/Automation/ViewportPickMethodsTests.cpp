#include "TestsPCH.h"

#include "EditorCore/Automation/ViewportPickMethods.h"

#include "EditorCore/Viewport/EditorViewportState.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

namespace Engine {
	namespace {

		void MakePickScene(Test::AutomationFixture& setup)
		{
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", "engine://Meshes/Cube" } } } } } }));
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Camera" }, { "components", { { "Transform", { { "Translation", { 0, 0, 5 } } } }, { "Camera", { { "Primary", true } } } } } }));
			ExplicitRenderCamera camera;
			camera.Position = { 0, 0, 5 };
			camera.Target = { 0, 0, 0 };
			REQUIRE(setup.GetEditor().GetViewportState().SetCamera(camera));
		}

	}
	TEST_SUITE("Automation")
	{
		TEST_CASE("ViewportPick: scene and game rays use the current framebuffer extent")
		{
			Test::AutomationFixture setup("PickExtents");
			MakePickScene(setup);
			for (auto view : { ViewportView::Scene, ViewportView::Game })
			{
				REQUIRE(setup.GetEditor().GetViewportState().SetPixelSize(view, glm::uvec2(5, 3)));
				const auto hit = setup.Call("viewport.pick", Json{ { "x", 2 }, { "y", 1 }, { "view", view == ViewportView::Scene ? "scene" : "game" } });
				REQUIRE_MESSAGE(hit.has_value(), hit.error().ToString());
				CHECK((*hit)["width"] == Json(5));
				CHECK((*hit)["height"] == Json(3));
				CHECK((*hit)["raycast"]["entity"]["name"] == Json("Cube"));
				CHECK((*hit)["raycast"]["distance"] == Json(4.5f));
			}
		}
		TEST_CASE("ViewportPick: boundary pixels and missing cameras return located errors")
		{
			Test::AutomationFixture setup("PickErrors");
			const auto missing = setup.Call("viewport.pick", Json{ { "x", 0 }, { "y", 0 }, { "view", "game" } });
			REQUIRE_FALSE(missing);
			CHECK(missing.error().GetCode() == ErrorCode::InvalidState);
			CHECK(missing.error().GetMessageText().find("SCENE_NO_PRIMARY_CAMERA") != std::string::npos);
			MakePickScene(setup);
			REQUIRE(setup.GetEditor().GetViewportState().SetPixelSize(ViewportView::Scene, glm::uvec2(5, 3)));
			for (const auto& [x, y, pointer] : std::vector<std::tuple<uint32_t, uint32_t, std::string>>{ { 5, 0, "/x" }, { 0, 3, "/y" } })
			{
				const auto response = setup.Request("viewport.pick", Json{ { "x", x }, { "y", y }, { "view", "scene" } });
				CHECK(response["error"]["data"]["issues"][0]["pointer"] == Json(pointer));
			}
			CHECK(setup.Call("viewport.pick", Json{ { "x", 4 }, { "y", 2 }, { "view", "scene" } }));
			REQUIRE(setup.GetEditor().GetViewportState().SetPixelSize(ViewportView::Scene, glm::uvec2(0)));
			const auto minimized = setup.Call("viewport.pick", Json{ { "x", 0 }, { "y", 0 }, { "view", "scene" } });
			REQUIRE_FALSE(minimized);
			CHECK(minimized.error().GetCode() == ErrorCode::InvalidState);
		}
		TEST_CASE("ViewportPick: camera clipping uses ray intervals in both projections")
		{
			Test::AutomationFixture setup("PickClipping");
			REQUIRE(setup.GetEditor().GetViewportState().SetPixelSize(ViewportView::Game, glm::uvec2(4, 4)));
			REQUIRE(setup.Call("entity.create", Json{ { "name", "Camera" }, { "components", { { "Camera", { { "Primary", true }, { "VerticalFov", 90 }, { "NearClip", 1 }, { "FarClip", 10 } } } } } }));
			for (const auto& [name, position, scale] : std::vector<std::tuple<std::string, Json, Json>>{
					 { "Foreground", Json::array({ 0.375, 0.125, -0.5 }), Json::array({ 0.5, 0.5, 0.1 }) },
					 { "Far", Json::array({ 6.75, 2.25, -9 }), Json::array({ 0.5, 0.5, 0.5 }) } })
				REQUIRE(setup.Call("entity.create", Json{ { "name", name }, { "components", { { "Transform", { { "Translation", position }, { "Scale", scale } } }, { "MeshRenderer", { { "Mesh", "engine://Meshes/Cube" } } } } } }));
			const Json pixel{ { "x", 3 }, { "y", 1 }, { "view", "game" } };
			const auto perspective = setup.Call("viewport.pick", pixel);
			REQUIRE(perspective);
			CHECK((*perspective)["raycast"]["entity"]["name"] == Json("Far"));
			CHECK(JsonReader((*perspective)["raycast"]["distance"]).ReadFloat().value_or(0) > 10);
			REQUIRE(setup.Call("entity.update", Json{ { "entity", "/Camera" }, { "components", { { "Camera", { { "Projection", "Orthographic" }, { "OrthographicSize", 9 } } } } } }));
			const auto orthographic = setup.Call("viewport.pick", pixel);
			REQUIRE(orthographic);
			CHECK((*orthographic)["raycast"]["entity"]["name"] == Json("Far"));
			CHECK(JsonReader((*orthographic)["raycast"]["distance"]).ReadFloat().value_or(0) == doctest::Approx(7.75f));
		}
		TEST_CASE("ViewportPick: scene and game extents remain independent across resize and screenshots")
		{
			Test::AutomationFixture setup("PickResize");
			MakePickScene(setup);
			REQUIRE(setup.GetEditor().GetViewportState().SetPixelSize(ViewportView::Scene, glm::uvec2(5, 3)));
			REQUIRE(setup.GetEditor().GetViewportState().SetPixelSize(ViewportView::Game, glm::uvec2(9, 7)));
			const auto before = setup.Call("viewport.pick", Json{ { "x", 2 }, { "y", 1 }, { "view", "scene" } });
			REQUIRE(before);
			// A failed capture must not publish its requested size as a displayed framebuffer extent either.
			const auto screenshot = setup.Call("viewport.screenshot", Json{ { "view", "scene" }, { "width", 101 }, { "height", 99 } });
			REQUIRE_FALSE(screenshot);
			const auto after = setup.Call("viewport.pick", Json{ { "x", 2 }, { "y", 1 }, { "view", "scene" } });
			REQUIRE(after);
			CHECK((*after)["raycast"] == (*before)["raycast"]);
			CHECK((*after)["width"] == Json(5));
			const auto game = setup.Call("viewport.pick", Json{ { "x", 4 }, { "y", 3 }, { "view", "game" } });
			REQUIRE(game);
			CHECK((*game)["width"] == Json(9));
			CHECK((*game)["height"] == Json(7));
			REQUIRE(setup.GetEditor().GetViewportState().SetPixelSize(ViewportView::Game, glm::uvec2(13, 11)));
			CHECK(setup.GetEditor().GetViewportState().GetPixelSize(ViewportView::Scene).value() == glm::uvec2(5, 3));
		}
	}

}
