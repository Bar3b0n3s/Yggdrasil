module Gtao;

// Prepass octahedral encoding is signed [-1,1], with +Z at (0,0).
public float3 DecodeGtaoNormal(float2 encoded)
{
	float3 normal = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	const float fold = max(-normal.z, 0.0);
	normal.xy += select(normal.xy >= 0.0, float2(-fold), float2(fold));
	return normalize(normal);
}

// AO sample centres map through normalized coordinates, including odd and one-dimensional extents.
public uint2 GtaoFullPixel(uint2 aoPixel, uint2 fullSize, uint2 aoSize)
{
	return min(uint2((float2(aoPixel) + 0.5) * float2(fullSize) / float2(aoSize)), fullSize - 1);
}

public float GtaoEdgeWeight(float centerDepth, float sampleDepth, float3 centerNormal, float3 sampleNormal)
{
	if (centerDepth >= 65504.0 || sampleDepth >= 65504.0)
		return centerDepth == sampleDepth ? 1.0 : 0.0;
	const float depthWeight = saturate(1.0 - abs(centerDepth - sampleDepth) / max(0.02 * centerDepth, 0.001));
	const float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), 16.0);
	return depthWeight * normalWeight;
}

// Forward set 0 binds AO at t6 and supplies full-resolution ViewDepth and SceneNormals at integrator-selected slots.
// Call with uint2(SV_Position.xy). No sampler or new constants: dimensions are queried from the bound textures.
// A full-resolution AO load is exact. Half-resolution reconstruction uses four bilateral taps; if none represents
// this surface (a subpixel silhouette), white avoids applying an unrelated occluder's AO to it.
public float SampleGtao(Texture2D<float> occlusion, Texture2D<float> viewDepth,
	Texture2D<float2> sceneNormals, uint2 fullPixel)
{
	uint2 fullSize;
	uint2 aoSize;
	viewDepth.GetDimensions(fullSize.x, fullSize.y);
	occlusion.GetDimensions(aoSize.x, aoSize.y);
	fullPixel = min(fullPixel, fullSize - 1);
	const float depth = viewDepth.Load(int3(fullPixel, 0));
	if (depth >= 65504.0)
		return 1.0;
	if (all(aoSize == fullSize))
		return occlusion.Load(int3(fullPixel, 0));
	const float3 normal = DecodeGtaoNormal(sceneNormals.Load(int3(fullPixel, 0)));
	const float2 coordinate = (float2(fullPixel) + 0.5) * float2(aoSize) / float2(fullSize) - 0.5;
	const int2 first = int2(floor(coordinate));
	const float2 fraction = frac(coordinate);
	float weighted = 0.0;
	float weightSum = 0.0;
	for (int y = 0; y < 2; ++y)
	{
		for (int x = 0; x < 2; ++x)
		{
			const uint2 tap = uint2(clamp(first + int2(x, y), int2(0), int2(aoSize) - 1));
			const uint2 fullTap = GtaoFullPixel(tap, fullSize, aoSize);
			const float sampleDepth = viewDepth.Load(int3(fullTap, 0));
			const float3 sampleNormal = DecodeGtaoNormal(sceneNormals.Load(int3(fullTap, 0)));
			const float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y)
				* GtaoEdgeWeight(depth, sampleDepth, normal, sampleNormal);
			weighted += weight * occlusion.Load(int3(tap, 0));
			weightSum += weight;
		}
	}
	return weightSum > 1e-6 ? weighted / weightSum : 1.0;
}
