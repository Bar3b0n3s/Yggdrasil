// Tonemap: §8.3 pass 11, tonemap and encode (Renderer/TonemapPass.h): SceneColor (RGBA16_FLOAT, linear radiance) to
// LdrColor (RGBA8_UNORM, display-encoded values, §8.9) in the order TonemapPass.h fixes: the bloom composite, the
// exposure (M26: times the Exposure pass's adapted exposure with TonemapFlagAutoExposure), the tonemapper
// (Common/Tonemapping.slang), the grading LUT (M26), the sRGB OETF and the blue-noise triangular dither. Alpha is 1.

#include "Shared/ExposureConstants.h"
#include "Shared/TonemapConstants.h"
#include "Shared/ViewConstants.h"

import Common.Color;
import Common.Tonemapping;

ConstantBuffer<ViewConstants> View : register(b0, space0);
[[vk::push_constant]] ConstantBuffer<TonemapConstants> Settings;
Texture2D<float4> SceneColor : register(t0, space0);
// The bloom chain (BloomPass), bound at its mip 0; SceneColor stands in when TonemapFlagBloom is clear.
Texture2D<float4> Bloom : register(t1, space0);
// The 64x64 R8 blue-noise texture (§8.4 t7), tiled by pixel coordinate; SceneColor stands in when TonemapFlagDither is clear.
Texture2D<float> BlueNoise : register(t7, space0);
// M26 (R-AEX; Docs/Decisions/0037-m26-contract.md decision 15): the Exposure pass's adapted exposure (Renderer/ExposurePass.h),
// read only with TonemapFlagAutoExposure; the pass's own stand-in buffer is bound without one.
StructuredBuffer<ExposureState> AdaptedExposure : register(t8, space0);
// M26 (Docs/Decisions/0037-m26-contract.md decision 15): the grading LUT (GpuLut::Texture), its entries display-encoded; the
// pass's own 1-texel 3D texture stands in when TonemapFlagLut is clear.
Texture3D<float4> Lut : register(t9, space0);
SamplerState LinearClamp : register(s0, space0);
[vk::image_format("rgba8")]
RWTexture2D<float4> LdrColor : register(u0, space0);

// The largest finite binary16 value: SceneColor's range. Exposed radiance is clamped to it (after NaN is replaced by 0), so
// a non-finite or exposed-out-of-range value saturates instead of reaching the tonemappers' arithmetic as Inf or NaN; every
// tonemapper is already at its white point there.
static const float MaxRadiance = 65504.0;

// The triangular distribution on (-1, 1) from a uniform value in (0, 1) (the inverse of its cumulative distribution).
float UniformToTriangular(float uniform)
{
	const float signedUniform = uniform * 2.0 - 1.0;
	return sign(signedUniform) * (1.0 - sqrt(1.0 - abs(signedUniform)));
}

// `encoded` with +-1 LSB of triangular dither from the blue-noise texel of `pixel`. Quantization rounds to the nearest of
// 256 levels, so the result stays within one level of the undithered one. The amplitude fades to 0 within the first and
// last level, so exact black and white (cleared or clipped pixels) stay exact: they show no banding to hide.
float3 Dither(float3 encoded, uint2 pixel)
{
	uint width = 0;
	uint height = 0;
	BlueNoise.GetDimensions(width, height);
	const float rank = round(BlueNoise[pixel % uint2(width, height)] * 255.0);
	const float noise = UniformToTriangular((rank + 0.5) / 256.0);
	const float3 amplitude = saturate(min(encoded, 1.0 - encoded) * 255.0);
	return encoded + noise * amplitude / 255.0;
}

// The exposure multiplier: the view's 2^ExposureEV, times the adapted exposure with TonemapFlagAutoExposure (where
// ExposureEV is the compensation).
float GetExposure()
{
	if ((Settings.Flags & TonemapFlagAutoExposure) != 0)
		return View.Exposure * AdaptedExposure[0].Multiplier;
	return View.Exposure;
}

// M26 (decision 15): the LUT step on a tonemapped, display-linear value. The table is addressed by the value encoded with the
// sRGB OETF (as grading LUTs are authored), mapped to the 3D texture's coordinates by LutScale and LutOffset (the domain and
// the texel centres, TonemapConstants), sampled trilinearly (LinearClamp clamps a value outside the domain), decoded with the
// EOTF, and blended with the ungraded value by LutIntensity.
float3 ApplyLut(float3 display)
{
	const float3 coordinate = LinearToSrgb(display) * Settings.LutScale.xyz + Settings.LutOffset.xyz;
	const float3 graded = SrgbToLinear(Lut.SampleLevel(LinearClamp, coordinate, 0.0).rgb);
	return lerp(display, graded, Settings.LutIntensity);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID)
{
	const uint2 pixel = threadID.xy;
	if (pixel.x >= uint(View.ViewportSize.x) || pixel.y >= uint(View.ViewportSize.y))
		return;
	float3 radiance = SceneColor[pixel].rgb;
	if ((Settings.Flags & TonemapFlagBloom) != 0)
	{
		const float2 uv = (float2(pixel) + 0.5) * View.InverseViewportSize;
		radiance = lerp(radiance, Bloom.SampleLevel(LinearClamp, uv, 0.0).rgb, Settings.BloomIntensity);
	}
	const float3 exposed = radiance * GetExposure();
	const float3 finite = select(isnan(exposed), float3(0.0), exposed);
	float3 display = ApplyTonemapper(Settings.Tonemapper, clamp(finite, 0.0, MaxRadiance));
	if ((Settings.Flags & TonemapFlagLut) != 0)
		display = ApplyLut(display);
	float3 encoded = (Settings.Flags & TonemapFlagEncodeSrgb) != 0 ? LinearToSrgb(display) : display;
	if ((Settings.Flags & TonemapFlagDither) != 0)
		encoded = Dither(encoded, pixel);
	LdrColor[pixel] = float4(encoded, 1.0);
}
