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

	// The most shadow-casting point lights one view shades with shadows (M23, C23.1, R-PLS; Renderer/PointShadowMaps.h's
	// MaxPointShadowLights, which equals it).
#if defined(ENGINE_SHADER)
	static const uint MaxPointShadows = 4;
#else
inline constexpr uint32_t MaxPointShadows = 4;
#endif

	// Point entry in ShadowConstants (M23, C23.1, R-PLS; Docs/Decisions/0034-m23-contract.md decision 3), 64 bytes; unused
	// entries zeroed. Its light's six faces are the layers 6 * Indices.y to 6 * Indices.y + 5 of the PointShadowMaps cube
	// array (t12 of set 0), in Vulkan's cube face order (+X, -X, +Y, -Y, +Z, -Z), each a 90-degree perspective reverse-Z
	// projection from PositionRange.xyz (Renderer/PointShadowMaps.h). PositionRange = world position, Range;
	// DepthSoftness = near, far, SourceRadius, maximum filter radius; Bias = DepthBias, NormalBias, texel world scale at
	// unit depth, padding; Indices.x is the uploaded Lights index, Indices.y the cube's layer group, zw padding.
	struct PointShadowConstants
	{
		Float4 PositionRange;
		Float4 DepthSoftness;
		Float4 Bias;
		UInt4 Indices;
	};

	// Set 0 b1, shared by shadow application and debug views. SceneRenderer owns/upload per-view; ShadowPass plans feed it.
	// Reverse-Z blocker depth is converted to light distance before PCSS. Unused cascades, spots and points are zeroed.
	// M23 (C23.1) grew it at its end: Counts.w, padding before, is the point count, and Points follows Counts, so every
	// earlier member keeps its offset.
	struct ShadowConstants
	{
		Float4x4 CascadeViewProjection[4];
		Float4 CascadeSplits; // far distance of each range in camera view space
		Float4 CascadeBlendStarts;
		Float4 CascadeTexelWorldSizes;
		Float4 CascadeDepthRanges[4]; // near, far, penumbra UV per world metre, max filter radius UV
		SpotShadowConstants Spots[8];
		Float4 Directional; // LightAngle radians, DepthBias, NormalBias, ShadowDistance
		UInt4 Counts;       // cascade count, spot count, directional uploaded light index, point count (M23)
		PointShadowConstants Points[MaxPointShadows];
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SpotShadowConstants) == 128);
	static_assert(offsetof(SpotShadowConstants, UvScaleBias) == 64 && offsetof(SpotShadowConstants, Indices) == 112);
	static_assert(sizeof(PointShadowConstants) == 64);
	static_assert(offsetof(PointShadowConstants, DepthSoftness) == 16 && offsetof(PointShadowConstants, Indices) == 48);
	static_assert(sizeof(ShadowConstants) == 1680);
	static_assert(offsetof(ShadowConstants, CascadeSplits) == 256 && offsetof(ShadowConstants, Spots) == 368);
	static_assert(offsetof(ShadowConstants, Directional) == 1392 && offsetof(ShadowConstants, Counts) == 1408);
	static_assert(offsetof(ShadowConstants, Points) == 1424);

}
#endif
