// Smoke: the minimal real program that exercises the shader pipeline end to end (Roadmap M0). It scales a buffer of
// floats, optionally clamping the result to [0, 1] (permutation SMOKE_SATURATE).

#include "Shared/SmokeConstants.h"

ConstantBuffer<SmokeConstants> Smoke : register(b0, space0);
RWStructuredBuffer<float> Values : register(u0, space0);

[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	const uint index = dispatchThreadID.x;
	if (index >= Smoke.ElementCount)
		return;

	float value = Values[index] * Smoke.Scale;
#if SMOKE_SATURATE
	value = saturate(value);
#endif
	Values[index] = value;
}
