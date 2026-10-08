// BrdfLut: the DFG LUT of the split-sum IBL with multi-scatter energy compensation (Architecture §8.5, §8.6 step 5;
// Renderer/BrdfLut.h, which states the content). Texel (i, j) of the BrdfLutSize² RG16_FLOAT target holds, for
// NdotV = (i + 0.5) / BrdfLutSize and perceptual roughness (j + 0.5) / BrdfLutSize (alpha = roughness²),
//   R = DFG1 = the mean of Fc * Gv and G = DFG2 = the mean of Gv
// over BrdfLutSampleCount GGX importance samples of the Hammersley sequence, with V = (sqrt(1 - NdotV²), 0, NdotV) in
// tangent space (N = +Z), L the reflection of V about each sampled half vector H, Fc = (1 - V.H)^5 and
// Gv = Vis(NdotV, NdotL) * 4 NdotL V.H / NdotH (samples below the horizon contribute 0). One thread per texel.

#include "Shared/BrdfLutConstants.h"

import Common.Brdf;

[vk::image_format("rg16f")]
RWTexture2D<float2> Lut : register(u0, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID)
{
	const uint2 texel = threadID.xy;
	if (texel.x >= BrdfLutSize || texel.y >= BrdfLutSize)
		return;
	const float nDotV = (float(texel.x) + 0.5) / float(BrdfLutSize);
	const float perceptualRoughness = (float(texel.y) + 0.5) / float(BrdfLutSize);
	const float alpha = perceptualRoughness * perceptualRoughness;
	const float3 view = float3(sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);

	float2 sum = float2(0.0);
	for (uint index = 0; index < BrdfLutSampleCount; ++index)
	{
		const float3 halfVector = ImportanceSampleGgx(Hammersley(index, BrdfLutSampleCount), alpha);
		const float vDotH = dot(view, halfVector);
		const float3 light = 2.0 * vDotH * halfVector - view;
		const float nDotL = saturate(light.z);
		const float nDotH = saturate(halfVector.z);
		if (nDotL > 0.0 && nDotH > 0.0)
		{
			const float clampedVDotH = saturate(vDotH);
			const float gv = VisibilitySmithGgxCorrelated(nDotV, nDotL, alpha) * 4.0 * nDotL * clampedVDotH / nDotH;
			sum += float2(FresnelWeight(clampedVDotH) * gv, gv);
		}
	}
	Lut[texel] = sum / float(BrdfLutSampleCount);
}
