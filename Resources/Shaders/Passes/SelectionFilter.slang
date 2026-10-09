#include "Shared/Private/SelectionPassConstants.h"

import Common.Color;

Texture2D<float> Source : register(t0, space0);
Texture2D<float> PackedHorizontal : register(t1, space0);
[[vk::push_constant]] ConstantBuffer<SelectionFilterConstants> Filter;
[vk::image_format("r8")]
RWTexture2D<float> MaskDestination : register(u0, space0);
[vk::image_format("rgba8")]
RWTexture2D<float4> ColorDestination : register(u1, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDilate(uint3 thread : SV_DispatchThreadID)
{
	uint width, height;
	MaskDestination.GetDimensions(width, height);
	if (thread.x >= width || thread.y >= height)
		return;
	const int2 pixel = int2(thread.xy);
	const int2 last = int2(width, height) - 1;
	uint maximum = 0;
	for (int offset = -int(Filter.Radius); offset <= int(Filter.Radius); ++offset)
	{
		const int2 samplePixel = clamp(pixel + (Filter.Vertical != 0 ? int2(0, offset) : int2(offset, 0)), int2(0), last);
		const float value = Source.Load(int3(samplePixel, 0));
		const uint code = Filter.Vertical != 0 ? (uint(round(value * 255.0)) >> 2) : uint(round(value * 2.0));
		maximum = max(maximum, code);
	}
	// Keep the original coverage in two low bits of Scratch, so the vertical pass can overwrite Mask without losing
	// the original silhouette. R8_UNORM preserves all integer codes exactly after round(value * 255).
	const uint original = uint(round(Source.Load(int3(pixel, 0)) * 2.0));
	MaskDestination[pixel] = Filter.Vertical != 0 ? float(maximum) * 0.5 : float((maximum << 2) | original) / 255.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSComposite(uint3 thread : SV_DispatchThreadID)
{
	uint width, height;
	ColorDestination.GetDimensions(width, height);
	if (thread.x >= width || thread.y >= height)
		return;
	const int2 pixel = int2(thread.xy);
	const uint original = uint(round(PackedHorizontal.Load(int3(pixel, 0)) * 255.0)) & 3;
	const float edge = original == 0 ? Source.Load(int3(pixel, 0)) : 0.0;
	const float alpha = saturate(Filter.Color.a * edge);
	const float4 background = ColorDestination[pixel];
	ColorDestination[pixel] = float4(lerp(background.rgb, LinearToSrgb(Filter.Color.rgb), alpha), alpha + background.a * (1.0 - alpha));
}
