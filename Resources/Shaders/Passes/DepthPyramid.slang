#include "Shared/DepthPyramidConstants.h"

[[vk::push_constant]] ConstantBuffer<DepthPyramidConstants> Constants;
Texture2D<float> Source : register(t0, space0);
[vk::image_format("r16f")]
RWTexture2D<float> Destination : register(u0, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void CSLinearize(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Constants.DestinationSize))
		return;
	const float depth = Source.Load(int3(id.xy, 0));
	float linearDepth = 65504.0;
	if (depth > 0.0 && isfinite(depth))
	{
		// Test before dividing, including denormal reverse depths, to keep the intermediate finite.
		linearDepth = Constants.ProjectionKind == 0
			? (depth <= Constants.Near / 65504.0 ? 65504.0 : Constants.Near / depth)
			: lerp(Constants.Far, Constants.Near, saturate(depth));
	}
	Destination[id.xy] = clamp(linearDepth, 0.0, 65504.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSReduce(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Constants.DestinationSize))
		return;
	// Integer products are safe for Vulkan's image extents. The public CPU oracle uses uint64 for arbitrary inputs.
	const uint2 first = id.xy * Constants.SourceSize / Constants.DestinationSize;
	const uint2 last = ((id.xy + 1) * Constants.SourceSize + Constants.DestinationSize - 1) / Constants.DestinationSize;
	float nearest = 65504.0;
	for (uint y = first.y; y < last.y; ++y)
	{
		for (uint x = first.x; x < last.x; ++x)
			nearest = min(nearest, Source.Load(int3(x, y, 0)));
	}
	Destination[id.xy] = nearest;
}
