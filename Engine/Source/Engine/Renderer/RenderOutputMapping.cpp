#include "EnginePCH.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cstdint>
#include <optional>

// The output stage's view-to-scene mapping (RenderSnapshot.h; Docs/Decisions/0029-m18-contract.md decisions 6 and 8): pure
// integer arithmetic for the scale and the offsets, IEEE-exact float operations for the UI's point mapping.

namespace Engine {

	namespace Utils {

		// floor(value / 2), rounded toward negative infinity (C++'s division truncates toward zero): -61 gives -31.
		static int64_t FloorHalf(int64_t value)
		{
			return value >= 0 ? value / 2 : -((1 - value) / 2);
		}

		// The view pixel of the scaled scene image's first pixel along one axis: floor((view - scale * scene) / 2).
		static int32_t ComputeOutputOffset(uint32_t view, uint32_t scene, uint32_t scale)
		{
			// |view - scale * scene| < 2^32 whenever the scene fits (scale * scene <= view) or scale is 1, so half of it fits.
			const int64_t leftover = static_cast<int64_t>(view) - static_cast<int64_t>(scale) * static_cast<int64_t>(scene);
			return static_cast<int32_t>(FloorHalf(leftover));
		}

		// The scene pixel along one axis that view pixel `pixel` shows, or none in the letterbox.
		static std::optional<uint32_t> MapViewCoordinate(uint32_t pixel, int32_t offset, uint32_t scale, uint32_t scene)
		{
			const int64_t relative = static_cast<int64_t>(pixel) - offset;
			if (relative < 0)
				return std::nullopt;
			const int64_t scenePixel = relative / scale;
			if (scenePixel >= static_cast<int64_t>(scene))
				return std::nullopt;
			return static_cast<uint32_t>(scenePixel);
		}

	}

	RenderOutputMapping ComputeRenderOutputMapping(uint32_t viewWidth, uint32_t viewHeight, const RenderOutputSettings& settings)
	{
		ENGINE_CORE_ASSERT(viewWidth >= 1 && viewHeight >= 1, "the output mapping needs a view of at least 1x1, got {}x{}", viewWidth, viewHeight);
		RenderOutputMapping mapping;
		mapping.ViewWidth = std::max(viewWidth, 1U);
		mapping.ViewHeight = std::max(viewHeight, 1U);
		mapping.SceneWidth = settings.SceneWidth != 0 ? settings.SceneWidth : mapping.ViewWidth;
		mapping.SceneHeight = settings.SceneHeight != 0 ? settings.SceneHeight : mapping.ViewHeight;
		// The largest integer scale that fits both axes; 1 when the scene does not fit, which centres and crops it.
		const uint32_t fit = std::min(mapping.ViewWidth / mapping.SceneWidth, mapping.ViewHeight / mapping.SceneHeight);
		mapping.Scale = std::max(fit, 1U);
		mapping.OffsetX = Utils::ComputeOutputOffset(mapping.ViewWidth, mapping.SceneWidth, mapping.Scale);
		mapping.OffsetY = Utils::ComputeOutputOffset(mapping.ViewHeight, mapping.SceneHeight, mapping.Scale);
		return mapping;
	}

	std::optional<glm::uvec2> MapViewPixelToScenePixel(const RenderOutputMapping& mapping, uint32_t x, uint32_t y)
	{
		if (x >= mapping.ViewWidth || y >= mapping.ViewHeight || mapping.Scale == 0)
			return std::nullopt;
		const std::optional<uint32_t> sceneX = Utils::MapViewCoordinate(x, mapping.OffsetX, mapping.Scale, mapping.SceneWidth);
		const std::optional<uint32_t> sceneY = Utils::MapViewCoordinate(y, mapping.OffsetY, mapping.Scale, mapping.SceneHeight);
		if (!sceneX || !sceneY)
			return std::nullopt;
		return glm::uvec2(*sceneX, *sceneY);
	}

	glm::vec2 MapViewPointToScenePoint(const RenderOutputMapping& mapping, glm::vec2 viewPoint)
	{
		ENGINE_CORE_ASSERT(mapping.Scale >= 1, "an output mapping has a scale of at least 1");
		// One subtraction and one division per axis, each correctly rounded (IEEE-exact for the integers of a mapping).
		const glm::vec2 offset(static_cast<float>(mapping.OffsetX), static_cast<float>(mapping.OffsetY));
		return (viewPoint - offset) / static_cast<float>(std::max(mapping.Scale, 1U));
	}

	Result<std::optional<RenderRayInterval>> ComputeOutputPixelRayInterval(const CameraData& camera, const RenderOutputMapping& mapping, uint32_t x,
		uint32_t y)
	{
		// The view pixel is mapped to its scene pixel exactly once, and the camera's viewport is the scene resolution, as the
		// renderer sets it (SceneRenderer.cpp's Prepare stage), whatever viewport its producer gave it.
		const std::optional<glm::uvec2> scenePixel = MapViewPixelToScenePixel(mapping, x, y);
		if (!scenePixel)
			return std::optional<RenderRayInterval>{};
		CameraData sceneCamera = camera;
		sceneCamera.ViewportWidth = mapping.SceneWidth;
		sceneCamera.ViewportHeight = mapping.SceneHeight;
		ENGINE_TRY_ASSIGN(const RenderRayInterval ray, ComputeViewPixelRayInterval(sceneCamera, scenePixel->x, scenePixel->y));
		return std::optional<RenderRayInterval>(ray);
	}

}
