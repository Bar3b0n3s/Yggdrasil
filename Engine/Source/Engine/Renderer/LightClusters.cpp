#include "EnginePCH.h"
#include "Engine/Renderer/LightClusters.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"

#include <algorithm>
#include <cmath>
#include <limits>

// The CPU side of the cluster grid (LightClusters.h; Docs/Decisions/0034-m23-contract.md decision 5). The slicing goes
// through DetMath's logarithm, so a camera gives the same grid in Debug, Release and Dist whatever the C runtime does.

namespace Engine {

	namespace Utils {

		constexpr double LightClusterLn2 = 0.69314718055994530941723212145818;

		// The smallest view depth whose logarithm the slice computation takes: a depth at or behind the camera lands in
		// slice 0 instead of turning the slice into an infinity or NaN. Common/LightClusters.slang clamps the same way.
		constexpr float MinLightClusterViewDepth = 1e-20f;

		[[nodiscard]] static uint32_t GetTileCount(uint32_t pixels)
		{
			return (pixels + LightClusterTileSize - 1) / LightClusterTileSize;
		}

		[[nodiscard]] static double Log2(double value)
		{
			return DetMath::Log(value) / LightClusterLn2;
		}

	}

	LightClusterGrid ComputeLightClusterGrid(const CameraData& camera, uint32_t width, uint32_t height, uint32_t maxShadedLights)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "ComputeLightClusterGrid needs a view of at least 1x1, got {}x{}", width, height);
		ENGINE_CORE_ASSERT(IsValidMaxShadedLights(maxShadedLights), "{} is not a shaded-light cap", maxShadedLights);
		LightClusterGrid grid;
		grid.WordCount = maxShadedLights / 32;
		grid.TilesX = Utils::GetTileCount(std::max(width, 1U));
		grid.TilesY = Utils::GetTileCount(std::max(height, 1U));
		grid.Slices = LightClusterDepthSlices;
		const float nearClip = camera.NearClip;
		const float farClip = camera.FarClip;
		const double ratio = static_cast<double>(farClip) / static_cast<double>(nearClip);
		if (!std::isfinite(nearClip) || !std::isfinite(farClip) || !(nearClip > 0.0f) || !(nearClip < farClip) || !std::isfinite(ratio))
		{
			// One slice spanning every depth: SliceScale 0 makes every depth slice 0 after the clamp.
			grid.Near = 0.0f;
			grid.Far = std::numeric_limits<float>::infinity();
			grid.SliceScale = 0.0f;
			grid.SliceBias = 0.0f;
			return grid;
		}
		grid.Near = nearClip;
		grid.Far = farClip;
		const double scale = static_cast<double>(grid.Slices) / Utils::Log2(ratio);
		grid.SliceScale = static_cast<float>(scale);
		grid.SliceBias = static_cast<float>(-Utils::Log2(static_cast<double>(nearClip)) * scale);
		return grid;
	}

	uint32_t GetLightClusterIndex(const LightClusterGrid& grid, uint32_t x, uint32_t y, float viewDepth)
	{
		const uint32_t tilesX = std::max(grid.TilesX, 1U);
		const uint32_t tilesY = std::max(grid.TilesY, 1U);
		const uint32_t slices = std::max(grid.Slices, 1U);
		const uint32_t tileX = std::min(x / LightClusterTileSize, tilesX - 1);
		const uint32_t tileY = std::min(y / LightClusterTileSize, tilesY - 1);
		// A depth that is not a number stays NaN through std::max (which returns its first argument unless it is less than the
		// second) and the logarithm, and lands in slice 0 below.
		const float depth = std::max(viewDepth, Utils::MinLightClusterViewDepth);
		const float logarithm = static_cast<float>(Utils::Log2(static_cast<double>(depth)));
		const float slice = std::floor(logarithm * grid.SliceScale + grid.SliceBias);
		uint32_t z = 0;
		if (slice > 0.0f)
			z = slice >= static_cast<float>(slices - 1) ? slices - 1 : static_cast<uint32_t>(slice);
		return (z * tilesY + tileY) * tilesX + tileX;
	}

	glm::vec3 GetLightCountDebugColor(uint32_t count)
	{
		if (count == 0)
			return glm::vec3(0.0f);
		if (count <= 2)
			return glm::vec3(0.0f, 0.0f, 1.0f);
		if (count <= 4)
			return glm::vec3(0.0f, 1.0f, 1.0f);
		if (count <= 8)
			return glm::vec3(0.0f, 1.0f, 0.0f);
		if (count <= 16)
			return glm::vec3(1.0f, 1.0f, 0.0f);
		if (count <= 32)
			return glm::vec3(1.0f, 0.5f, 0.0f);
		return glm::vec3(1.0f, 0.0f, 0.0f);
	}

}
