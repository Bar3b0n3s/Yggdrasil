// Quad: the QuadBatcher's textured quads (Renderer/QuadBatcher.h; Docs/Decisions/0029-m18-contract.md decision 10), the 2D
// primitive of sprites, tilemaps, UI images and particles. Each vertex (Shared/QuadVertex.h) is transformed from the batch's
// space to clip space by the push constants' Transform; the fragment is the sampled texel times the vertex colour, both
// linear (colour textures are sampled through their sRGB views), with straight alpha. Two fragment entries, one per
// pipeline:
//   - PSHdr writes the product as linear radiance into SceneColor (the Sprites stage);
//   - PSLdr encodes it with the sRGB OETF (Common/Color.slang), since its LDR target holds display-encoded values (§8.9),
//     as Text encodes its colour (the SceneOverlay stage's UI).
// Textures are sampled at their first mip level ("without mips"), so a point-sampled quad whose screen size is an integer
// multiple of its texel rectangle reproduces every source texel.

#include "Shared/QuadVertex.h"

import Common.Color;

[[vk::push_constant]] ConstantBuffer<QuadConstants> Quad;
Texture2D<float4> QuadTexture : register(t0, space0);
SamplerState QuadSampler : register(s0, space0);

// QuadVertex member by member (locations 0, 1, 2).
struct VertexInput
{
	float3 Position : POSITION;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
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
	output.Position = mul(Quad.Transform, float4(input.Position, 1.0));
	output.TexCoord = input.TexCoord;
	output.Color = input.Color;
	return output;
}

// The sampled texel times the vertex colour: linear RGB and straight alpha.
float4 ShadeQuad(VertexOutput input)
{
	return QuadTexture.SampleLevel(QuadSampler, input.TexCoord, 0.0) * input.Color;
}

[shader("fragment")]
float4 PSHdr(VertexOutput input) : SV_Target0
{
	return ShadeQuad(input);
}

[shader("fragment")]
float4 PSLdr(VertexOutput input) : SV_Target0
{
	const float4 shaded = ShadeQuad(input);
	return float4(LinearToSrgb(shaded.rgb), saturate(shaded.a));
}
