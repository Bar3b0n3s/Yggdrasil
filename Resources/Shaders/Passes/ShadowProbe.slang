#include "Shared/ShadowProbeConstants.h"

#include "Common/Shadows.slang"

ConstantBuffer<ShadowConstants> Shadows : register(b1, space0);
Texture2DArray<float> Cascades : register(t1, space0);
Texture2D<float> Atlas : register(t2, space0);
SamplerComparisonState Comparison : register(s1, space0);
[[vk::push_constant]] ConstantBuffer<ShadowProbeConstants> Probe;
[[vk::image_format("rgba32f")]] RWTexture2D<float4> Output : register(u0, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
	if (any(thread.xy >= Probe.Options.zw))
		return;
	float4 result = 0.0;
	if (Probe.Options.x == 0)
		result = float4(Cascades.Load(int4(thread.xy, Probe.Options.y, 0)), 0, 0, 1);
	else if (Probe.Options.x == 1)
		result = float4(Atlas.Load(int3(thread.xy, 0)), 0, 0, 1);
	else if (Probe.Options.x == 2)
		result = float4(ShadowCascadeColor(Shadows, Probe.Parameters.y + float(thread.x) * Probe.Parameters.x), 1);
	else
	{
		float2 xy = (float2(thread.xy) + 0.5) / float2(Probe.Options.zw) * float2(2.0, -2.0) + float2(-1.0, 1.0);
		float visibility;
		if (Probe.Options.x == 3)
			visibility = SampleDirectionalShadow(Cascades, Comparison, Shadows, float3(xy, Probe.Parameters.z), float3(0, 0, 1), float3(0, 0, 1), Probe.Parameters.y, thread.xy);
		else
			visibility = SampleSpotShadow(Atlas, Comparison, Shadows, Probe.Options.y, float3(xy * Probe.Parameters.z, -Probe.Parameters.z), float3(0, 0, 1), float3(0, 0, 1), thread.xy);
		result = float4(visibility, visibility, visibility, 1);
	}
	Output[thread.xy] = result;
}
