#pragma once

#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <optional>

// The Runtime's game cursor (Session/PlaySession.h, view coordinates; Docs/Decisions/0032-m21-contract.md, W21.1's
// integration and review): where a native cursor position in the Runtime's window lands in its play session's view. A pure
// function of RuntimeCore, which RuntimeApp::OnEvent calls and the unit tests compile, so the Runtime's mapping is tested
// without a window.

namespace Engine {

	// The view point of the native cursor at `windowPosition` in a window of `windowSize` (window coordinates) that shows the
	// session's game image over its whole area at `imageSize` pixels (the framebuffer). Live play's view is that image
	// (`fixedViewSize` nullopt), so the mapping is the framebuffer's scale. A deterministic session keeps its fixed view
	// (`fixedViewSize`: a recording or a replay the automation subset started), which is reached through both output
	// mappings of `output`, the session's last extraction's output settings (MapImageToViewCoordinates): under a
	// PixelPerfectCamera the image and the view show the reference resolution at different integer scales and letterboxes,
	// and the cursor names the scene pixel the window shows; without one it is the plain stretch. Linear and unclamped;
	// nullopt as MapImageToViewCoordinates (a non-finite position, a window, image or view without area). Pure.
	[[nodiscard]] std::optional<glm::vec2> MapRuntimeCursorToView(const glm::vec2& windowPosition, const glm::vec2& windowSize, const glm::uvec2& imageSize,
		const std::optional<glm::uvec2>& fixedViewSize, const RenderOutputSettings& output);

}
