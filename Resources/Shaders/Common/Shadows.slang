// In-house PCSS: linear blocker distances, 16/32 Vogel taps and fixed per-pixel IGN rotation.
// Perspective spot geometry follows the PCSS integration paper; directional lights use angular world penumbrae.
#pragma once

#include "Shared/ShadowConstants.h"

// Vendored NVRHI's Vulkan comparison sampler is strict Less. Complementing each filtered result is exactly
// GreaterOrEqual (including equality), and commutes with bilinear PCF. Do not use a different comparison op.
static const float ShadowPi = 3.14159265358979323846;
static const uint ShadowBlockerTaps = 16;
static const uint ShadowFilterTaps = 32;

float ShadowRotation(uint2 pixel)
{
	return 2.0 * ShadowPi * frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
}

float2 ShadowDisk(uint index, uint count, float rotation)
{
	float angle = float(index) * 2.399963229728653 + rotation;
	return sqrt((float(index) + 0.5) / float(count)) * float2(cos(angle), sin(angle));
}

float2 ShadowUv(float4 clip)
{
	return clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
}

float ShadowLinearDepth(float depth, float nearPlane, float farPlane, bool perspective)
{
	return perspective ? nearPlane * farPlane / (nearPlane + depth * (farPlane - nearPlane)) : farPlane - depth * (farPlane - nearPlane);
}

// A geometric receiver plane is linear in projected UV/depth, including perspective projection. Differentiate
// two world tangents analytically: screen derivatives inside divergent cascade/light branches are undefined.
float2 ShadowReceiverGradient(float4x4 viewProjection, float4 clip, float3 normal)
{
	float3 tangent = cross(normal, abs(normal.z) < 0.9 ? float3(0, 0, 1) : float3(0, 1, 0));
	float3 bitangent = cross(normal, tangent);
	float4 first = mul(viewProjection, float4(tangent, 0));
	float4 second = mul(viewProjection, float4(bitangent, 0));
	float3 a = (first.xyz * clip.w - clip.xyz * first.w) * float3(0.5, -0.5, 1);
	float3 b = (second.xyz * clip.w - clip.xyz * second.w) * float3(0.5, -0.5, 1);
	float determinant = a.x * b.y - a.y * b.x;
	float scale = sqrt(dot(a.xy, a.xy) * dot(b.xy, b.xy));
	// A plane edge-on to the light has no invertible UV parameterization; retain the existing normal/caster bias.
	if (scale == 0.0 || abs(determinant) <= 1e-6 * scale)
		return 0.0;
	float2 gradient = float2(a.z * b.y - a.y * b.z, a.x * b.z - a.z * b.x) / determinant;
	return all(isfinite(gradient)) ? gradient : float2(0.0);
}

float ShadowReceiverDepth(float depth, float2 gradient, float2 offset)
{
	float projected = depth + dot(gradient, offset);
	// Eight float-epsilon relative error bounds cover matrix/raster interpolation roundoff, without a world bias.
	return max(0.0, projected + max(abs(depth), abs(projected)) * (8.0 * 1.1920928955078125e-7));
}

// SampleCmp shares one reference across its four bilinear texels. Use the receiver plane's nearest depth over
// exactly that footprint, never over the PCSS search radius. This bound is less than one texel on either axis.
float ShadowFilterDepth(float depth, float2 gradient, float2 uv, float2 sampleUv, float2 dimensions)
{
	float2 lower = floor(sampleUv * dimensions - 0.5) + 0.5;
	float2 nearest = (lower + float2(gradient.x > 0.0 ? 1.0 : 0.0, gradient.y > 0.0 ? 1.0 : 0.0)) / dimensions;
	return ShadowReceiverDepth(depth, gradient, nearest - uv);
}

// Four weights also drive the debug palette. Their sum fades to zero over the final cascade's last ten percent.
float4 ShadowCascadeWeights(ShadowConstants shadows, float viewDistance)
{
	float4 weights = 0.0;
	if (viewDistance <= 0.0 || shadows.Counts.x == 0 || viewDistance >= shadows.CascadeSplits[shadows.Counts.x - 1])
		return weights;
	for (uint cascade = 0; cascade < shadows.Counts.x; ++cascade)
	{
		if (viewDistance <= shadows.CascadeSplits[cascade])
		{
			float fade = saturate((viewDistance - shadows.CascadeBlendStarts[cascade]) /
				max(shadows.CascadeSplits[cascade] - shadows.CascadeBlendStarts[cascade], 1e-6));
			weights[cascade] = 1.0 - fade;
			if (cascade + 1 < shadows.Counts.x)
				weights[cascade + 1] = fade;
			break;
		}
	}
	return weights;
}

