// The forward passes' light loop (M23, R-CLC; Docs/Decisions/0034-m23-contract.md decisions 4 and 5): the direct light of
// the view's lights at one pixel. Passes/Scene.slang includes it after its resource declarations and reads it through
// ShadeDirectLights; it uses the view's Lights, Shadows, ShadowMaps, ShadowAtlas, PointShadowMaps, PointShadowMapsHalf (M29),
// ShadowCompare, Clusters and LightClusterMasks. Each light's shadow visibility is Common/Shadows.slang's GetLightVisibility (stream B's, with the
// point-shadow branch; decision 4).
//
// The loop visits the set bits of the pixel's cluster's masks in ascending order, which is the light list's order
// (Clusters.Mode.x LightClusterModeClustered), or every light of the list, as the bits of full words
// (LightClusterModeBruteForce, the reference RenderQualitySettings::ClusteredLights false selects): both run the same body.
// A light outside the pixel's cluster contributes exactly zero there (the windowed falloff is 0 beyond Range, a spot's cone
// factor 0 outside its outer cone, and ShadeLight returns float3(0) for a zero attenuation), and the body adds nothing for
// a light whose direct light is exactly zero, so the clustered sum is the brute-force sum, bit for bit. Later milestones'
// indirect terms (M26's probes) are added after the loop, outside it.
#pragma once

#include "Shared/LightClusterConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ShadowConstants.h"
#include "Common/LightClusters.slang"
#include "Common/Shadows.slang"

// The direct light of one light-list entry at `position`.
float3 ShadeLight(ShaderLight light, float3 position, float3 normal, float3 view, float nDotV, float3 diffuseColor, float3 f0,
	float3 energyCompensation, float alpha, float3 reflection)
{
	if (light.Type == ShaderLightTypeDirectional)
	{
		const float3 direction = -light.Direction;
		return EvaluateDirectLight(normal, view, nDotV, diffuseColor, f0, energyCompensation, alpha, direction, direction, alpha, Pi * light.Radiance);
	}

	const float3 toLight = light.Position - position;
	const float distanceSquared = dot(toLight, toLight);
	const float3 direction = toLight * rsqrt(max(distanceSquared, 1e-12));
	float attenuation = WindowedInverseSquare(distanceSquared, light.InverseRangeSquared, light.SourceRadius);
	if (light.Type == ShaderLightTypeSpot)
		attenuation *= SpotConeAttenuation(dot(-direction, light.Direction), light.SpotCosOuter, light.SpotCosInner);
	if (attenuation <= 0.0)
		return float3(0.0);

	float3 specularDirection = direction;
	float specularAlpha = alpha;
	if (light.SourceRadius > 0.0)
	{
		specularDirection = SphereLightSpecularDirection(toLight, reflection, light.SourceRadius);
		specularAlpha = SphereLightAlpha(alpha, light.SourceRadius, sqrt(distanceSquared));
	}
	return EvaluateDirectLight(normal, view, nDotV, diffuseColor, f0, energyCompensation, alpha, direction, specularDirection, specularAlpha,
		Pi * light.Radiance * attenuation);
}

// The bits of the light list's entries in mask word `word` of a list of `count` lights: what brute force visits.
uint GetLightListWord(uint word, uint count)
{
	const uint first = word * 32;
	if (count >= first + 32)
		return 0xFFFFFFFFu;
	return count > first ? (1u << (count - first)) - 1u : 0u;
}

// The direct light at the pixel `pixel` (its scene-resolution coordinates) shading `worldPosition` at view depth `viewDepth`,
// summed over the lights in the light list's order: each light's ShadeLight times its shadow visibility (1 unless
// `receiveShadows`). The BRDF inputs are ShadeForward's.
float3 ShadeDirectLights(float3 worldPosition, uint2 pixel, float viewDepth, bool receiveShadows, float3 shadowNormal, float3 normal, float3 view,
	float nDotV, float3 diffuseColor, float3 f0, float3 energyCompensation, float alpha, float3 reflection)
{
	const bool clustered = Clusters.Mode.x == LightClusterModeClustered;
	const uint maskOffset = clustered ? GetLightClusterIndex(Clusters, pixel, viewDepth) * Clusters.Grid.w : 0;
	float3 radiance = float3(0.0);
	for (uint word = 0; word * 32 < View.LightCount; ++word)
	{
		uint mask = clustered ? LightClusterMasks[maskOffset + word] : GetLightListWord(word, View.LightCount);
		while (mask != 0)
		{
			const uint index = word * 32 + firstbitlow(mask);
			mask &= mask - 1;
			const ShaderLight light = Lights[index];
			const float3 direct = ShadeLight(light, worldPosition, normal, view, nDotV, diffuseColor, f0, energyCompensation, alpha, reflection);
			if (any(direct != float3(0.0)))
			{
				const float visibility = receiveShadows
					? GetLightVisibility(ShadowMaps, ShadowAtlas, PointShadowMaps, PointShadowMapsHalf, ShadowCompare, Shadows, light, index,
						worldPosition, shadowNormal, viewDepth, pixel)
					: 1.0;
				radiance += visibility * direct;
			}
		}
	}
	return radiance;
}
