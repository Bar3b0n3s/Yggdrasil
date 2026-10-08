// Text: §8.3 pass 14 (Renderer/TextRenderer.h, Architecture §8.10): signed-distance-field glyph quads from a font's R8 atlas
// (FontData: 128 is the outline, larger values inside), drawn into the final LDR target. The vertex shader places each vertex
// by its mode (Shared/TextVertex.h: screen, world, billboard or debug label); the fragment shader turns the distance into
// coverage with a smoothstep over its screen-space derivative (fwidth), so edges stay one to two pixels soft at any size.
// The target holds display-encoded values (§8.9): the linear colour is encoded with the sRGB OETF and blended with straight
// alpha. One program for both pipelines: depth-tested against SceneDepth (world texts, tested labels) or on top.

#include "Shared/TextVertex.h"
#include "Shared/ViewConstants.h"

import Common.Color;

ConstantBuffer<ViewConstants> View : register(b0, space0);
Texture2D<float> Atlas : register(t0, space0);
SamplerState AtlasSampler : register(s0, space0);

// The atlas value of an outline (FontImporter: stb_truetype's on-edge value 128 of 255).
static const float SdfEdgeValue = 128.0 / 255.0;
// The smallest smoothstep half-width, so a flat field (zero derivative) still gives a defined coverage.
static const float MinEdgeWidth = 1.0e-4;

struct VertexInput
{
	float3 Position : POSITION;
	float2 Corner : CORNER;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
	uint Mode : MODE;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
};

[shader("vertex")]
VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.TexCoord = input.TexCoord;
	output.Color = float4(LinearToSrgb(input.Color.rgb), saturate(input.Color.a));
	if (input.Mode == TextVertexScreen)
	{
		output.Position = float4(input.Position.xy, 0.0, 1.0);
		return output;
	}

	// World, billboard and label vertices all go through the same view and projection products, so a billboard that
	// happens to face the camera rasterizes exactly like the same text laid out in world space.
	float4 viewPosition = mul(View.View, float4(input.Position, 1.0));
	if (input.Mode == TextVertexBillboard)
		viewPosition.xy += input.Corner;
	if (input.Mode == TextVertexLabel && viewPosition.z >= 0.0)
	{
		// On or behind the camera plane: every vertex of the label lands on the same point outside the clip volume.
		output.Position = float4(2.0, 2.0, 0.0, 1.0);
		return output;
	}
	float4 clip = mul(View.Projection, viewPosition);
	if (input.Mode == TextVertexLabel)
		clip.xy += input.Corner * clip.w;
	output.Position = clip;
	return output;
}

[shader("fragment")]
float4 PSMain(VertexOutput input) : SV_Target0
{
	const float field = Atlas.Sample(AtlasSampler, input.TexCoord);
	const float width = max(fwidth(field), MinEdgeWidth);
	const float coverage = smoothstep(SdfEdgeValue - width, SdfEdgeValue + width, field);
	return float4(input.Color.rgb, input.Color.a * coverage);
}
