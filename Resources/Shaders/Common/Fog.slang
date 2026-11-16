// The fog of a view (M32, Docs/Decisions/0047-m32-fog-ssr.md decisions 3 and 5; Renderer/Fog.h; Architecture §8.6). The
// passes that fog include it after declaring the view's fog constants, which it reads by name: Fog
// (ConstantBuffer<FogConstants>, Shared/FogConstants.h; b5 of the forward, skybox and SSR-composite layouts, b1 of the quads').
// Every caller tests Fog.Enabled first, so a view without fog runs none of it.
//
// A point is fogged along its view ray from its eye point (the camera for a perspective view; the point moved back along the
// camera's forward axis to the camera's plane for an orthographic one) over its view depth z (its distance along the forward
// axis), from StartDistance on: with L = max(z - StartDistance, 0), the optical depth is Density L plus the height fog's
// exact integral along that path, taken from its lower end so that no exponent grows with the path's height:
// HeightDensity e^(-k (h_low - BaseHeight)) L G(k |dH|), where k is HeightFalloff, h_low the lower of the path's heights at
// StartDistance and at z, dH its rise over L and G(x) = (1 - e^(-x)) / x. The fog amount is
// min(1 - e^(-depth), MaxOpacity) and its colour Fog.Color plus the sun glow, SunColor * max(dot(ray, SunDirection), 0) ^
// SunGlowExponent along the view ray. A blended colour becomes c (1 - a) + F a, an additive one c (1 - a). The sky, at
// infinite depth, takes the limit: MaxOpacity with distance fog, else the height fog's integral to infinity, which converges
// only for a ray that climbs. The density's exponent is clamped to FogMaxExponent, so no term overflows, and a camera high
// above a thin layer of height fog still sees it below (a path's density read at its top would underflow to 0).
#pragma once

#include "Shared/FogConstants.h"

// The largest exponent a fog term takes: e^80 is about 5.5e34, far inside binary32's range.
static const float FogMaxExponent = 80.0;

// G(x) = (1 - e^(-x)) / x for x >= 0, the share of a path's length that a density falling at the rate x over it, from its
// value at the path's start, weighs: 1 at 0, in (0, 1].
float GetFogPathFactor(float x)
{
	if (x < 1e-3)
		return 1.0 - x * (0.5 - x / 6.0);
	return (1.0 - exp(-x)) / x;
}

// The height fog's density per metre at the world height `height`.
float GetFogHeightDensity(float height)
{
	return Fog.HeightDensity * exp(min(-Fog.HeightFalloff * (height - Fog.BaseHeight), FogMaxExponent));
}

// The fog's colour along the unit view ray `ray`: Fog.Color plus the sun glow.
float3 GetFogColor(float3 ray)
{
	return Fog.Color + Fog.SunColor * pow(saturate(dot(ray, Fog.SunDirection)), Fog.SunGlowExponent);
}

// The fog of the world point `worldPosition`: rgb its colour, a its amount.
float4 EvaluateFog(float3 worldPosition)
{
	const float depth = dot(worldPosition - Fog.CameraPosition, Fog.ViewForward);
	const float3 eye = Fog.Orthographic != 0 ? worldPosition - Fog.ViewForward * depth : Fog.CameraPosition;
	const float3 offset = worldPosition - eye;
	const float lengthSquared = dot(offset, offset);
	const float3 ray = Fog.Orthographic != 0 || !(lengthSquared > 1e-20) ? Fog.ViewForward : offset * rsqrt(lengthSquared);
	const float pathLength = max(depth - Fog.StartDistance, 0.0);
	float opticalDepth = Fog.Density * pathLength;
	if (Fog.HeightDensity > 0.0 && pathLength > 0.0)
	{
		// The height gained per metre of view depth, and the path's heights where the fog begins and at the point: the
		// density falls from the lower end over |rise| L metres of height.
		const float rise = offset.y / depth;
		const float startHeight = eye.y + rise * Fog.StartDistance;
		const float lowHeight = min(startHeight, startHeight + rise * pathLength);
		opticalDepth += GetFogHeightDensity(lowHeight) * pathLength * GetFogPathFactor(Fog.HeightFalloff * abs(rise) * pathLength);
	}
	return float4(GetFogColor(ray), min(1.0 - exp(-opticalDepth), Fog.MaxOpacity));
}

// The fog of the sky along the unit world direction `direction`, seen from the eye point `eye` (the camera for a
// perspective view; the pixel's point on the camera's plane for an orthographic one, whose direction is ViewForward).
float4 EvaluateSkyFog(float3 direction, float3 eye)
{
	if (Fog.Density > 0.0)
		return float4(GetFogColor(direction), Fog.MaxOpacity);
	float amount = 0.0;
	if (Fog.HeightDensity > 0.0)
	{
		const float rise = Fog.Orthographic != 0 ? Fog.ViewForward.y : direction.y / max(dot(direction, Fog.ViewForward), 1e-6);
		const float atStart = GetFogHeightDensity(eye.y + rise * Fog.StartDistance);
		const float rate = Fog.HeightFalloff * rise;
		amount = rate > 1e-6 ? min(1.0 - exp(-atStart / rate), Fog.MaxOpacity) : Fog.MaxOpacity;
	}
	return float4(GetFogColor(direction), amount);
}

// `color` behind the fog `fog` (EvaluateFog's): blended toward its colour by its amount.
float3 ApplyFog(float3 color, float4 fog)
{
	return color * (1.0 - fog.a) + fog.rgb * fog.a;
}

// An additive `color` behind the fog `fog`: faded by its amount, so the fog's own colour is not added twice.
float3 ApplyFogAdditive(float3 color, float4 fog)
{
	return color * (1.0 - fog.a);
}
