#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>

// World canvases' placement (M22, C22.3, R-UI-WORLD; Docs/Decisions/0033-m22-contract.md decision 49): the part of
// UiCanvasLayout::World (Scene/UiLayout.h) that does not depend on the canvas entity's pose, shared by the layout, which
// places the canvas at the entity's simulated world matrix, and render extraction (Scene/UiExtraction.h), which places it at
// the entity's render pose. Only IEEE-exact operations (Scene is on the simulation path's lint scope, §4.12).

namespace Engine {

	namespace Utils {

		// A World canvas's size in canvas pixels from its UiCanvas fields: each at least 1, so a size a script or a scene
		// wrote out of the registered range still lays out (the registry refuses 0).
		[[nodiscard]] inline glm::uvec2 GetUiWorldCanvasSize(uint32_t width, uint32_t height)
		{
			return glm::uvec2(std::max(width, 1U), std::max(height, 1U));
		}

		// The matrix from a World canvas's pixels (x right, y down, origin at its top-left corner, `size` wide and high) to
		// its entity's local space: translate(-size / 2), then the y flip, then 1 / `pixelsPerUnit` on x and y, so the canvas
		// lies in the local XY plane centred on the origin with canvas y down along local -Y, facing +Z (z unchanged). The
		// canvas's world matrix is the entity's world matrix times this one.
		[[nodiscard]] inline glm::mat4 MakeUiWorldCanvasLocalMatrix(glm::uvec2 size, float pixelsPerUnit)
		{
			const float unit = 1.0f / pixelsPerUnit;
			glm::mat4 local(1.0f);
			local[0][0] = unit;
			local[1][1] = -unit;
			local[3][0] = -(static_cast<float>(size.x) * 0.5f) * unit;
			local[3][1] = (static_cast<float>(size.y) * 0.5f) * unit;
			return local;
		}

	}

}
