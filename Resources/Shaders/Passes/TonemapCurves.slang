// TonemapCurves: the GPU side of "TonemapPass: every tonemapper matches its CPU reference at 1,024 points" (§8.9, §15.3).
// Every element of Values holds an exposed linear colour in xyz and is replaced by the display-linear result of the
// tonemapper Settings.Tonemapper (Common/Tonemapping.slang, the functions the Tonemap pass applies), in 32-bit float, so the
// curves are compared without the 8-bit quantization of LdrColor. No engine pass uses it; its pipeline layout lives in
// Tests/Source/Support/TonemapCurvesProgram.h.

#include "Shared/TonemapConstants.h"

import Common.Tonemapping;

[[vk::push_constant]] ConstantBuffer<TonemapConstants> Settings;
RWStructuredBuffer<float4> Values : register(u0, space0);

[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID)
{
	uint count = 0;
	uint stride = 0;
	Values.GetDimensions(count, stride);
	const uint index = threadID.x;
	if (index >= count)
		return;
	// The Tonemap pass hands the tonemappers c = max(exposed radiance, 0) (Renderer/TonemapPass.h step 3).
	Values[index] = float4(ApplyTonemapper(Settings.Tonemapper, max(Values[index].xyz, float3(0.0))), 1.0);
}
