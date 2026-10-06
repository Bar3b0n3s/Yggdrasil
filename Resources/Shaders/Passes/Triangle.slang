// Triangle: the clear-and-triangle view of the Graphics foundation (Roadmap M5; Renderer/TrianglePass). One triangle,
// counter-clockwise in a +Y-up view, transformed by ViewConstants (b0, space 0, §8.4). Drawn with back-face culling and
// frontCounterClockwise = true, it stays visible only if the projection's Y flip and the pipelines' winding convention
// agree (§8.3; "Rasterizer: CCW triangle survives back-face culling"). The vertices come from SV_VertexID, so the
// pipeline has no input layout.

#include "Shared/ViewConstants.h"

import Common.Color;

ConstantBuffer<ViewConstants> View : register(b0, space0);

struct VertexOutput
{
	float4 Position : SV_Position;
	float3 Color : COLOR0;
};

// World-space positions, counter-clockwise seen from +Z (the camera looks down -Z), and linear vertex colours.
static const float3 TrianglePositions[3] = {
	float3(-0.75, -0.6, 0.0),
	float3(0.75, -0.6, 0.0),
	float3(0.0, 0.7, 0.0),
};

static const float3 TriangleColors[3] = {
	float3(0.9, 0.1, 0.1),
	float3(0.1, 0.9, 0.1),
	float3(0.1, 0.2, 0.9),
};

[shader("vertex")]
VertexOutput VSMain(uint vertexID : SV_VertexID)
{
	VertexOutput output;
	output.Position = mul(View.ViewProjection, float4(TrianglePositions[vertexID], 1.0));
	output.Color = TriangleColors[vertexID];
	return output;
}

[shader("fragment")]
float4 PSMain(VertexOutput input) : SV_Target0
{
	return float4(LinearToSrgb(input.Color), 1.0);
}
