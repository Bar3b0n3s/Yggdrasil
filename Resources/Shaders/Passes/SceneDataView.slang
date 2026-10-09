#include "Shared/ViewConstants.h"
#include "Shared/Private/SceneDataViewConstants.h"
#include "Common/Shadows.slang"

import Common.Gtao;

ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<ShadowConstants> Shadows : register(b1, space0);
Texture2D<float> SceneDepth : register(t0, space0);
Texture2D<float> Occlusion : register(t1, space0);
Texture2D<float> ViewDepth : register(t2, space0);
Texture2D<float2> SceneNormals : register(t3, space0);
Texture2D<float4> Coverage : register(t4, space0);
[[vk::push_constant]] ConstantBuffer<SceneDataViewConstants> DataView;
[vk::image_format("rgba8")]
RWTexture2D<float4> Output : register(u0, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
	uint width, height;
	Output.GetDimensions(width, height);
	if (thread.x >= width || thread.y >= height)
		return;
	const uint2 pixel = thread.xy;
	const float depth = SceneDepth.Load(int3(pixel, 0));
	float3 color = 0.0;
	if (DataView.Options.x == 6)
		color = float3(depth == 0.0 ? 1.0 : SampleGtao(Occlusion, ViewDepth, SceneNormals, pixel));
	else if (DataView.Options.x == 7 && depth > 0.0)
	{
		const float distance = View.ProjectionKind == ProjectionKindOrthographic ? View.Far - depth * (View.Far - View.Near) : View.Near / depth;
		color = ShadowCascadeColor(Shadows, distance);
	}
	else if (DataView.Options.x == 8)
	{
		const float count = Coverage.Load(int3(pixel, 0)).r;
		if (count >= 5.0) color = float3(1, 0, 0);
		else if (count >= 4.0) color = float3(1, 1, 0);
		else if (count >= 3.0) color = float3(0, 1, 0);
		else if (count >= 2.0) color = float3(0, 1, 1);
		else if (count >= 1.0) color = float3(0, 0, 1);
	}
	Output[pixel] = float4(color, 1.0);
}
