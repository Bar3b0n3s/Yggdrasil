#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/LightClusterConstants.h"

#include <glm/glm.hpp>

#include <cstdint>

// The cluster grid of the clustered light loop on the CPU (M23, C23.1, R-CLC; Docs/Decisions/0034-m23-contract.md
// decision 5): the grid the Ambient stage writes into LightClusterConstants (Shared/LightClusterConstants.h) for the
// LightClusters pass, the forward passes and the LightCount view (Renderer/LightClusterPass.h), and the LightCount debug
// view's palette. Pure, thread-safe and deterministic: the same camera and size give the same grid in every configuration
// (the slicing's logarithms are DetMath's).

namespace Engine {

	// One view's cluster grid (LightClusterConstants without the GPU layout).
	struct LightClusterGrid
	{
		uint32_t TilesX = 0; // ceil(scene width / LightClusterTileSize)
		uint32_t TilesY = 0; // ceil(scene height / LightClusterTileSize)
		uint32_t Slices = LightClusterDepthSlices;
		float Near = 0.1f; // the camera's NearClip: slice 0 starts here
		float Far = 1.0f;  // the camera's FarClip: the last slice ends here (and stays open beyond it)
		// floor(log2(viewDepth) * SliceScale + SliceBias) is a view depth's slice (before clamping to [0, Slices - 1]):
		// SliceScale = Slices / log2(Far / Near), SliceBias = -log2(Near) * SliceScale.
		float SliceScale = 0.0f;
		float SliceBias = 0.0f;
		// M29 (Docs/Decisions/0044-m29-light-limits.md decision 1): the 32-bit mask words of each cluster, the view's shaded-light
		// cap over 32 (8 at the default 256, MaxLightClusterWordCount at 2048), LightClusterConstants::Grid.w.
		uint32_t WordCount = DefaultMaxShadedLights / 32;

		bool operator==(const LightClusterGrid&) const = default;
	};

	// The grid of a view of `width` x `height` scene pixels (both >= 1, asserted) seen by `camera`: its tiles cover the
	// view, partial at the right and bottom edges, and its slices the camera's [NearClip, FarClip] range of view depth,
	// exponentially (LightClusterConstants' comment), for both projections. A camera whose clip range is not finite and
	// positive with Near < Far gives one slice spanning everything (SliceScale 0, so every depth is slice 0 after the
	// clamp). M29: its WordCount holds `maxShadedLights` (IsValidMaxShadedLights, asserted) lights' bits. Pure.
	[[nodiscard]] LightClusterGrid ComputeLightClusterGrid(const CameraData& camera, uint32_t width, uint32_t height,
		uint32_t maxShadedLights = DefaultMaxShadedLights);

	// The cluster of scene pixel (x, y) at view depth `viewDepth` (positive in front of the camera): the index
	// (z * TilesY + y / LightClusterTileSize) * TilesX + x / LightClusterTileSize, z the depth's slice clamped to
	// [0, Slices - 1]. The forward shader computes the same formula (Common/LightLoop.slang), but in float with the GPU's
	// log2, where this takes the logarithm in double (DetMath::Log): the two agree away from slice boundaries, and next to
	// one they may pick adjacent slices, which the LightClusters pass's depth margin covers (it widens each cluster's depth
	// range by 0.1 %, so a light near a boundary is in both slices' clusters). Pure.
	[[nodiscard]] uint32_t GetLightClusterIndex(const LightClusterGrid& grid, uint32_t x, uint32_t y, float viewDepth);

	// The LightCount debug view's colour for a cluster holding `count` lights (display-encoded, like GetOverdrawDebugColor):
	// 0 black, 1 to 2 blue, 3 to 4 cyan, 5 to 8 green, 9 to 16 yellow, 17 to 32 orange (1, 0.5, 0), more red. Pure.
	[[nodiscard]] glm::vec3 GetLightCountDebugColor(uint32_t count);

}
