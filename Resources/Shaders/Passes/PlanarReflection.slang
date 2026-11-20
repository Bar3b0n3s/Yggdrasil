// PlanarReflection: the mip chain of a planar reflection's colour (M42, Docs/Decisions/0059-m42-planar-reflections.md
// decision 3; Renderer/PlanarReflectionPass.h). One dispatch per level after the first: each texel of level k is the mean of
// the 2 x 2 texels of level k - 1 it covers (the last row or column of an odd level clamped to the level's edge), so the
// forward passes' level curve (Common/PlanarReflections.slang) blurs the glossy reflection as a probe's cube mips blur its
// radiance. Pre-exposure linear HDR radiance (SceneColorFormat) in, the same out.

#include "Shared/PlanarReflectionConstants.h"

[[vk::push_constant]] ConstantBuffer<PlanarReflectionMipConstants> Constants;
Texture2D<float4> Source : register(t0, space0);
[vk::image_format("rgba16f")]
RWTexture2D<float4> Destination : register(u0, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Constants.DestinationSize))
		return;
	const int2 last = int2(Constants.SourceSize) - 1;
	const int2 origin = int2(id.xy) * 2;
	const float4 sum = Source.Load(int3(min(origin, last), 0)) + Source.Load(int3(min(origin + int2(1, 0), last), 0))
		+ Source.Load(int3(min(origin + int2(0, 1), last), 0)) + Source.Load(int3(min(origin + int2(1, 1), last), 0));
	Destination[id.xy] = sum * 0.25;
}
