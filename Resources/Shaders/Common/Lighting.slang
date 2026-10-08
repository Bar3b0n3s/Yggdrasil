// The lighting terms of the PBR forward passes (Architecture §8.5, §8.6), built on Common/Brdf.slang:
//   - artist-unit lights: a light of radiance C * I (Color times Intensity) delivers pi * C * I illuminance to a surface
//     facing it, so a white Lambertian surface lit head-on by a directional light of intensity 1 has radiance 1 (the
//     walking skeleton's Lambert convention, kept); point and spot lights fall off with the windowed inverse square up to
//     their Range, spot lights with a smoothstep between their outer and inner cone, and SourceRadius widens the specular
//     lobe (Karis 2013's representative point with the (alpha / alpha')² normalization);
//   - geometric specular anti-aliasing from the screen-space variance of the geometric normal (Tokuyoshi and Kaplanyan
//     2019, with Filament's variance and threshold);
//   - the image-based terms' occlusion: Jimenez's multi-bounce fit for diffuse, Lagarde's specular occlusion and the
//     horizon occlusion of normal-mapped surfaces (Docs/Decisions/0013-m8-decisions.md decision 21, A).
module Lighting;

import Common.Brdf;

// The screen-space normal variance scale and the largest added alpha² of the specular anti-aliasing.
static const float SpecularAntiAliasingVariance = 0.15;
static const float SpecularAntiAliasingThreshold = 0.2;

// The closest distance the inverse-square falloff is evaluated at (1 cm), so a light at the shaded point stays finite.
static const float MinLightDistanceSquared = 1e-4;

// Alpha widened by the variance of `geometricNormal` across the pixel quad: sqrt(alpha² + min(2 sigma² |dN|², threshold)).
public float ApplySpecularAntiAliasing(float3 geometricNormal, float alpha)
{
	const float3 du = ddx(geometricNormal);
	const float3 dv = ddy(geometricNormal);
	const float variance = SpecularAntiAliasingVariance * (dot(du, du) + dot(dv, dv));
	const float kernelAlpha2 = min(2.0 * variance, SpecularAntiAliasingThreshold);
	return sqrt(saturate(alpha * alpha + kernelAlpha2));
}

// The windowed inverse-square falloff of a point or spot light: saturate(1 - (d / Range)^4)² / max(d², 1 cm², radius²).
public float WindowedInverseSquare(float distanceSquared, float inverseRangeSquared, float sourceRadius)
{
	const float factor = distanceSquared * inverseRangeSquared;
	const float window = saturate(1.0 - factor * factor);
	return window * window / max(distanceSquared, max(MinLightDistanceSquared, sourceRadius * sourceRadius));
}

// The smoothstep cone attenuation of a spot light for the cosine of the angle between its axis and the shaded point.
public float SpotConeAttenuation(float cosAngle, float cosOuter, float cosInner)
{
	return smoothstep(cosOuter, max(cosInner, cosOuter + 1e-4), cosAngle);
}

// The unit direction to the point of a sphere light (its centre at `toLight` from the shaded point, `radius` metres)
// closest to the reflection ray `reflection` (Karis 2013's representative point), for the specular lobe.
public float3 SphereLightSpecularDirection(float3 toLight, float3 reflection, float radius)
{
	const float3 centreToRay = dot(toLight, reflection) * reflection - toLight;
	const float distanceToRay = length(centreToRay);
	const float3 closest = toLight + centreToRay * saturate(radius / max(distanceToRay, 1e-6));
	return normalize(closest);
}

// The alpha of a sphere light's widened specular lobe: alpha plus half the light's angular radius, at most 1.
public float SphereLightAlpha(float alpha, float radius, float distance)
{
	return saturate(alpha + radius / (2.0 * max(distance, 1e-4)));
}

// The light one direction reflects towards the viewer: (Lambert diffuse + GGX specular with the multi-scatter
// compensation) times n.l times `irradiance` (pi times the light's radiance times its attenuation). The diffuse term uses
// `lightDirection`, the specular term `specularDirection` with `specularAlpha` and the energy normalization
// (alpha / specularAlpha)² (both equal `lightDirection` and alpha for a light without a source radius).
public float3 EvaluateDirectLight(float3 normal, float3 view, float nDotV, float3 diffuseColor, float3 f0, float3 energyCompensation, float alpha,
	float3 lightDirection, float3 specularDirection, float specularAlpha, float3 irradiance)
{
	const float nDotL = saturate(dot(normal, lightDirection));
	const float3 diffuse = diffuseColor * (InversePi * nDotL);
	float3 specular = float3(0.0);
	const float nDotLs = saturate(dot(normal, specularDirection));
	if (nDotLs > 0.0)
	{
		const float3 halfSum = view + specularDirection;
		const float3 halfVector = halfSum * rsqrt(max(dot(halfSum, halfSum), 1e-12));
		const float nDotH = saturate(dot(normal, halfVector));
		const float lDotH = saturate(dot(specularDirection, halfVector));
		const float distribution = DistributionGgx(nDotH, specularAlpha);
		const float visibility = VisibilitySmithGgxCorrelated(nDotV, nDotLs, specularAlpha);
		const float3 fresnel = FresnelSchlick(f0, lDotH);
		const float ratio = alpha / max(specularAlpha, 1e-6);
		specular = fresnel * energyCompensation * (distribution * visibility * ratio * ratio * nDotLs);
	}
	return (diffuse + specular) * irradiance;
}

// Jimenez et al. 2016's multi-bounce fit: the visibility of the diffuse indirect light for ambient occlusion `ao` over a
// surface of `albedo`, brighter than ao alone because light bounces inside the occluded region.
public float3 MultiBounceAmbientOcclusion(float ao, float3 albedo)
{
	const float3 a = 2.0404 * albedo - 0.3324;
	const float3 b = -4.7951 * albedo + 0.6417;
	const float3 c = 2.7552 * albedo + 0.6903;
	return max(float3(ao), ((ao * a + b) * ao + c) * ao);
}

// Lagarde and de Rousiers 2014's specular occlusion from ambient occlusion, n.v and alpha.
public float SpecularOcclusion(float nDotV, float ao, float alpha)
{
	return saturate(pow(nDotV + ao, exp2(-16.0 * alpha - 1.0)) - 1.0 + ao);
}

// The horizon occlusion of a normal-mapped surface: a reflection below the geometric surface is dimmed (Jimenez 2016).
public float HorizonOcclusion(float3 reflection, float3 geometricNormal)
{
	const float horizon = min(1.0 + dot(reflection, geometricNormal), 1.0);
	return horizon * horizon;
}
