// Bloom: §8.3 pass 10 (Renderer/BloomPass.h). One dispatch per step of the chain, each reading one level through Source
// (bilinear, LinearClamp) and writing one mip of the chain through Destination:
//   - CSDownsampleKaris: SceneColor into mip 0, Jimenez's 13-tap downsample with the Karis average;
//   - CSDownsample: mip i into mip i + 1, the 13-tap downsample;
//   - CSUpsample: mip i + 1 into mip i, mip i = (mip i + tent(mip i + 1)) * Scale with a 3x3 tent of bilinear taps, Scale
//     being 1 / the chain's mip count for the step into mip 0 and 1 otherwise.
// Permutation BLOOM_RGBA16 selects the chain's storage format: R11G11B10_FLOAT, or the RGBA16_FLOAT fallback (§8.1).

#include "Shared/BloomConstants.h"

[[vk::push_constant]] ConstantBuffer<BloomConstants> Constants;
Texture2D<float4> Source : register(t0, space0);
SamplerState LinearClamp : register(s0, space0);
#if BLOOM_RGBA16
[vk::image_format("rgba16f")]
#else
[vk::image_format("r11f_g11f_b10f")]
#endif
RWTexture2D<float4> Destination : register(u0, space0);

// The largest radiance the first downsample passes on: R11G11B10's largest finite value (65024) divided by the longest
// chain (BloomPass::MaxMipCount, 6), so the upsample's running sums, which reach the chain's mip count times the input,
// stay finite in both storage formats. Non-finite SceneColor texels (NaN, Inf) would otherwise spread over the whole chain.
static const float MaxBloomRadiance = 65024.0 / 6.0;

float3 SampleSource(float2 uv)
{
	return Source.SampleLevel(LinearClamp, uv, 0.0).rgb;
}

// A SceneColor tap for the first downsample: NaN replaced by 0, clamped to [0, MaxBloomRadiance].
float3 SampleRadiance(float2 uv)
{
	const float3 value = SampleSource(uv);
	return clamp(select(isnan(value), float3(0.0), value), 0.0, MaxBloomRadiance);
}

// Rec. 709 luminance, for the Karis average's weights.
float Luminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Jimenez's 13-tap downsample (Next Generation Post Processing in Call of Duty: Advanced Warfare, 2014) around the
// destination texel at `uv`: 13 bilinear taps form five overlapping 2x2-texel boxes, the centre one weighing 0.5 and the four
// corner ones 0.125 each. With `karis`, each box's weight is divided by 1 + its luminance and the result is normalized by
// the weights' sum (Karis's average, which keeps lone fireflies from flickering through the chain); equal texels give the
// texel either way.
float3 Downsample(float2 uv, bool karis)
{
	const float2 texel = Constants.SourceTexelSize;
	float3 taps[13];
	const float2 offsets[13] = {
		float2(-2.0, -2.0), float2(0.0, -2.0), float2(2.0, -2.0),
		float2(-2.0, 0.0), float2(0.0, 0.0), float2(2.0, 0.0),
		float2(-2.0, 2.0), float2(0.0, 2.0), float2(2.0, 2.0),
		float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
	};
	for (int index = 0; index < 13; ++index)
	{
		const float2 tapUv = uv + offsets[index] * texel;
		taps[index] = karis ? SampleRadiance(tapUv) : SampleSource(tapUv);
	}
	// Taps 0 to 8 are the outer 3x3 grid (a b c / d e f / g h i), taps 9 to 12 the inner corners (j k / l m).
	const float3 boxes[5] = {
		(taps[9] + taps[10] + taps[11] + taps[12]) * 0.25,
		(taps[0] + taps[1] + taps[3] + taps[4]) * 0.25,
		(taps[1] + taps[2] + taps[4] + taps[5]) * 0.25,
		(taps[3] + taps[4] + taps[6] + taps[7]) * 0.25,
		(taps[4] + taps[5] + taps[7] + taps[8]) * 0.25,
	};
	const float boxWeights[5] = { 0.5, 0.125, 0.125, 0.125, 0.125 };
	float3 sum = float3(0.0);
	float weightSum = 0.0;
	for (int box = 0; box < 5; ++box)
	{
		const float weight = karis ? boxWeights[box] / (1.0 + Luminance(boxes[box])) : boxWeights[box];
		sum += boxes[box] * weight;
		weightSum += weight;
	}
	return sum / weightSum;
}

bool IsOutside(uint2 texel)
{
	return texel.x >= Constants.DestinationSize.x || texel.y >= Constants.DestinationSize.y;
}

float2 GetDestinationUv(uint2 texel)
{
	return (float2(texel) + 0.5) * Constants.DestinationTexelSize;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDownsampleKaris(uint3 threadID : SV_DispatchThreadID)
{
	if (IsOutside(threadID.xy))
		return;
	Destination[threadID.xy] = float4(Downsample(GetDestinationUv(threadID.xy), true), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDownsample(uint3 threadID : SV_DispatchThreadID)
{
	if (IsOutside(threadID.xy))
		return;
	Destination[threadID.xy] = float4(Downsample(GetDestinationUv(threadID.xy), false), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSUpsample(uint3 threadID : SV_DispatchThreadID)
{
	if (IsOutside(threadID.xy))
		return;
	const float2 uv = GetDestinationUv(threadID.xy);
	const float2 texel = Constants.SourceTexelSize;
	// The 3x3 tent (1 2 1 / 2 4 2 / 1 2 1) / 16 of bilinear taps one source texel apart.
	float3 tent = SampleSource(uv) * 4.0;
	tent += (SampleSource(uv + float2(-texel.x, 0.0)) + SampleSource(uv + float2(texel.x, 0.0)) + SampleSource(uv + float2(0.0, -texel.y))
				+ SampleSource(uv + float2(0.0, texel.y)))
		* 2.0;
	tent += SampleSource(uv - texel) + SampleSource(uv + texel) + SampleSource(uv + float2(texel.x, -texel.y)) + SampleSource(uv + float2(-texel.x, texel.y));
	tent *= 1.0 / 16.0;
	const float3 current = Destination[threadID.xy].rgb;
	Destination[threadID.xy] = float4((current + tent) * Constants.Scale, 1.0);
}
