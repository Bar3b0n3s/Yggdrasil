#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Engine {

	inline constexpr uint32_t SpotShadowAtlasSize = 4096;
	inline constexpr uint32_t SpotShadowTileSize = 1024;
	// M29 (Docs/Decisions/0044-m29-light-limits.md decision 2): the atlas's 16 tiles (from M23's 8), tile slot t at
	// (t % 4, t / 4) * SpotShadowTileSize; Shared/ShadowConstants.h's MaxSpotShadows equals it.
	inline constexpr uint32_t MaxSpotShadowLights = 16;
	static_assert(MaxSpotShadowLights == (SpotShadowAtlasSize / SpotShadowTileSize) * (SpotShadowAtlasSize / SpotShadowTileSize));
	inline constexpr uint32_t SpotShadowGuardTexels = 2;
	inline constexpr std::string_view RenderSpotShadowBudgetCode = "RENDER_SPOT_SHADOW_BUDGET";

	struct SpotShadowTile
	{
		uint32_t LightIndex = 0;
		UUID Entity{};     // M29: the light's entity, which the next render's hysteresis matches
		uint32_t Slot = 0; // M29: the tile slot, < MaxSpotShadowLights; X and Y follow it
		uint32_t X = 0;    // outer tile origin, framebuffer pixels
		uint32_t Y = 0;
		uint32_t Size = SpotShadowTileSize;
		glm::vec4 UvScaleBias = glm::vec4(0.0f); // interior scale.xy, offset.zw, guard excluded
		glm::mat4 ViewProjection = glm::mat4(1.0f);
		float NearClip = 0.01f;
		float FarClip = 0.0f;
	};

	struct SpotShadowAtlas
	{
		std::vector<SpotShadowTile> Tiles{}; // at most MaxSpotShadowLights, descending importance then UUID; distinct slots
		uint32_t Dropped = 0;                // excess visible shadow-casting spots; host warns once per view under the code above
		uint32_t Rejected = 0;               // visible shadow-casting spots whose data cannot be shadowed; still lit, unshadowed
	};

	// Pure/thread-safe. Takes indices from CullLights.Visible, filters shadowed spots, uses CullLights' importance,
	// tie by UUID then snapshot index. M29 (decisions 2 and 3): at most MaxSpotShadowLights tiles; when more compete, a spot
	// that `previous` (the view's previous atlas, null after a reset) shadowed ranks with its importance times
	// LightHysteresis. A spot `previous` shadowed keeps its slot; the others take the lowest free slots in importance order,
	// so without `previous` the slots are row-major in importance order, as M23 placed them. No point-light shadows.
	// Perspective reverse-Z maps the cone; filter reads clamp to the interior, never the adjacent tile.
	// A candidate whose light data cannot be shadowed (non-finite values, a cone outside (0, 90) degrees or Inner > Outer,
	// an unrepresentable projection) is left out and counted in Rejected; it never costs the other spots their shadows.
	// InvalidArgument only for out-of-range/duplicate indices or an invalid camera.
	[[nodiscard]] Result<SpotShadowAtlas> AllocateSpotShadowAtlas(std::span<const LightData> lights,
		std::span<const uint32_t> visibleLightIndices, const CameraData& camera, const SpotShadowAtlas* previous = nullptr);

}
