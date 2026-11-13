// Exposure: histogram auto-exposure (M26, R-AEX; Docs/Decisions/0037-m26-contract.md decision 15; Renderer/ExposurePass.h),
// the GPU half of Renderer/AutoExposure.h's CPU statement, from the shared numbers of Shared/ExposureConstants.h:
//   - CSHistogram: every pixel of the image Tonemap exposes (SceneColor with the bloom composite, before the exposure) adds
//     one to the bin of its Rec. 709 luminance, first into the group's bins in shared memory, then each group's non-empty
//     bins into the histogram (u0, cleared by the pass before). Integer atomic adds only: no order of the adds changes a
//     count, so the histogram equals AutoExposure.h's ComputeLuminanceHistogram exactly. The luminance's products and sums
//     are NoContraction, so no fused multiply-add changes a bit, and its bin is found by comparing with the shared edges,
//     never by a log2.
//   - CSAdapt: one thread computes the target from the histogram (the metered window's mean log2 luminance at the bins'
//     centres, mapped to middle grey and clamped to the range) and the adapted exposure from the previous frame's (u1), as
//     ComputeExposureTarget and AdaptExposure state it, with their IEEE operations in their order (only the division and
//     exp2 may round differently from the CPU's), and writes it with its multiplier for the Tonemap pass.

#include "Shared/ExposureConstants.h"

[[vk::push_constant]] ConstantBuffer<ExposureConstants> Settings;
Texture2D<float4> SceneColor : register(t0, space0);
// The bloom chain (BloomPass), bound at its mip 0; SceneColor stands in when ExposureFlagBloom is clear.
Texture2D<float4> Bloom : register(t1, space0);
SamplerState LinearClamp : register(s0, space0);
RWStructuredBuffer<uint> Histogram : register(u0, space0);
RWStructuredBuffer<ExposureState> Exposure : register(u1, space0);

// The threads of one CSHistogram group along each axis: 256 threads, at least one per bin.
static const uint HistogramGroupSize = 16;

groupshared uint GroupBins[ExposureHistogramBinCount];

// a * b with SPIR-V's NoContraction: never fused with the operations around it (Scene.slang's MultiplyExact explains why
// the decoration is written in SPIR-V assembly).
float MultiplyExact(float a, float b)
{
	return spirv_asm {
		%product:$$float = OpFMul $a $b;
		OpDecorate %product NoContraction;
		result:$$float = OpCopyObject %product
	};
}

// a + b with SPIR-V's NoContraction.
float AddExact(float a, float b)
{
	return spirv_asm {
		%sum:$$float = OpFAdd $a $b;
		OpDecorate %sum NoContraction;
		result:$$float = OpCopyObject %sum
	};
}

// The bin of `radiance`'s luminance (AutoExposure.h's GetLuminanceHistogramBin): (0.2126 r + 0.7152 g) + 0.0722 b, bin 0 for
// a non-finite luminance, else the last bin whose lower edge is at most it, by a binary search over the shared edges.
uint GetLuminanceBin(float3 radiance)
{
	const float red = MultiplyExact(asfloat(ExposureLuminanceWeightBits[0]), radiance.r);
	const float green = MultiplyExact(asfloat(ExposureLuminanceWeightBits[1]), radiance.g);
	const float blue = MultiplyExact(asfloat(ExposureLuminanceWeightBits[2]), radiance.b);
	const float luminance = AddExact(AddExact(red, green), blue);
	if (isnan(luminance) || isinf(luminance))
		return 0;
	uint bin = 0;
	for (uint step = ExposureHistogramBinCount / 2; step > 0; step /= 2)
	{
		if (luminance >= asfloat(ExposureHistogramEdgeBits[bin + step]))
			bin += step;
	}
	return bin;
}

[shader("compute")]
[numthreads(HistogramGroupSize, HistogramGroupSize, 1)]
void CSHistogram(uint3 threadID : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
	if (groupIndex < ExposureHistogramBinCount)
		GroupBins[groupIndex] = 0;
	GroupMemoryBarrierWithGroupSync();
	const uint2 pixel = threadID.xy;
	if (pixel.x < Settings.ImageSize.x && pixel.y < Settings.ImageSize.y)
	{
		float3 radiance = SceneColor[pixel].rgb;
		if ((Settings.Flags & ExposureFlagBloom) != 0)
		{
			const float2 uv = (float2(pixel) + 0.5) * Settings.InverseImageSize;
			radiance = lerp(radiance, Bloom.SampleLevel(LinearClamp, uv, 0.0).rgb, Settings.BloomIntensity);
		}
		InterlockedAdd(GroupBins[GetLuminanceBin(radiance)], 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	if (groupIndex < ExposureHistogramBinCount && GroupBins[groupIndex] != 0)
		InterlockedAdd(Histogram[groupIndex], GroupBins[groupIndex]);
}

// The centre of bin `bin` in log2 luminance: -12 + (bin + 0.5) / 2, exact in float.
float GetBinCentreLog2(uint bin)
{
	return AddExact(-12.0, MultiplyExact(float(bin) + 0.5, 0.5));
}

// AutoExposure.h's ComputeExposureTarget: 0 for an empty histogram.
float ComputeExposureTarget()
{
	uint total = 0;
	for (uint bin = 0; bin < ExposureHistogramBinCount; ++bin)
		total += Histogram[bin];
	if (total == 0)
		return 0.0;
	const uint low = total / ExposureLowRankDivisor;
	const uint high = total - total / ExposureHighRankDivisor;
	uint below = 0;
	uint metered = 0;
	float sum = 0.0;
	for (uint bin = 0; bin < ExposureHistogramBinCount; ++bin)
	{
		const uint count = Histogram[bin];
		const uint start = max(below, low);
		const uint end = min(below + count, high);
		if (end > start)
		{
			sum = AddExact(sum, MultiplyExact(float(end - start), GetBinCentreLog2(bin)));
			metered += end - start;
		}
		below += count;
	}
	const float mean = sum / float(metered);
	const float target = asfloat(ExposureMiddleGreyLog2Bits) - mean;
	return min(max(target, Settings.MinEV), Settings.MaxEV);
}

// AutoExposure.h's AdaptExposure.
float AdaptExposure(float current, float target, bool reset)
{
	if (reset || isnan(current) || isinf(current))
		return target;
	if (target > current)
	{
		const float step = MultiplyExact(Settings.SpeedUp, Settings.DeltaTime);
		return target - current <= step ? target : AddExact(current, step);
	}
	if (target < current)
	{
		const float step = MultiplyExact(Settings.SpeedDown, Settings.DeltaTime);
		return current - target <= step ? target : current - step;
	}
	return target;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void CSAdapt()
{
	const float target = ComputeExposureTarget();
	const float adapted = AdaptExposure(Exposure[0].AdaptedEV, target, (Settings.Flags & ExposureFlagReset) != 0);
	ExposureState state;
	state.AdaptedEV = adapted;
	state.Multiplier = exp2(adapted);
	state.TargetEV = target;
	state.Padding0 = 0;
	Exposure[0] = state;
}
