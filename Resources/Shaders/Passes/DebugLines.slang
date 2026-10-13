// DebugLines: §8.3 pass 13's debug lines (Renderer/DebugRenderer.h, Architecture §8.10). The DebugDrawList tessellated on the
// CPU into a LineList of DebugLineVertex (world position, linear straight-alpha colour), drawn 1 px wide into the final LDR
// target after tonemapping. The target holds display-encoded values (§8.9), so the vertex shader encodes the linear colour
// with the sRGB OETF and the pipelines blend with straight alpha. One program for both pipelines: depth-tested against
// SceneDepth (reverse-Z, GreaterOrEqual, no writes) or on top (no depth test).
//
// Depth-tested lines usually lie on the surfaces they outline: wireframe edges on their own triangles, the grid on a ground at
// y = 0, bounds and colliders on faces. A line fragment takes its depth from the segment at the projection of its centre,
// up to half a pixel from where the surface's depth is evaluated, so wherever the surface's depth changes across the line
// about half the fragments would fail the test (dashed lines). Polygon depth bias does not apply to lines, so the vertex
// shader moves every vertex toward the camera by DebugLineDepthOffsetPixels pixel footprints of its distance: a coplanar line
// then stays in front of its surface wherever the surface makes more than about 3.6 degrees with the view ray (half a pixel
// of a depth slope of up to 16 footprints per pixel), and in front of every float rounding of the CPU-built vertices. The on-top
// pipeline tests no depth, so the offset changes nothing there.

#include "Shared/ViewConstants.h"

import Common.Color;

// The offset toward the camera, in pixel footprints at the vertex's distance (see the file comment).
static const float DebugLineDepthOffsetPixels = 8.0;

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
	float4 clip = mul(View.ViewProjection, float4(input.Position, 1.0));
	if (View.ProjectionKind == ProjectionKindOrthographic)
	{
		// d = (Far + zView) / (Far - Near) is linear in view depth, and a pixel footprint is 2 OrthoHalfExtents.y / height.
		if (View.Far > View.Near)
			clip.z += DebugLineDepthOffsetPixels * 2.0 * View.OrthoHalfExtents.y * View.InverseViewportSize.y / (View.Far - View.Near) * clip.w;
	}
	else
	{
		// d = Near / viewDepth: moving toward the camera by the fraction f of its distance divides the depth by 1 - f, with
		// f = DebugLineDepthOffsetPixels pixel angles of 2 tan(fov / 2) / height each.
		const float fraction = DebugLineDepthOffsetPixels * 2.0 * View.TanHalfFovY * View.InverseViewportSize.y;
		clip.z /= max(1.0 - fraction, 0.5);
	}
	output.Position = clip;
	output.Color = float4(LinearToSrgb(input.Color.rgb), saturate(input.Color.a));
	return output;
}

[shader("fragment")]
float4 PSMain(VertexOutput input) : SV_Target0
{
	return input.Color;
}
