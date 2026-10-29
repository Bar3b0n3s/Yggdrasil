#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The cluster grid of the clustered light loop (M23, C23.1, R-CLC; Docs/Decisions/0034-m23-contract.md decision 5).
	// The view is divided into LightClusterTileSize x LightClusterTileSize tiles of scene pixels (the last row and column
	// partial) times LightClusterDepthSlices slices of view depth, and each cluster holds one bit per entry of the view's
	// light list (Shared/ShaderLight.h, at most MaxVisibleLights = 256 entries, so LightClusterWordCount 32-bit words): bit
	// b of word w is set when light 32 w + b may reach the cluster. The LightClusters pass (the end of the Ambient stage)
	// writes the masks, one thread per cluster and word, each the only writer of its word, so no atomic decides a bit; the
	// forward pass reads its pixel's cluster and shades its set bits in ascending order, which is the light list's order,
	// so the sum is the brute-force loop's over every light whose contribution is not exactly zero.
#if defined(ENGINE_SHADER)
	static const uint LightClusterTileSize = 64;
	static const uint LightClusterDepthSlices = 24;
	static const uint LightClusterWordCount = 8;
	static const uint LightClusterModeBruteForce = 0;
	static const uint LightClusterModeClustered = 1;
#else
inline constexpr uint32_t LightClusterTileSize = 64;
inline constexpr uint32_t LightClusterDepthSlices = 24;
inline constexpr uint32_t LightClusterWordCount = 8;
inline constexpr uint32_t LightClusterModeBruteForce = 0;
inline constexpr uint32_t LightClusterModeClustered = 1;
#endif

	// The grid of one view: b3 of descriptor set 0 for the forward passes and the constants of the LightClusters pass. The
	// cluster masks are a structured buffer of uint (t11 of set 0, the pass's u0), cluster (x, y, z) at word offset
	// ((z * Grid.y + y) * Grid.x + x) * Grid.w. Slicing: slice z covers view depths [Near * Ratio^z, Near * Ratio^(z+1)),
	// Ratio = (Far / Near)^(1 / LightClusterDepthSlices), the last slice open towards the far side; Slicing.z and .w are the
	// slice of a view depth d as floor(log2(d) * Slicing.z + Slicing.w), clamped to the grid. Mode.x is
	// LightClusterModeClustered for the clustered loop, or LightClusterModeBruteForce for the loop over every light of the
	// list (the reference the clustered render equals, RenderQualitySettings::ClusteredLights false).
	struct LightClusterConstants
	{
		UInt4 Grid;     // tiles in x, tiles in y, LightClusterDepthSlices, LightClusterWordCount
		Float4 Slicing; // Near, Far, slice scale, slice bias
		UInt4 Mode;     // x: LightClusterModeClustered or LightClusterModeBruteForce; yzw padding
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(LightClusterConstants) == 48, "LightClusterConstants must match its 48-byte constant-buffer layout");
	static_assert(offsetof(LightClusterConstants, Slicing) == 16 && offsetof(LightClusterConstants, Mode) == 32,
		"LightClusterConstants members must sit where the constant-buffer layout puts them");

}
#endif