float3 ShadowCascadeColor(ShadowConstants shadows, float viewDistance)
{
	float4 weights = ShadowCascadeWeights(shadows, viewDistance);
	return float3(weights.x + weights.w, weights.y + weights.w, weights.z);
}

float SampleCascadePcss(Texture2DArray<float> maps, SamplerComparisonState comparison, ShadowConstants shadows,
	uint cascade, float3 worldPosition, float3 normal, float3 toLight, uint2 pixel)
{
	uint width, height, layers;
	maps.GetDimensions(width, height, layers);
	float texel = 1.0 / float(width);
	float offset = shadows.Directional.z * shadows.CascadeTexelWorldSizes[cascade] * (1.0 - saturate(dot(normal, toLight)));
	float4 clip = mul(shadows.CascadeViewProjection[cascade], float4(worldPosition + normal * offset, 1.0));
	float2 uv = ShadowUv(clip);
	float receiverDepth = clip.z / clip.w;
	if (any(uv < 0.0) || any(uv > 1.0) || receiverDepth < 0.0 || receiverDepth > 1.0)
		return 1.0;
	float2 gradient = ShadowReceiverGradient(shadows.CascadeViewProjection[cascade], clip, normal);
	float4 range = shadows.CascadeDepthRanges[cascade];
	float receiverDistance = ShadowLinearDepth(receiverDepth, range.x, range.y, false);
	float maximumRadius = max(1.5 * texel, range.w);
	float searchRadius = clamp((receiverDistance - range.x) * range.z, 1.5 * texel, maximumRadius);
	float rotation = ShadowRotation(pixel);
	float blockerSum = 0.0;
	uint blockers = 0;
	for (uint tap = 0; tap < ShadowBlockerTaps; ++tap)
	{
		float2 sampleUv = clamp(uv + ShadowDisk(tap, ShadowBlockerTaps, rotation) * searchRadius, 0.5 * texel, 1.0 - 0.5 * texel);
		int2 coordinate = int2(sampleUv * float2(width, height));
		float2 centre = (float2(coordinate) + 0.5) / float2(width, height);
		float depth = maps.Load(int4(coordinate, cascade, 0));
		if (depth > 0.0 && depth > ShadowReceiverDepth(receiverDepth, gradient, centre - uv))
		{
			blockerSum += ShadowLinearDepth(depth, range.x, range.y, false);
			++blockers;
		}
	}
	if (blockers == 0)
		return 1.0;
	float filterRadius = clamp((receiverDistance - blockerSum / float(blockers)) * range.z, 1.5 * texel, maximumRadius);
	float visibility = 0.0;
	for (uint tap = 0; tap < ShadowFilterTaps; ++tap)
	{
		float2 sampleUv = clamp(uv + ShadowDisk(tap, ShadowFilterTaps, rotation) * filterRadius, 0.5 * texel, 1.0 - 0.5 * texel);
		float reference = ShadowFilterDepth(receiverDepth, gradient, uv, sampleUv, float2(width, height));
		visibility += 1.0 - maps.SampleCmpLevelZero(comparison, float3(sampleUv, cascade), reference);
	}
	return visibility / float(ShadowFilterTaps);
}

// Evaluate only for Counts.z (the uploaded directional light index). ReceiveShadows=false bypasses this helper.
float SampleDirectionalShadow(Texture2DArray<float> maps, SamplerComparisonState comparison, ShadowConstants shadows,
	float3 worldPosition, float3 normal, float3 toLight, float viewDistance, uint2 pixel)
{
	float4 weights = ShadowCascadeWeights(shadows, viewDistance);
	float visibility = 1.0 - dot(weights, float4(1.0));
	for (uint cascade = 0; cascade < shadows.Counts.x; ++cascade)
		if (weights[cascade] > 0.0)
			visibility += weights[cascade] * SampleCascadePcss(maps, comparison, shadows, cascade, worldPosition, normal, toLight, pixel);
	return visibility;
}

