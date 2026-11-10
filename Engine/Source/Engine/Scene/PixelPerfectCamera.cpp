#include "EnginePCH.h"
#include "Engine/Scene/PixelPerfectCamera.h"

#include "Engine/Core/Assert.h"

#include <cmath>

// Simulation path (§4.12): the snap is + - * / and std::round (exact), in double, so a view is bit-identical in every
// configuration.

namespace Engine {

	namespace Utils {

		// `coordinate` moved so that `coordinate - halfExtent`, the view's left or bottom edge, is a whole number of pixels of
		// 1 / `pixelsPerUnit` units (round half away from zero). A non-finite coordinate stays non-finite, and the renderer
		// refuses a camera at it.
		static float SnapToPixelGrid(float coordinate, float halfExtent, double pixelsPerUnit)
		{
			const double edgePixels = std::round((static_cast<double>(coordinate) - halfExtent) * pixelsPerUnit);
			return static_cast<float>(edgePixels / pixelsPerUnit + halfExtent);
		}

	}

	PixelPerfectView ComputePixelPerfectView(const PixelPerfectCameraComponent& camera, const glm::vec3& position)
	{
		ENGINE_CORE_ASSERT(camera.ReferenceWidth >= 1 && camera.ReferenceWidth <= MaxPixelPerfectReferenceSide && camera.ReferenceHeight >= 1
				&& camera.ReferenceHeight <= MaxPixelPerfectReferenceSide,
			"a PixelPerfectCamera's reference resolution is 1 to {} pixels per side, got {} x {}", MaxPixelPerfectReferenceSide, camera.ReferenceWidth,
			camera.ReferenceHeight);
		ENGINE_CORE_ASSERT(std::isfinite(camera.PixelsPerUnit) && camera.PixelsPerUnit > 0.0f, "a PixelPerfectCamera's PixelsPerUnit is positive, got {}",
			camera.PixelsPerUnit);

		PixelPerfectView view;
		view.Output = RenderOutputSettings{ .SceneWidth = camera.ReferenceWidth,
			.SceneHeight = camera.ReferenceHeight,
			.PixelArt = true,
			.LetterboxColor = camera.LetterboxColor };
		// The half extents of the view in world units, as the orthographic projection takes them (a float half height).
		view.OrthographicSize = static_cast<float>(camera.ReferenceHeight) / (2.0f * camera.PixelsPerUnit);
		const float halfWidth = static_cast<float>(camera.ReferenceWidth) / (2.0f * camera.PixelsPerUnit);
		const double pixelsPerUnit = camera.PixelsPerUnit;
		view.Position = glm::vec3(Utils::SnapToPixelGrid(position.x, halfWidth, pixelsPerUnit),
			Utils::SnapToPixelGrid(position.y, view.OrthographicSize, pixelsPerUnit), position.z);
		return view;
	}

}
