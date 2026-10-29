#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

	// The largest reference resolution side of a PixelPerfectCamera, in pixels: the largest view side the editor accepts
	// (EditorViewportState::SetGameResolution).
	inline constexpr uint32_t MaxPixelPerfectReferenceSide = 8192;

	// Registry name "PixelPerfectCamera" (Architecture §5.3; M21, Docs/Decisions/0032-m21-contract.md decision 6): renders its
	// entity's Camera at a fixed reference resolution and upscales it by the largest integer factor that fits the view, with
	// point sampling and a letterbox, through M18's output stage (RenderOutputSettings; Docs/Decisions/0029-m18-contract.md
	// decisions 5 to 8). Requires Camera. Render extraction (Scene/PixelPerfectCamera.h) turns it into the snapshot's Output
	// (SceneWidth x SceneHeight = ReferenceWidth x ReferenceHeight, PixelArt, LetterboxColor) and an orthographic projection
	// whose half height is ReferenceHeight / (2 * PixelsPerUnit) world units, whatever the Camera's own Projection says, with
	// the camera's world position snapped to the pixel grid (multiples of 1 / PixelsPerUnit in X and Y), so a sprite of the
	// same PixelsPerUnit maps each texel to exactly one scene pixel. The integer scale is the output mapping's
	// (ComputeRenderOutputMapping), never a field. LetterboxColor is linear.
	struct PixelPerfectCameraComponent
	{
		uint32_t ReferenceWidth = 320;
		uint32_t ReferenceHeight = 180;
		float PixelsPerUnit = 16.0f;
		glm::vec3 LetterboxColor = glm::vec3(0.0f);
	};

}