// Finds the atlas allocation by uploaded Lights index. Unallocated/point lights return full visibility.
float SampleSpotShadow(Texture2D<float> atlas, SamplerComparisonState comparison, ShadowConstants shadows,
	uint lightIndex, float3 worldPosition, float3 normal, float3 toLight, uint2 pixel)
{
	uint width, height;
	atlas.GetDimensions(width, height);
	for (uint index = 0; index < shadows.Counts.y; ++index)
	{
		SpotShadowConstants spot = shadows.Spots[index];
		if (spot.Indices.x != lightIndex)
			continue;
		float4 clip = mul(spot.ViewProjection, float4(worldPosition, 1.0));
		if (clip.w <= 0.0)
			return 1.0;
		float distance = ShadowLinearDepth(saturate(clip.z / clip.w), spot.DepthSoftness.x, spot.DepthSoftness.y, true);
		float offset = spot.Bias.y * spot.Bias.z * distance * (1.0 - saturate(dot(normal, toLight)));
		clip = mul(spot.ViewProjection, float4(worldPosition + normal * offset, 1.0));
		if (clip.w <= 0.0)
			return 1.0;
		float2 localUv = ShadowUv(clip);
		float receiverDepth = clip.z / clip.w;
		if (any(localUv < 0.0) || any(localUv > 1.0) || receiverDepth < 0.0 || receiverDepth > 1.0)
			return 1.0;
		float receiverDistance = ShadowLinearDepth(receiverDepth, spot.DepthSoftness.x, spot.DepthSoftness.y, true);
		float2 texel = 1.0 / float2(width, height);
		float2 uv = localUv * spot.UvScaleBias.xy + spot.UvScaleBias.zw;
		float2 gradient = ShadowReceiverGradient(spot.ViewProjection, clip, normal) / spot.UvScaleBias.xy;
		// Clamp to interior texel centres, so bilinear comparison never consumes guard or neighbouring tile texels.
		float2 lower = spot.UvScaleBias.zw + 0.5 * texel;
		float2 upper = spot.UvScaleBias.zw + spot.UvScaleBias.xy - 0.5 * texel;
		float projectionScale = length(float3(spot.ViewProjection[0][0], spot.ViewProjection[0][1], spot.ViewProjection[0][2]));
		float sizeUv = spot.DepthSoftness.z * projectionScale * 0.5 * spot.UvScaleBias.x;
		float maximumRadius = max(1.5 * texel.x, spot.DepthSoftness.w);
		float searchRadius = clamp(sizeUv * (receiverDistance - spot.DepthSoftness.x) / (receiverDistance * spot.DepthSoftness.x), 1.5 * texel.x, maximumRadius);
		float rotation = ShadowRotation(pixel);
		float blockerSum = 0.0;
		uint blockers = 0;
		for (uint tap = 0; tap < ShadowBlockerTaps; ++tap)
		{
			float2 sampleUv = clamp(uv + ShadowDisk(tap, ShadowBlockerTaps, rotation) * searchRadius, lower, upper);
			int2 coordinate = int2(sampleUv * float2(width, height));
			float2 centre = (float2(coordinate) + 0.5) / float2(width, height);
			float depth = atlas.Load(int3(coordinate, 0));
			if (depth > 0.0 && depth > ShadowReceiverDepth(receiverDepth, gradient, centre - uv))
			{
				blockerSum += ShadowLinearDepth(depth, spot.DepthSoftness.x, spot.DepthSoftness.y, true);
				++blockers;
			}
		}
		if (blockers == 0)
			return 1.0;
		float blockerDistance = blockerSum / float(blockers);
		float radius = clamp(sizeUv * (receiverDistance - blockerDistance) / (receiverDistance * blockerDistance), 1.5 * texel.x, maximumRadius);
		float visibility = 0.0;
		for (uint tap = 0; tap < ShadowFilterTaps; ++tap)
		{
			float2 sampleUv = clamp(uv + ShadowDisk(tap, ShadowFilterTaps, rotation) * radius, lower, upper);
			float reference = ShadowFilterDepth(receiverDepth, gradient, uv, sampleUv, float2(width, height));
			visibility += 1.0 - atlas.SampleCmpLevelZero(comparison, sampleUv, reference);
		}
		return visibility / float(ShadowFilterTaps);
	}
	return 1.0;
}
