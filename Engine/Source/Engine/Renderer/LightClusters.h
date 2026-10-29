#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/LightClusterConstants.h"

#include <glm/glm.hpp>

#include <cstdint>

// The cluster grid of the clustered light loop on the CPU (M23, C23.1, R-CLC; Docs/Decisions/0034-m23-contract.md
// decision 5): the grid the Prepare stage writes into LightClusterConstants (Shared/LightClusterConstants.h) for the
// LightClusters pass and the forward passes, and the LightCount debug view's palette. Pure, thread-safe and
// deterministic: the same camera and size give the same grid in every configuration.

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

		bool operator==(const LightClusterGrid&) const = default;
	};

	// The grid of a view of `width` x `height` scene pixels (both >= 1, asserted) seen by `camera`: its tiles cover the
	// view, partial at the right and bottom edges, and its slices the camera's [NearClip, FarClip] range of view depth,
	// exponentially (LightClusterConstants' comment), for both projections. A camera whose clip range is not finite and
	// positive with Near < Far gives one slice spanning everything (SliceScale 0, so every depth is slice 0 after the
	// clamp). Pure.
	[[nodiscard]] LightClusterGrid ComputeLightClusterGrid(const CameraData& camera, uint32_t width, uint32_t height);

	// The cluster of scene pixel (x, y) at view depth `viewDepth` (positive in front of the camera): the index
	// (z * TilesY + y / LightClusterTileSize) * TilesX + x / LightClusterTileSize, z the depth's slice clamped to
	// [0, Slices - 1]. The forward shader computes exactly this (Common/LightLoop.slang). Pure.
	[[nodiscard]] uint32_t GetLightClusterIndex(const LightClusterGrid& grid, uint32_t x, uint32_t y, float viewDepth);

	// The LightCount debug view's colour for a cluster holding `count` lights (display-encoded, like GetOverdrawDebugColor):
	// 0 black, 1 to 2 blue, 3 to 4 cyan, 5 to 8 green, 9 to 16 yellow, 17 to 32 orange (1, 0.5, 0), more red. Pure.
	[[nodiscard]] glm::vec3 GetLightCountDebugColor(uint32_t count);

}
