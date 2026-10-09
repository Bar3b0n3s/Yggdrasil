#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Engine {

	inline constexpr uint32_t SpotShadowAtlasSize = 4096;
	inline constexpr uint32_t SpotShadowTileSize = 1024;
	inline constexpr uint32_t MaxSpotShadowLights = 8;
	inline constexpr uint32_t SpotShadowGuardTexels = 2;
	inline constexpr std::string_view RenderSpotShadowBudgetCode = "RENDER_SPOT_SHADOW_BUDGET";

	struct SpotShadowTile
	{
		uint32_t LightIndex = 0;
		uint32_t X = 0; // outer tile origin, framebuffer pixels
		uint32_t Y = 0;
		uint32_t Size = SpotShadowTileSize;
		glm::vec4 UvScaleBias = glm::vec4(0.0f); // interior scale.xy, offset.zw, guard excluded
		glm::mat4 ViewProjection = glm::mat4(1.0f);
		float NearClip = 0.01f;
		float FarClip = 0.0f;
	};

	struct SpotShadowAtlas
	{
		std::vector<SpotShadowTile> Tiles{}; // at most 8, descending importance then UUID
		uint32_t Dropped = 0;                // excess visible shadow-casting spots; host warns once per view under the code above
	};

	// Pure/thread-safe. Takes indices from CullLights.Visible, filters shadowed spots, uses CullLights' importance,
	// tie by UUID then snapshot index. Tiles in row-major order, no temporal allocation cache or point-light shadows.
	// Perspective reverse-Z maps the cone; filter reads clamp to the interior, never the adjacent tile.
	// InvalidArgument for out-of-range/duplicate indices, invalid camera or candidate light data.
	[[nodiscard]] Result<SpotShadowAtlas> AllocateSpotShadowAtlas(std::span<const LightData> lights,
		std::span<const uint32_t> visibleLightIndices, const CameraData& camera);

}
