// Tonemap: §8.3 pass 11, the walking skeleton's subset (Roadmap M7; Renderer/SceneRenderer; Docs/Decisions/0012-m7-decisions.md
// decision 7). LdrColor (RGBA8_UNORM, display-encoded values, §8.9) = sRGB OETF(clamp(SceneColor * 2^ExposureEV, 0, 1)): the
// Linear tonemapper (clamp). AgX, ACES and PBR Neutral, the blue-noise dither, bloom and FXAA arrive with M8. Alpha is 1.

#include "Shared/ViewConstants.h"

import Common.Color;

ConstantBuffer<ViewConstants> View : register(b0, space0);
Texture2D<float4> SceneColor : register(t0, space0);
[vk::image_format("rgba8")]
RWTexture2D<float4> LdrColor : register(u0, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID)
{
	const uint2 pixel = threadID.xy;
	if (pixel.x >= uint(View.ViewportSize.x) || pixel.y >= uint(View.ViewportSize.y))
		return;
	const float3 exposed = SceneColor[pixel].rgb * View.Exposure;
	LdrColor[pixel] = float4(LinearToSrgb(saturate(exposed)), 1.0);
}
