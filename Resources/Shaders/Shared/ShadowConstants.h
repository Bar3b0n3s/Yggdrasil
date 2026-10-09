#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// Spot entry in ShadowConstants, 128 bytes; unused entries zeroed. Indices.x is the uploaded Lights index,
	// not the snapshot index. DepthSoftness = near, far, SourceRadius, maximum filter radius in UV.
	struct SpotShadowConstants
	{
		Float4x4 ViewProjection;
		Float4 UvScaleBias;
		Float4 DepthSoftness;
		Float4 Bias; // DepthBias, NormalBias, texel world scale at unit depth, padding
		UInt4 Indices;
	};

	// Set 0 b1, shared by shadow application and debug views. SceneRenderer owns/upload per-view; ShadowPass plans feed it.
	// Reverse-Z blocker depth is converted to light distance before PCSS. Unused cascades and spots are zeroed.
	struct ShadowConstants
	{
		Float4x4 CascadeViewProjection[4];
		Float4 CascadeSplits; // far distance of each range in camera view space
		Float4 CascadeBlendStarts;
		Float4 CascadeTexelWorldSizes;
		Float4 CascadeDepthRanges[4]; // near, far, penumbra UV per world metre, max filter radius UV
		SpotShadowConstants Spots[8];
		Float4 Directional; // LightAngle radians, DepthBias, NormalBias, ShadowDistance
		UInt4 Counts;       // cascade count, spot count, directional uploaded light index, padding
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SpotShadowConstants) == 128);
	static_assert(offsetof(SpotShadowConstants, UvScaleBias) == 64 && offsetof(SpotShadowConstants, Indices) == 112);
	static_assert(sizeof(ShadowConstants) == 1424);
	static_assert(offsetof(ShadowConstants, CascadeSplits) == 256 && offsetof(ShadowConstants, Spots) == 368);
	static_assert(offsetof(ShadowConstants, Directional) == 1392 && offsetof(ShadowConstants, Counts) == 1408);

}
#endif
