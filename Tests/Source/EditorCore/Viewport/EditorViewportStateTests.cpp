#include "TestsPCH.h"
#include "EditorCore/Viewport/EditorViewportState.h"

#include "Engine/Automation/Methods/ScreenshotMethods.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>

#include <limits>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorViewportState: scene and game report independent rendered extents")
		{
			EditorViewportState state;
			REQUIRE(state.GetPixelSize(ViewportView::Scene).has_value());
			CHECK(*state.GetPixelSize(ViewportView::Scene) == glm::uvec2(640, 360));
			CHECK(*state.GetPixelSize(ViewportView::Game) == glm::uvec2(640, 360));
			REQUIRE(state.SetPixelSize(ViewportView::Scene, { 800, 600 }).has_value());
			REQUIRE(state.SetPixelSize(ViewportView::Game, { 1024, 768 }).has_value());
			CHECK(state.GetSceneSize() == glm::uvec2(800, 600));
			CHECK(*state.GetPixelSize(ViewportView::Scene) == glm::uvec2(800, 600));
			CHECK(*state.GetPixelSize(ViewportView::Game) == glm::uvec2(1024, 768));
			REQUIRE(state.SetGameResolution({ 1920, 1080 }).has_value());
			CHECK(state.GetGameResolution() == glm::uvec2(1920, 1080));
			CHECK(*state.GetPixelSize(ViewportView::Game) == glm::uvec2(1024, 768));
			CHECK_FALSE(state.SetGameResolution({ 0, 1 }).has_value());
			CHECK_FALSE(state.SetPixelSize(ViewportView::Scene, { 8193, 1 }).has_value());
			CHECK_FALSE(state.SetPixelSize(ViewportView::Game, { 1, 0 }).has_value());
			CHECK_FALSE(state.GetPixelSize(static_cast<ViewportView>(255)).has_value());
			CHECK_FALSE(state.SetPixelSize(static_cast<ViewportView>(255), { 1, 1 }).has_value());
			CHECK(*state.GetPixelSize(ViewportView::Scene) == glm::uvec2(800, 600));
		}

		TEST_CASE("EditorViewportState: hiding an image preserves camera aspect but refuses picking")
		{
			EditorViewportState state;
			REQUIRE(state.SetPixelSize(ViewportView::Scene, { 900, 600 }).has_value());
			REQUIRE(state.SetPixelSize(ViewportView::Scene, { 0, 0 }).has_value());
			const auto hidden = state.GetPixelSize(ViewportView::Scene);
			REQUIRE_FALSE(hidden.has_value());
			CHECK(hidden.error().GetCode() == ErrorCode::InvalidState);
			CHECK(state.GetSceneSize() == glm::uvec2(900, 600));
			CHECK(state.GetPixelSize(ViewportView::Game).has_value());
			state.SetSceneSize({ 0, 600 });
			CHECK(state.GetSceneSize() == glm::uvec2(900, 600));
			CHECK_FALSE(state.GetPixelSize(ViewportView::Scene).has_value());
		}

		TEST_CASE("EditorViewportState: camera updates are atomic and do not dirty the scene")
		{
			Test::EditorTestFixture fixture("ViewportCamera");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			EditorContext& editor = fixture.GetEditor();
			const uint64_t revision = editor.GetRevision();
			EditorViewportState& state = editor.GetViewportState();
			ExplicitRenderCamera camera;
			camera.Position = { 3.0f, 5.0f, 8.0f };
			REQUIRE(state.SetCamera(camera).has_value());
			for (int failure = 0; failure < 8; ++failure)
			{
				ExplicitRenderCamera bad = camera;
				if (failure == 0)
					bad.Target = bad.Position;
				if (failure == 1)
					bad.Target = bad.Position + glm::vec3(0.0f, 1.0f, 0.0f);
				if (failure == 2)
					bad.Position.x = std::numeric_limits<float>::infinity();
				if (failure == 3)
					bad.VerticalFov = 180.0f;
				if (failure == 4)
					bad.NearClip = bad.FarClip;
				if (failure == 5)
					bad.OrthographicSize = 0.0f;
				if (failure == 6)
					bad.Projection = static_cast<RenderProjection>(255);
				if (failure == 7)
					bad.ClearColor.z = std::numeric_limits<float>::quiet_NaN();
				CHECK_FALSE(state.SetCamera(bad).has_value());
				CHECK(state.GetCamera().Position == camera.Position);
				CHECK(state.GetCamera().Target == camera.Target);
			}
			CHECK(editor.GetRevision() == revision);
			CHECK_FALSE(editor.IsSceneDirty());
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("EditorViewportState: options persist without a renderer")
		{
			EditorViewportState state;
			state.SetOptions({ .Grid = false, .Gizmos = false, .Colliders = true, .Icons = false, .Wireframe = true });
			CHECK_FALSE(state.GetOptions().Grid);
			CHECK_FALSE(state.GetOptions().Gizmos);
			CHECK(state.GetOptions().Colliders);
			CHECK_FALSE(state.GetOptions().Icons);
			CHECK(state.GetOptions().Wireframe);
			REQUIRE(state.SetDebugView(RenderDebugView::Overdraw).has_value());
			CHECK_FALSE(state.SetDebugView(static_cast<RenderDebugView>(RenderDebugViewCount)).has_value());
			CHECK(state.GetDebugView() == RenderDebugView::Overdraw);
		}

		TEST_CASE("EditorViewportRect: clicks map to the displayed image and exclude letterbox bars")
		{
			const EditorViewportRect rectangle{ .Min = { 100.0f, 200.0f }, .Size = { 400.0f, 200.0f }, .ImageSize = { 1600, 900 } };
			const auto first = ToViewportPixel(rectangle, { 100.0f, 200.0f });
			REQUIRE(first.has_value());
			CHECK(first->X == 0);
			CHECK(first->Y == 0);
			const auto center = ToViewportPixel(rectangle, { 300.0f, 300.0f });
			REQUIRE(center.has_value());
			CHECK(center->X == 800);
			CHECK(center->Y == 450);
			const auto last = ToViewportPixel(rectangle, { 499.99f, 399.99f });
			REQUIRE(last.has_value());
			CHECK(last->X == 1599);
			CHECK(last->Y == 899);
			CHECK_FALSE(ToViewportPixel(rectangle, { 500.0f, 300.0f }).has_value());
			CHECK_FALSE(ToViewportPixel(rectangle, { 300.0f, 400.0f }).has_value());
			CHECK_FALSE(ToViewportPixel(rectangle, { 300.0f, 199.0f }).has_value());
			CHECK_FALSE(ToViewportPixel(rectangle, { 99.0f, 300.0f }).has_value());
		}

		TEST_CASE("EditorViewportRect: zero size and minimized views reject input")
		{
			EditorViewportRect rectangle{ .Size = { 100.0f, 100.0f }, .ImageSize = { 100, 100 } };
			CHECK_FALSE(ToViewportPixel({}, { 0.0f, 0.0f }).has_value());
			CHECK_FALSE(ToViewportPixel(rectangle, { std::numeric_limits<float>::quiet_NaN(), 0.0f }).has_value());
			rectangle.ImageSize = { 0, 0 };
			CHECK_FALSE(ToViewportPixel(rectangle, { 0.0f, 0.0f }).has_value());
			rectangle.ImageSize = { 100, 100 };
			rectangle.Size.y = -1.0f;
			CHECK_FALSE(ToViewportPixel(rectangle, { 0.0f, 0.0f }).has_value());
			rectangle.Size.y = std::numeric_limits<float>::infinity();
			CHECK_FALSE(ToViewportPixel(rectangle, { 0.0f, 0.0f }).has_value());
		}

		TEST_CASE("EditorViewportRect: resizing uses displayed pixels until the next rendered frame")
		{
			EditorViewportRect rectangle{ .Size = { 1280.0f, 720.0f }, .ImageSize = { 640, 360 } };
			const auto old = ToViewportPixel(rectangle, { 640.0f, 360.0f });
			REQUIRE(old.has_value());
			CHECK(old->X == 320);
			CHECK(old->Y == 180);
			rectangle.ImageSize = { 2560, 1440 };
			const auto fresh = ToViewportPixel(rectangle, { 640.0f, 360.0f });
			REQUIRE(fresh.has_value());
			CHECK(fresh->X == 1280);
			CHECK(fresh->Y == 720);
		}

		TEST_CASE("EditorViewportRect: fractional scaling works on a negative desktop origin")
		{
			const EditorViewportRect rectangle{ .Min = { -1280.5f, -40.25f }, .Size = { 800.0f, 600.0f }, .ImageSize = { 1000, 750 } };
			const auto first = ToViewportPixel(rectangle, rectangle.Min);
			REQUIRE(first.has_value());
			CHECK(first->X == 0);
			CHECK(first->Y == 0);
			const auto fractional = ToViewportPixel(rectangle, rectangle.Min + glm::vec2(7.5f, 10.5f));
			REQUIRE(fractional.has_value());
			CHECK(fractional->X == 9);
			CHECK(fractional->Y == 13);
			CHECK_FALSE(ToViewportPixel(rectangle, rectangle.Min - glm::vec2(0.25f, 0.0f)).has_value());
			CHECK_FALSE(ToViewportPixel(rectangle, rectangle.Min + rectangle.Size).has_value());
		}
	}

}
