#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// Spot entry in ShadowConstants, 128 bytes; unused entries zeroed. Indices.x is the uploaded Lights index,
	// not the snapshot index; the light's ShaderLight::ShadowSlot is the entry's index (M29). DepthSoftness = near, far,
	// SourceRadius, maximum filter radius in UV.
	struct SpotShadowConstants
	{
		Float4x4 ViewProjection;
		Float4 UvScaleBias;
		Float4 DepthSoftness;
		Float4 Bias; // DepthBias, NormalBias, texel world scale at unit depth, padding
		UInt4 Indices;
	};

	// The most shadow-casting point and spot lights one view shades with shadows (M23, C23.1, R-PLS; M29,
	// Docs/Decisions/0044-m29-light-limits.md decision 2: 16 of each, from 4 and 8; Renderer/PointShadowMaps.h's
	// MaxPointShadowLights and Renderer/SpotShadowAtlas.h's MaxSpotShadowLights, which equal them). The first
	// MaxFullPointShadows planned point lights' cubes are in PointShadowMaps (t12), the rest in PointShadowMapsHalf (t15).
#if defined(ENGINE_SHADER)
	static const uint MaxPointShadows = 16;
	static const uint MaxSpotShadows = 16;
	static const uint MaxFullPointShadows = 4;
	static const uint PointShadowTierFull = 0;
	static const uint PointShadowTierHalf = 1;
#else
inline constexpr uint32_t MaxPointShadows = 16;
inline constexpr uint32_t MaxSpotShadows = 16;
inline constexpr uint32_t MaxFullPointShadows = 4;
inline constexpr uint32_t PointShadowTierFull = 0;
inline constexpr uint32_t PointShadowTierHalf = 1;
#endif

	// Point entry in ShadowConstants (M23, C23.1, R-PLS; Docs/Decisions/0034-m23-contract.md decision 3), 64 bytes; unused
	// entries zeroed. Its light's six faces are the layers 6 * Indices.y to 6 * Indices.y + 5 of its tier's cube array (M29:
	// Indices.z PointShadowTierFull, PointShadowMaps at t12 of set 0; PointShadowTierHalf, PointShadowMapsHalf at t15, faces of
	// half the size), in Vulkan's cube face order (+X, -X, +Y, -Y, +Z, -Z), each a 90-degree perspective reverse-Z projection
	// from PositionRange.xyz (Renderer/PointShadowMaps.h). PositionRange = world position, Range; DepthSoftness = near, far,
	// SourceRadius, maximum filter radius; Bias = DepthBias, NormalBias, texel world scale at unit depth (its tier's face),
	// padding; Indices.x is the uploaded Lights index, Indices.y the cube's layer group, Indices.z the tier, w padding; the
	// light's ShaderLight::ShadowSlot is the entry's index (M29).
	struct PointShadowConstants
	{
		Float4 PositionRange;
		Float4 DepthSoftness;
		Float4 Bias;
		UInt4 Indices;
	};

	// Set 0 b1, shared by shadow application and debug views. SceneRenderer owns/upload per-view; ShadowPass plans feed it.
	// Reverse-Z blocker depth is converted to light distance before PCSS. Unused cascades, spots and points are zeroed.
	// M23 (C23.1) grew it at its end: Counts.w, padding before, is the point count, and Points follows Counts. M29 (decision 2)
	// grew Spots and Points to 16 entries each, which moves the members after Spots; the shaders read them by name.
	struct ShadowConstants
	{
		Float4x4 CascadeViewProjection[4];
		Float4 CascadeSplits; // far distance of each range in camera view space
		Float4 CascadeBlendStarts;
		Float4 CascadeTexelWorldSizes;
		Float4 CascadeDepthRanges[4]; // near, far, penumbra UV per world metre, max filter radius UV
		SpotShadowConstants Spots[MaxSpotShadows];
		Float4 Directional; // LightAngle radians, DepthBias, NormalBias, ShadowDistance
		UInt4 Counts;       // cascade count, spot count, directional uploaded light index, point count (M23)
		PointShadowConstants Points[MaxPointShadows];
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SpotShadowConstants) == 128);
	static_assert(offsetof(SpotShadowConstants, UvScaleBias) == 64 && offsetof(SpotShadowConstants, Indices) == 112);
	static_assert(sizeof(PointShadowConstants) == 64);
	static_assert(offsetof(PointShadowConstants, DepthSoftness) == 16 && offsetof(PointShadowConstants, Indices) == 48);
	static_assert(sizeof(ShadowConstants) == 3472);
	static_assert(offsetof(ShadowConstants, CascadeSplits) == 256 && offsetof(ShadowConstants, Spots) == 368);
	static_assert(offsetof(ShadowConstants, Directional) == 2416 && offsetof(ShadowConstants, Counts) == 2432);
	static_assert(offsetof(ShadowConstants, Points) == 2448);

}
#endif
