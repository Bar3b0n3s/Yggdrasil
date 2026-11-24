// Sky: M45's procedural sky (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Renderer/SkyLighting.h,
// Renderer/SkyPass.h). The constants are Shared/SkyConstants.h's.
//
// The test entries CSTestCopyWrite and CSTestCopyRead are the GPU proof of the copy into a view's constant buffers ("SkyLighting:
// a buffer copied into a view's constant buffers after their CPU writes reaches the passes that read them"): the first writes
// a SkyResults buffer as the sky's computes do, the second reads the constant buffers the copies reached, at the forward view's
// registers, into a buffer the test reads back. No pass creates a pipeline for them; their layouts live in
// Tests/Source/Support/SkyCopyProgram.h.

#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/SkyConstants.h"

ConstantBuffer<SkyConstants> Sky : register(b1, space0);
ConstantBuffer<EnvironmentConstants> Environment : register(b2, space0);
ConstantBuffer<ProbeConstants> Probes : register(b4, space0);
ConstantBuffer<FogConstants> Fog : register(b5, space0);
RWStructuredBuffer<float4> TestResults : register(u6, space0);
RWStructuredBuffer<float4> TestReadback : register(u7, space0);

// The number of float4 elements CSTestCopyRead writes: EnvironmentConstants' twelve, FogConstants' first two, and each
// reflection probe's AxisScale and Lookup.
static const uint SkyTestReadbackCount = 14 + 2 * MaxViewReflectionProbes;

// Element i of SkyResults, component c, is Sky.Intensity + 4 i + c: distinct values that follow the frame's constants.
[shader("compute")]
[numthreads(32, 1, 1)]
void CSTestCopyWrite(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	const uint index = dispatchThreadID.x;
	if (index >= SkyResultsCount)
		return;
	const float base = Sky.Intensity + 4.0 * float(index);
	TestResults[index] = float4(base, base + 1.0, base + 2.0, base + 3.0);
}

// The constant buffers as the passes see them: EnvironmentConstants' members in order (the nine SH9, then AmbientColor with
// Intensity, the rotation with the lods, and the flag's bits), FogConstants' Color with Density and CameraPosition with
// StartDistance, then each reflection probe's AxisScale and Lookup.
[shader("compute")]
[numthreads(1, 1, 1)]
void CSTestCopyRead(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x != 0)
		return;
	for (uint index = 0; index < 9; ++index)
		TestReadback[index] = Environment.IrradianceSH9[index];
	TestReadback[9] = float4(Environment.AmbientColor, Environment.Intensity);
	TestReadback[10] = float4(Environment.RotationSin, Environment.RotationCos, Environment.SpecularMaxLod, Environment.SkyboxLod);
	TestReadback[11] = float4(asfloat(Environment.HasEnvironment), asfloat(Environment.Padding0), asfloat(Environment.Padding1), asfloat(Environment.Padding2));
	TestReadback[12] = float4(Fog.Color, Fog.Density);
	TestReadback[13] = float4(Fog.CameraPosition, Fog.StartDistance);
	for (uint probe = 0; probe < MaxViewReflectionProbes; ++probe)
	{
		TestReadback[14 + 2 * probe] = Probes.Reflections[probe].AxisScale;
		TestReadback[15 + 2 * probe] = Probes.Reflections[probe].Lookup;
	}
}
