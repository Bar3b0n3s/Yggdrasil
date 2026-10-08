// DebugLines: §8.3 pass 13's debug lines (Renderer/DebugRenderer.h, Architecture §8.10). The DebugDrawList tessellated on the
// CPU into a LineList of DebugLineVertex (world position, linear straight-alpha colour), drawn 1 px wide into the final LDR
// target after tonemapping. The target holds display-encoded values (§8.9), so the vertex shader encodes the linear colour
// with the sRGB OETF and the pipelines blend with straight alpha. One program for both pipelines: depth-tested against
// SceneDepth (reverse-Z, GreaterOrEqual, no writes) or on top (no depth test).

#include "Shared/ViewConstants.h"

import Common.Color;

ConstantBuffer<ViewConstants> View : register(b0, space0);

struct VertexInput
{
	float3 Position : POSITION;
	float4 Color : COLOR0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float4 Color : COLOR0;
};

[shader("vertex")]
VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.Position = mul(View.ViewProjection, float4(input.Position, 1.0));
	output.Color = float4(LinearToSrgb(input.Color.rgb), saturate(input.Color.a));
	return output;
}

[shader("fragment")]
float4 PSMain(VertexOutput input) : SV_Target0
{
	return input.Color;
}
