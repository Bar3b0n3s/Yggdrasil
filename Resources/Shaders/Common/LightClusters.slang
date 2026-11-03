// The cluster grid of the clustered light loop in the shaders (M23, R-CLC; Docs/Decisions/0034-m23-contract.md decision 5;
// Shared/LightClusterConstants.h): a pixel's cluster, which Renderer/LightClusters.h's GetLightClusterIndex states on the
// CPU, the depth range of a slice and the LightCount debug view's palette (GetLightCountDebugColor). Included by
// Passes/LightClusters.slang, Common/LightLoop.slang and Passes/SceneDataView.slang.
#pragma once

#include "Shared/LightClusterConstants.h"

// The smallest view depth whose logarithm the slice takes (Renderer/LightClusters.cpp clamps the same way), so a depth at or
// behind the camera lands in slice 0.
static const float MinLightClusterViewDepth = 1e-20;

// The slice of view depth `viewDepth` (positive in front of the camera): floor(log2(viewDepth) * Slicing.z + Slicing.w),
// clamped to [0, Grid.z - 1]; a depth that gives no number lands in slice 0.
uint GetLightClusterSlice(LightClusterConstants clusters, float viewDepth)
{
	const float slice = floor(log2(max(viewDepth, MinLightClusterViewDepth)) * clusters.Slicing.z + clusters.Slicing.w);
	if (!(slice > 0.0))
		return 0;
	const uint last = clusters.Grid.z - 1;
	return slice >= float(last) ? last : uint(slice);
}

// The cluster of scene pixel `pixel` at view depth `viewDepth`: (z * Grid.y + y / LightClusterTileSize) * Grid.x + x /
// LightClusterTileSize, the tile clamped to the grid. Its masks are the LightClusterWordCount words from its index times
// Grid.w.
uint GetLightClusterIndex(LightClusterConstants clusters, uint2 pixel, float viewDepth)
{
	const uint tileX = min(pixel.x / LightClusterTileSize, clusters.Grid.x - 1);
	const uint tileY = min(pixel.y / LightClusterTileSize, clusters.Grid.y - 1);
	return (GetLightClusterSlice(clusters, viewDepth) * clusters.Grid.y + tileY) * clusters.Grid.x + tileX;
}

// The LightCount view's colour of a cluster holding `count` lights: 0 black, 1 to 2 blue, 3 to 4 cyan, 5 to 8 green, 9 to 16
// yellow, 17 to 32 orange, more red.
float3 GetLightCountDebugColor(uint count)
{
	if (count == 0)
		return float3(0.0, 0.0, 0.0);
	if (count <= 2)
		return float3(0.0, 0.0, 1.0);
	if (count <= 4)
		return float3(0.0, 1.0, 1.0);
	if (count <= 8)
		return float3(0.0, 1.0, 0.0);
	if (count <= 16)
		return float3(1.0, 1.0, 0.0);
	if (count <= 32)
		return float3(1.0, 0.5, 0.0);
	return float3(1.0, 0.0, 0.0);
}
