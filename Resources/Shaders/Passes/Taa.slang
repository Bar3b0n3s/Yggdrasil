// Taa: temporal anti-aliasing's resolve (M26, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10 and 17;
// Renderer/TaaPass.h), first in the HdrPost stage. One thread per pixel of the jittered HDR SceneColor (t0):
//   - with TaaFlagReset the resolved image is the current pixel, exactly (the history starts afresh);
//   - otherwise the pixel's previous position, its centre minus its motion vector (SceneVelocity, t1: current minus previous
//     position in scene pixels), samples the view's previous resolved image (t2) bilinearly; the sample is clamped, in
//     YCoCg, to the box of the current 3 x 3 neighbourhood (clamped to the image; non-finite neighbours left out), and the
//     resolved pixel is lerp(current, clamped history, Feedback). A previous position outside the image, or a non-finite
//     motion, history or current value, resolves to the current pixel.
// No random term and no atomic: each pixel is written once from fixed inputs, so a resolve is deterministic on a device, and
// a capture's warm-up from a reset history gives the same image every time (decision 10). Alpha is the current pixel's.

#include "Shared/TaaConstants.h"

[[vk::push_constant]] ConstantBuffer<TaaConstants> Settings;
Texture2D<float4> SceneColor : register(t0, space0);
Texture2D<float2> Velocity : register(t1, space0);
// The previous resolved image; SceneColor stands in with TaaFlagReset, which never samples it.
Texture2D<float4> History : register(t2, space0);
SamplerState LinearClamp : register(s0, space0);
[vk::image_format("rgba16f")]
RWTexture2D<float4> Resolved : register(u0, space0);

float3 RgbToYCoCg(float3 color)
{
	return float3(0.25 * color.r + 0.5 * color.g + 0.25 * color.b, 0.5 * color.r - 0.5 * color.b, -0.25 * color.r + 0.5 * color.g - 0.25 * color.b);
}

float3 YCoCgToRgb(float3 color)
{
	return float3(color.x + color.y - color.z, color.x + color.z, color.x - color.y - color.z);
}

bool IsFinite(float3 value)
{
	return all(isfinite(value));
}

// The resolved value of `pixel`, whose current value is `current`.
float4 Resolve(int2 pixel, float4 current)
{
	if ((Settings.Flags & TaaFlagReset) != 0 || !IsFinite(current.rgb))
		return current;
	const float2 motion = Velocity.Load(int3(pixel, 0));
	const float2 previous = (float2(pixel) + 0.5 - motion) * Settings.InverseImageSize;
	if (!all(isfinite(motion)) || any(previous < 0.0) || any(previous > 1.0))
		return current;
	const float3 history = History.SampleLevel(LinearClamp, previous, 0.0).rgb;
	if (!IsFinite(history))
		return current;

	// The current neighbourhood's box in YCoCg.
	const int2 last = int2(Settings.ImageSize) - 1;
	float3 low = RgbToYCoCg(current.rgb);
	float3 high = low;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			const float3 neighbour = SceneColor.Load(int3(clamp(pixel + int2(x, y), int2(0), last), 0)).rgb;
			if (!IsFinite(neighbour))
				continue;
			const float3 ycocg = RgbToYCoCg(neighbour);
			low = min(low, ycocg);
			high = max(high, ycocg);
		}
	}
	const float3 clamped = YCoCgToRgb(clamp(RgbToYCoCg(history), low, high));
	return float4(lerp(current.rgb, clamped, Settings.Feedback), current.a);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Settings.ImageSize))
		return;
	const int2 pixel = int2(id.xy);
	Resolved[pixel] = Resolve(pixel, SceneColor.Load(int3(pixel, 0)));
}
