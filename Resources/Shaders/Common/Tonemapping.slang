// The tonemappers of Architecture §8.9, exactly as Renderer/TonemapPass.h writes them out (the M8 contract froze the
// formulas, so this module and the CPU references of Tests/Source/Support/RenderReference.h implement the same text and
// "TonemapPass: every tonemapper matches its CPU reference at 1,024 points" compares them through Passes/TonemapCurves.slang).
// Each takes exposed linear radiance c >= 0 per channel and returns display-linear values in [0, 1], before the OETF.
module Tonemapping;

#include "Shared/TonemapConstants.h"

// (dot(row0, value), dot(row1, value), dot(row2, value)): the matrix with these rows times the column `value`.
float3 MultiplyRows(float3 row0, float3 row1, float3 row2, float3 value)
{
	return float3(dot(row0, value), dot(row1, value), dot(row2, value));
}

// AgX, Base look (Sobotka's AgX as Blender 4 configures it, the "minimal" fit for Rec. 709 primaries): the inset matrix,
// the log2 encoding over [MinEv, MaxEv], the 6th-order sigmoid polynomial, the outset matrix and the 2.2 power to
// display-linear.
public float3 TonemapAgx(float3 color)
{
	const float minEv = -12.47393;
	const float maxEv = 4.026069;
	const float3 inset = MultiplyRows(float3(0.842479062253094, 0.0784335999999992, 0.0792237451477643),
		float3(0.0423282422610123, 0.878468636469772, 0.0791661274605434), float3(0.0423756549057051, 0.0784336, 0.879142973793104), color);
	const float3 x = saturate((log2(max(inset, float3(1e-10))) - minEv) / (maxEv - minEv));
	const float3 x2 = x * x;
	const float3 x4 = x2 * x2;
	const float3 sigmoid = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
	const float3 outset = MultiplyRows(float3(1.19687900512017, -0.0980208811401368, -0.0990297440797205),
		float3(-0.0528968517574562, 1.15190312990417, -0.0989611768448433), float3(-0.0529716355144438, -0.0980434501171241, 1.15107367264116),
		sigmoid);
	return saturate(pow(max(outset, float3(0.0)), 2.2));
}

// ACES as Stephen Hill's fit of the RRT and the sRGB ODT, without an exposure pre-scale: the input matrix, the rational
// RRT+ODT fit per channel and the output matrix.
public float3 TonemapAces(float3 color)
{
	const float3 v = MultiplyRows(float3(0.59719, 0.35458, 0.04823), float3(0.07600, 0.90834, 0.01566), float3(0.02840, 0.13383, 0.83777), color);
	const float3 w = (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081);
	return saturate(MultiplyRows(float3(1.60475, -0.53108, -0.07367), float3(-0.10208, 1.10813, -0.00605), float3(-0.00327, -0.07276, 1.07602), w));
}

// Khronos PBR Neutral (the Khronos reference implementation): an offset for the darkest channel, then above
// StartCompression (0.76) a compression of the peak towards 1 with a desaturation of 0.15.
public float3 TonemapPbrNeutral(float3 color)
{
	const float startCompression = 0.76;
	const float desaturation = 0.15;
	const float d = 0.24;
	const float x = min(color.r, min(color.g, color.b));
	const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	float3 result = color - offset;
	const float peak = max(result.r, max(result.g, result.b));
	if (peak < startCompression)
		return saturate(result);
	const float newPeak = 1.0 - d * d / (peak + d - startCompression);
	result *= newPeak / peak;
	const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
	return saturate(lerp(result, float3(newPeak), g));
}

// Linear: a clamp (debugging, and the data debug views).
public float3 TonemapLinear(float3 color)
{
	return saturate(color);
}

// The tonemapper `tonemapper` (a TonemapConstants::Tonemapper value); Linear for any other value.
public float3 ApplyTonemapper(uint tonemapper, float3 color)
{
	switch (tonemapper)
	{
		case TonemapperAgx:        return TonemapAgx(color);
		case TonemapperAces:       return TonemapAces(color);
		case TonemapperPbrNeutral: return TonemapPbrNeutral(color);
		default:                   return TonemapLinear(color);
	}
}
