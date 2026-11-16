// LightClusters: the cluster masks of the clustered light loop (M23, R-CLC; Docs/Decisions/0034-m23-contract.md decision 5;
// Renderer/LightClusterPass.h). One thread per cluster and mask word: x runs over every tile's Grid.w words of
// one slice, y over the slices. Each thread tests the 32 lights of its word against its cluster's view-space bounds and
// writes its word alone, so no atomic decides a bit. The test is conservative, so every light that lights a point of the
// cluster in the forward pass (its windowed falloff is 0 at and beyond Range, its cone factor 0 outside the outer cone) is
// in the mask:
//   - the cluster is its tile widened by a pixel on every side, times its slice's view depths widened by a thousandth at
//     either end (slice 0 from depth 0 and the last slice open beyond the far clip, as Common/LightClusters.slang clamps a
//     pixel's slice), which covers the pixel centres' rays and the slice the forward pass computes for a depth;
//   - a point or spot light is its sphere of Range, widened by a hundredth of it and by the rounding of the world positions
//     the forward pass subtracts, intersected with the cluster's view-space box over the depths both share;
//   - a directional light, or one whose range has no finite inverse square, is in every cluster.

#include "Shared/LightClusterConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ViewConstants.h"
#include "Common/LightClusters.slang"

ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<LightClusterConstants> Clusters : register(b1, space0);
StructuredBuffer<ShaderLight> Lights : register(t0, space0);
RWStructuredBuffer<uint> Masks : register(u0, space0);

static const float LightClusterInfinity = 3.402823466e+38;

// The view-space bounds of one cluster: the tile's extent per unit of view depth (perspective) or in view units
// (orthographic), and its depth range.
struct ClusterBounds
{
	float2 LateralMin;
	float2 LateralMax;
	float NearDepth;
	float FarDepth;
	bool Perspective;
};

// The view-space point NDC (x, y) unprojects to: on the near plane for a perspective projection (reverse-Z depth 1), and
// in the middle of the depth range for an orthographic one, whose x and y do not depend on it.
float3 UnprojectNdc(float2 ndc, bool perspective)
{
	const float4 position = mul(View.InverseProjection, float4(ndc, perspective ? 1.0 : 0.5, 1.0));
	return position.xyz / position.w;
}

ClusterBounds GetClusterBounds(uint tile, uint slice)
{
	const bool perspective = View.ProjectionKind != ProjectionKindOrthographic;
	const uint2 tileXY = uint2(tile % Clusters.Grid.x, tile / Clusters.Grid.x);
	const float2 pixelMin = float2(tileXY * LightClusterTileSize) - 1.0;
	const float2 pixelMax = min(float2((tileXY + 1) * LightClusterTileSize), View.ViewportSize) + 1.0;
	// NVRHI's flipped viewport puts NDC y = +1 on pixel row 0.
	const float2 ndcMin = pixelMin * View.InverseViewportSize * float2(2.0, -2.0) + float2(-1.0, 1.0);
	const float2 ndcMax = pixelMax * View.InverseViewportSize * float2(2.0, -2.0) + float2(-1.0, 1.0);
	ClusterBounds bounds;
	bounds.Perspective = perspective;
	bounds.LateralMin = float2(LightClusterInfinity, LightClusterInfinity);
	bounds.LateralMax = -bounds.LateralMin;
	for (uint corner = 0; corner < 4; ++corner)
	{
		const float2 ndc = float2((corner & 1) != 0 ? ndcMax.x : ndcMin.x, (corner & 2) != 0 ? ndcMax.y : ndcMin.y);
		const float3 unprojected = UnprojectNdc(ndc, perspective);
		const float2 lateral = perspective ? unprojected.xy / -unprojected.z : unprojected.xy;
		bounds.LateralMin = min(bounds.LateralMin, lateral);
		bounds.LateralMax = max(bounds.LateralMax, lateral);
	}
	// Slice z covers the depths d with z <= log2(d) * scale + bias < z + 1.
	const float scale = Clusters.Slicing.z;
	const float bias = Clusters.Slicing.w;
	bounds.NearDepth = 0.0;
	bounds.FarDepth = LightClusterInfinity;
	if (scale > 0.0)
	{
		if (slice > 0)
			bounds.NearDepth = exp2((float(slice) - bias) / scale) * 0.999;
		if (slice + 1 < Clusters.Grid.z)
			bounds.FarDepth = exp2((float(slice + 1) - bias) / scale) * 1.001;
	}
	return bounds;
}

// Whether `light` may light a point of the cluster `bounds` describe.
bool LightReachesCluster(ShaderLight light, ClusterBounds bounds)
{
	if (light.Type == ShaderLightTypeDirectional || !(light.InverseRangeSquared > 0.0))
		return true;
	const float3 centre = mul(View.View, float4(light.Position, 1.0)).xyz;
	const float radius = rsqrt(light.InverseRangeSquared) * 1.01 + 1e-5 * (length(light.Position) + length(View.CameraPosition) + 1.0);
	const float depth = -centre.z;
	const float nearDepth = max(bounds.NearDepth, depth - radius);
	const float farDepth = min(bounds.FarDepth, depth + radius);
	if (!(nearDepth <= farDepth))
		return false;
	// The cluster's box over the depths it shares with the sphere: in perspective the tile's extent grows linearly with depth.
	const float2 lateralMin = bounds.Perspective ? min(bounds.LateralMin * nearDepth, bounds.LateralMin * farDepth) : bounds.LateralMin;
	const float2 lateralMax = bounds.Perspective ? max(bounds.LateralMax * nearDepth, bounds.LateralMax * farDepth) : bounds.LateralMax;
	const float3 closest = clamp(centre, float3(lateralMin, -farDepth), float3(lateralMax, -nearDepth));
	const float3 offset = centre - closest;
	return dot(offset, offset) <= radius * radius;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
	const uint tilesPerSlice = Clusters.Grid.x * Clusters.Grid.y;
	const uint wordsPerSlice = tilesPerSlice * Clusters.Grid.w;
	if (thread.x >= wordsPerSlice || thread.y >= Clusters.Grid.z)
		return;
	const uint tile = thread.x / Clusters.Grid.w;
	const uint word = thread.x % Clusters.Grid.w;
	const uint slice = thread.y;
	uint mask = 0;
	// A degenerate grid (Slicing.z 0) puts every depth in slice 0; the other slices hold nothing anyone reads.
	if (Clusters.Slicing.z > 0.0 || slice == 0)
	{
		const ClusterBounds bounds = GetClusterBounds(tile, slice);
		const uint first = word * 32;
		const uint end = min(View.LightCount, first + 32);
		for (uint index = first; index < end; ++index)
		{
			if (LightReachesCluster(Lights[index], bounds))
				mask |= 1u << (index - first);
		}
	}
	Masks[slice * wordsPerSlice + thread.x] = mask;
}
