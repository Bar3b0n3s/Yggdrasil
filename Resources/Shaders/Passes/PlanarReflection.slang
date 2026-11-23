// PlanarReflection: the mip chain of a planar reflection's colour (M42, Docs/Decisions/0059-m42-planar-reflections.md
// decision 3; Renderer/PlanarReflectionPass.h). One dispatch per level after the first: each texel of level k is the mean of
// level k - 1 over its span, weighted by area. A destination of D texels on an axis of S source texels (D = max(1, S / 2))
// gives its texel i the span [i S / D, (i + 1) S / D), so the texels [floor(i S / D), ceil((i + 1) S / D)) take part (2 for
// an even S, up to 3 for an odd one, DepthPyramid's footprint rule) with the length of each one's overlap, and every source
// texel counts, an odd level's last row and column included. So the forward passes' level curve
// (Common/PlanarReflections.slang) blurs the glossy reflection as a probe's cube mips blur its radiance, centred on the sharp
// image at every level. Pre-exposure linear HDR radiance (SceneColorFormat) in, the same out.

#include "Shared/PlanarReflectionConstants.h"

[[vk::push_constant]] ConstantBuffer<PlanarReflectionMipConstants> Constants;
Texture2D<float4> Source : register(t0, space0);
[vk::image_format("rgba16f")]
RWTexture2D<float4> Destination : register(u0, space0);

// The share of destination texel `index` that source texel `texel` covers on an axis reducing `source` texels to
// `destination`: the overlap of [texel, texel + 1) with the span [index * source / destination, (index + 1) * source /
// destination), counted in 1 / destination of a texel (integers, exact), over the span's length source / destination.
float GetMipWeight(uint texel, uint index, uint source, uint destination)
{
	const uint low = max(texel * destination, index * source);
	const uint high = min((texel + 1) * destination, (index + 1) * source);
	return float(high - low) / float(source);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Constants.DestinationSize))
		return;
	const uint2 first = id.xy * Constants.SourceSize / Constants.DestinationSize;
	const uint2 last = ((id.xy + 1) * Constants.SourceSize + Constants.DestinationSize - 1) / Constants.DestinationSize;
	float4 sum = 0.0;
	for (uint y = first.y; y < last.y; ++y)
	{
		float4 row = 0.0;
		for (uint x = first.x; x < last.x; ++x)
			row += GetMipWeight(x, id.x, Constants.SourceSize.x, Constants.DestinationSize.x) * Source.Load(int3(x, y, 0));
		sum += GetMipWeight(y, id.y, Constants.SourceSize.y, Constants.DestinationSize.y) * row;
	}
	Destination[id.xy] = sum;
}
