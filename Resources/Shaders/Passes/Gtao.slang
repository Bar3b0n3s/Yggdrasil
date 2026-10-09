#include "Shared/GtaoConstants.h"

import Common.Gtao;

[[vk::push_constant]] ConstantBuffer<GtaoConstants> Constants;
Texture2D<float> ViewDepth : register(t0, space0);
Texture2D<float2> SceneNormals : register(t1, space0);
Texture2D<float> SourceOcclusion : register(t2, space0);
[vk::image_format("r8")]
RWTexture2D<float> Destination : register(u0, space0);

static const float GtaoPi = 3.14159265358979323846;

float3 GtaoPosition(float2 uv, float depth)
{
	const float2 xy = (float2(2.0, -2.0) * uv + float2(-1.0, 1.0)) * Constants.PositionScale;
	return float3(Constants.ProjectionKind == 0 ? xy * depth : xy, -depth);
}

// Antiderivative of (nv*cos(theta) + nt*sin(theta))*sin(theta). Splitting at zero handles |sin(theta)|.
float GtaoArcPrimitive(float theta, float nv, float nt)
{
	const float sine = sin(theta);
	return 0.5 * nv * sine * sine + nt * (0.5 * theta - 0.25 * sin(2.0 * theta));
}

float GtaoIntegrateArc(float2 horizonCosines, float3 normal, float3 view, float3 tangent)
{
	const float nv = dot(normal, view);
	const float nt = dot(normal, tangent);
	if (nv * nv + nt * nt < 1e-10)
		return 0.0;
	const float normalAngle = atan2(nt, nv);
	const float first = max(-acos(clamp(horizonCosines.x, -1.0, 1.0)), normalAngle - 0.5 * GtaoPi);
	const float last = min(acos(clamp(horizonCosines.y, -1.0, 1.0)), normalAngle + 0.5 * GtaoPi);
	if (last <= first)
		return 0.0;
	float integral = 0.0;
	if (first < 0.0)
		integral += GtaoArcPrimitive(first, nv, nt) - GtaoArcPrimitive(min(last, 0.0), nv, nt);
	if (last > 0.0)
		integral += GtaoArcPrimitive(last, nv, nt) - GtaoArcPrimitive(max(first, 0.0), nv, nt);
	return max(integral, 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Constants.OutputSize))
		return;
	const uint2 pixel = GtaoFullPixel(id.xy, Constants.FullSize, Constants.OutputSize);
	const float depth = ViewDepth.Load(int3(pixel, 0));
	if (depth >= 65504.0 || depth <= 0.0 || Constants.Intensity == 0.0)
	{
		Destination[id.xy] = 1.0;
		return;
	}
	const float2 uv = (float2(pixel) + 0.5) / float2(Constants.FullSize);
	const float3 position = GtaoPosition(uv, depth);
	const float3 view = Constants.ProjectionKind == 0 ? normalize(-position) : float3(0.0, 0.0, 1.0);
	const float3 normal = DecodeGtaoNormal(SceneNormals.Load(int3(pixel, 0)));
	const float pixelRadius = min(Constants.Radius * (Constants.RadiusScale / (Constants.ProjectionKind == 0 ? depth : 1.0)),
		length(float2(Constants.FullSize)));
	if (pixelRadius < 1.0)
	{
		Destination[id.xy] = 1.0;
		return;
	}
	// Fixed interleaved-gradient orientation. No frame number, temporal rotation, reprojection or history.
	const float rotation = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
	float visibility = 0.0;
	float unoccluded = 0.0;
	for (uint slice = 0; slice < Constants.SliceCount; ++slice)
	{
		const float angle = GtaoPi * (float(slice) + rotation) / float(Constants.SliceCount);
		const float2 direction = float2(cos(angle), sin(angle));
		const float3 planeDirection = float3(direction.x * Constants.PositionScale.x / float(Constants.FullSize.x),
			-direction.y * Constants.PositionScale.y / float(Constants.FullSize.y), 0.0);
		const float3 tangent = normalize(planeDirection - view * dot(planeDirection, view));
		float2 horizons = float2(-1.0);
		for (uint side = 0; side < 2; ++side)
		{
			for (uint step = 1; step <= 3; ++step)
			{
				const float offset = max(1.0, pixelRadius * float(step * step) / 9.0);
				const float2 samplePixel = float2(pixel) + direction * (side == 0 ? -offset : offset);
				if (any(samplePixel < 0.0) || any(samplePixel >= float2(Constants.FullSize)))
					continue;
				const uint mip = min(uint(max(0.0, floor(log2(max(offset, 1.0))) - 2.0)), Constants.MipCount - 1);
				const uint2 mipSize = max(Constants.FullSize >> mip, uint2(1));
				const float2 sampleUv = (floor(samplePixel) + 0.5) / float2(Constants.FullSize);
				const uint2 mipPixel = min(uint2(sampleUv * float2(mipSize)), mipSize - 1);
				const float sampleDepth = ViewDepth.Load(int3(mipPixel, mip));
				if (sampleDepth >= 65504.0 || sampleDepth <= 0.0)
					continue;
				const float3 delta = GtaoPosition(sampleUv, sampleDepth) - position;
				const float distance = length(delta);
				if (distance <= 1e-6 || dot(delta, normal) <= 0.0)
					continue;
				// Fade only the outer fifth of the sphere. A receding sample relaxes the horizon (thin-occluder
				// heuristic); a growing horizon replaces it, preserving solid corners.
				const float attenuation = saturate((1.0 - distance / Constants.Radius) * 5.0);
				const float candidate = lerp(-1.0, dot(delta / distance, view), attenuation);
				horizons[side] = candidate >= horizons[side] ? candidate : lerp(horizons[side], candidate, 0.1);
			}
		}
		visibility += GtaoIntegrateArc(horizons, normal, view, tangent);
		unoccluded += GtaoIntegrateArc(float2(-1.0), normal, view, tangent);
	}
	// Normalize against the same unoccluded hemisphere quadrature. Sparse slices otherwise darken flat
	// off-axis receivers even without an occluder, from projected-normal integration error.
	Destination[id.xy] = pow(saturate(visibility / max(unoccluded, 1e-6)), Constants.Intensity);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDenoise(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Constants.OutputSize))
		return;
	const uint2 pixel = GtaoFullPixel(id.xy, Constants.FullSize, Constants.OutputSize);
	const float depth = ViewDepth.Load(int3(pixel, 0));
	if (depth >= 65504.0)
	{
		Destination[id.xy] = 1.0;
		return;
	}
	const float3 normal = DecodeGtaoNormal(SceneNormals.Load(int3(pixel, 0)));
	float weighted = 0.0;
	float weightSum = 0.0;
	for (int offset = -2; offset <= 2; ++offset)
	{
		const int2 coordinate = int2(id.xy) + (Constants.Axis == 0 ? int2(offset, 0) : int2(0, offset));
		if (any(coordinate < 0) || any(coordinate >= int2(Constants.OutputSize)))
			continue;
		const uint2 fullTap = GtaoFullPixel(uint2(coordinate), Constants.FullSize, Constants.OutputSize);
		const float sampleDepth = ViewDepth.Load(int3(fullTap, 0));
		const float3 sampleNormal = DecodeGtaoNormal(SceneNormals.Load(int3(fullTap, 0)));
		const float spatial = offset == 0 ? 6.0 : (abs(offset) == 1 ? 4.0 : 1.0);
		const float weight = spatial * GtaoEdgeWeight(depth, sampleDepth, normal, sampleNormal);
		weighted += weight * SourceOcclusion.Load(int3(coordinate, 0));
		weightSum += weight;
	}
	Destination[id.xy] = weighted / max(weightSum, 1e-6);
}

// Test harness for the production forward helper. No runtime pass/pipeline is created for this entry.
[shader("compute")]
[numthreads(8, 8, 1)]
void CSTestUpsample(uint3 id : SV_DispatchThreadID)
{
	if (all(id.xy < Constants.FullSize))
		Destination[id.xy] = SampleGtao(SourceOcclusion, ViewDepth, SceneNormals, id.xy);
}
