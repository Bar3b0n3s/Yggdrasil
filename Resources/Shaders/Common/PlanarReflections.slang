// The planar reflections' term of the forward passes (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3;
// Renderer/PlanarReflections.h). Passes/Scene.slang includes it after declaring the view's PlanarReflection
// (ConstantBuffer<PlanarReflectionConstants>, b7), PlanarReflections0 to PlanarReflections3 (t17 to t20) and LinearClamp,
// which it reads by name.
//
// On a drawn reflection's glass (DrawFlagPlanarReflection) the forward pass replaces the prefiltered radiance (the
// environment's or a reflection probe's) with the reflection of the instance's slot: the reflection image at the shaded
// point's projection by the slot's view-projection (the mirror camera's, whose image is the main view's pixel mirrored on
// the glass), blurred by roughness through its mips (ShadePlanarReflection's level curve) and faded back into the
// prefiltered radiance over the upper half of the roughnesses up to SsrMaxRoughness, as SSR fades. The caller multiplies
// the result by the same multi-scatter, specular and horizon occlusion as the prefiltered term.
#pragma once

#include "Shared/PlanarReflectionConstants.h"

// The reflection of slot `slot` at texture coordinates `uv`, at mip level `level`.
float3 SamplePlanarReflectionSlot(uint slot, float2 uv, float level)
{
	switch (slot)
	{
		case 0:  return PlanarReflections0.SampleLevel(LinearClamp, uv, level).rgb;
		case 1:  return PlanarReflections1.SampleLevel(LinearClamp, uv, level).rgb;
		case 2:  return PlanarReflections2.SampleLevel(LinearClamp, uv, level).rgb;
		default: return PlanarReflections3.SampleLevel(LinearClamp, uv, level).rgb;
	}
}

// `prefiltered` with slot `slot`'s reflection in its place at the shaded point `worldPosition`, whose specular lobe has the
// GGX `alpha` and the roughness coordinate `roughness` (sqrt(alpha), the one SSR compares with its MaxRoughness): the
// reflection at level log2(max(alpha * Heights[slot] * PlanarReflectionBlurScale, 1)) (Shared/PlanarReflectionConstants.h),
// weighted by saturate((SsrMaxRoughness - roughness) / (SsrMaxRoughness / 2)), so it is whole up to half SsrMaxRoughness and
// gone at it. The slot's chain reaches the level of SsrMaxRoughness (Renderer/PlanarReflections.h's
// ComputePlanarReflectionMipCount), so the clamp to its last mip changes no sample that has a weight. A slot past Count, or a
// point the projection puts behind the mirror camera, keeps `prefiltered`.
float3 ShadePlanarReflection(uint slot, float3 worldPosition, float roughness, float alpha, float3 prefiltered)
{
	if (slot >= PlanarReflection.Count)
		return prefiltered;
	const float4 clip = mul(PlanarReflection.ViewProjections[slot], float4(worldPosition, 1.0));
	if (!(clip.w > 1e-6))
		return prefiltered;
	const float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
	const float level = clamp(log2(max(alpha * PlanarReflection.Heights[slot] * PlanarReflectionBlurScale, 1.0)), 0.0,
		max(PlanarReflection.MipCounts[slot] - 1.0, 0.0));
	const float maxRoughness = PlanarReflection.SsrMaxRoughness;
	const float weight = saturate((maxRoughness - roughness) / max(maxRoughness * 0.5, 1e-6));
	return lerp(prefiltered, SamplePlanarReflectionSlot(slot, uv, level), weight);
}
