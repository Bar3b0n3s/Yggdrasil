#include "TestsPCH.h"

#include "Runtime/RuntimeCursor.h"

#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <optional>

// The Runtime's game cursor (Runtime/RuntimeCursor.h; Docs/Decisions/0032-m21-contract.md, W21.1's integration and review):
// the regression test of the Runtime half of the fixed-view cursor fix, whose editor half "EditorPlayController: under a
// PixelPerfectCamera a fixed-view session takes a live cursor at the scene pixel the Game image shows" covers.

namespace Engine {

	TEST_SUITE("Runtime")
	{
		TEST_CASE("RuntimeCursor: a recording resumed in the Runtime under a PixelPerfectCamera maps the cursor through the output mapping")
		{
			// A PixelPerfectCamera's 100x60 reference resolution fills the window's 800x450 framebuffer at scale 7 from offset
			// (50, 15), and the recording's fixed 320x180 view at scale 3 from offset (10, 0): framebuffer point (123.5, 158.5),
			// scene point (10.5, 20.5), is view point (41.5, 61.5), where a stretch would give (49.4, 63.4). In a window of 400x225
			// logical pixels over that framebuffer (a display scale of 2), the cursor point (61.75, 79.25) is that framebuffer
			// point.
			const RenderOutputSettings pixelArt{ .SceneWidth = 100, .SceneHeight = 60, .PixelArt = true };
			const glm::vec2 window(800.0f, 450.0f);
			CHECK(MapRuntimeCursorToView({ 123.5f, 158.5f }, window, { 800, 450 }, glm::uvec2(320, 180), pixelArt) == glm::vec2(41.5f, 61.5f));
			CHECK(MapRuntimeCursorToView({ 61.75f, 79.25f }, { 400.0f, 225.0f }, { 800, 450 }, glm::uvec2(320, 180), pixelArt) == glm::vec2(41.5f, 61.5f));
			// Live play's view is the framebuffer image itself: the framebuffer's scale, whatever the output settings.
			CHECK(MapRuntimeCursorToView({ 61.75f, 79.25f }, { 400.0f, 225.0f }, { 800, 450 }, std::nullopt, pixelArt) == glm::vec2(123.5f, 158.5f));
			// Without a PixelPerfectCamera each scene is its view, and a fixed view is reached by the plain stretch.
			CHECK(MapRuntimeCursorToView({ 400.0f, 225.0f }, window, { 1600, 900 }, glm::uvec2(320, 180), RenderOutputSettings{}) == glm::vec2(160.0f, 90.0f));
			// A window without area maps nothing.
			CHECK_FALSE(MapRuntimeCursorToView({ 1.0f, 1.0f }, glm::vec2(0.0f, 450.0f), { 1600, 900 }, glm::uvec2(320, 180), pixelArt).has_value());
		}
	}

}
