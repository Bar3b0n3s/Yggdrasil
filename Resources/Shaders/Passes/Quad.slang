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
//
// M21's lit entries (C21.2; Docs/Decisions/0032-m21-contract.md decision 22), with their own vertex entry VSLit, which also
// passes the quad's batch-space position on:
//   - PSHdrLit: PSHdr's product with its RGB multiplied by the light accumulation (QuadLitTexture: Lights2DLight, the
//     framebuffer's size) loaded at the fragment's pixel, its alpha unchanged (the HdrLit pipeline);
//   - PSNormals: the world normal of the quad at the fragment (the Normals pipeline, into Lights2DNormals): where the texel's
//     alpha times the vertex colour's alpha is above 0, its normal map's texel (QuadLitTexture, decoded as 2 rgb - 1; for a
//     two-channel map, Quad.NormalMapTwoChannel 1, z reconstructed from x and y as Scene.slang reconstructs it, since a BC5
//     texture and the decode fallback's copy of one hold no z: M23's milestone review) turned by the quad's tangent frame,
//     normalized, with 1 in w; without a normal map (Quad.HasNormalMap 0) the flat normal (0, 0, 1) with 0 in w; elsewhere
//     nothing (a zero SV_Coverage, as Scene.slang drops its Mask fragments: a discard needs a device feature the device does
//     not enable). The tangent frame comes from the screen-space derivatives of the batch-space position and the texture
//     coordinates, taken before any branch: +X along the increasing u, +Y against the increasing v (texture space runs
//     down), +Z the unit normal of the quad's plane facing the viewer, so a quad mirrored by its flips or a negative scale
//     mirrors its normals' X or Y and keeps them facing the lights above the plane.
//
// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 5): PSHdr and PSHdrLit, whose batch space is world space (the Sprites stage's
// sprites and tilemaps), meet the view's fog (Fog, b1; Common/Fog.slang) at the fragment's batch-space position when the
// fog is enabled and Quad.FogMode asks (QuadFogModeBlend): the colour blends toward the fog's, alpha unchanged. Both take
// VSLit's outputs, which carry that position (the Hdr pipeline draws with VSLit, whose clip position is VSMain's).

#include "Shared/FogConstants.h"
#include "Shared/QuadVertex.h"

import Common.Color;

[[vk::push_constant]] ConstantBuffer<QuadConstants> Quad;
Texture2D<float4> QuadTexture : register(t0, space0);
SamplerState QuadSampler : register(s0, space0);
// The lit entries' second texture: PSHdrLit's light accumulation, PSNormals' normal map (a white texel without one).
Texture2D<float4> QuadLitTexture : register(t1, space0);
// M32: the view's fog (b1: the push constants hold slot 0).
ConstantBuffer<FogConstants> Fog : register(b1, space0);

#include "Common/Fog.slang"

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

struct LitVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
	float3 BatchPosition : TEXCOORD1; // the corner in the batch's space (world space for sprites and tiles)
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

[shader("vertex")]
LitVertexOutput VSLit(VertexInput input)
{
	LitVertexOutput output;
	output.Position = mul(Quad.Transform, float4(input.Position, 1.0));
	output.TexCoord = input.TexCoord;
	output.Color = input.Color;
	output.BatchPosition = input.Position;
	return output;
}

// The sampled texel times the vertex colour: linear RGB and straight alpha.
float4 ShadeQuad(float2 texCoord, float4 color)
{
	return QuadTexture.SampleLevel(QuadSampler, texCoord, 0.0) * color;
}

// `shaded` at the world point `worldPosition` behind the view's fog, per Quad.FogMode.
float4 ApplyQuadFog(float4 shaded, float3 worldPosition)
{
	if (Fog.Enabled == 0 || Quad.FogMode == QuadFogModeNone)
		return shaded;
	const float4 fog = EvaluateFog(worldPosition);
	return float4(ApplyFog(shaded.rgb, fog), shaded.a);
}

[shader("fragment")]
float4 PSHdr(LitVertexOutput input) : SV_Target0
{
	return ApplyQuadFog(ShadeQuad(input.TexCoord, input.Color), input.BatchPosition);
}

[shader("fragment")]
float4 PSLdr(VertexOutput input) : SV_Target0
{
	const float4 shaded = ShadeQuad(input.TexCoord, input.Color);
	return float4(LinearToSrgb(shaded.rgb), saturate(shaded.a));
}

[shader("fragment")]
float4 PSHdrLit(LitVertexOutput input) : SV_Target0
{
	const float4 shaded = ShadeQuad(input.TexCoord, input.Color);
	const float3 light = QuadLitTexture.Load(int3(int2(input.Position.xy), 0)).rgb;
	return ApplyQuadFog(float4(shaded.rgb * light, shaded.a), input.BatchPosition);
}

struct NormalsOutput
{
	float4 Normal : SV_Target0;
	uint Coverage : SV_Coverage;
};

// Normalizes `vector`, or returns `fallback` for a zero or non-finite one.
float3 NormalizeOr(float3 vector, float3 fallback)
{
	const float lengthSquared = dot(vector, vector);
	return lengthSquared > 1e-30 && !isinf(lengthSquared) ? vector * rsqrt(lengthSquared) : fallback;
}

[shader("fragment")]
NormalsOutput PSNormals(LitVertexOutput input)
{
	// The tangent frame from the derivatives, before any branch: P(u, v) is affine over the quad, so
	// ddx(P) = Pu ddx(u) + Pv ddx(v) and ddy(P) = Pu ddy(u) + Pv ddy(v), solved for Pu and Pv.
	const float3 positionX = ddx(input.BatchPosition);
	const float3 positionY = ddy(input.BatchPosition);
	const float2 texCoordX = ddx(input.TexCoord);
	const float2 texCoordY = ddy(input.TexCoord);
	const float determinant = texCoordX.x * texCoordY.y - texCoordY.x * texCoordX.y;
	const float3 alongU = positionX * texCoordY.y - positionY * texCoordX.y;
	const float3 alongV = positionY * texCoordX.x - positionX * texCoordY.x;
	const float inverse = determinant != 0.0 ? 1.0 / determinant : 0.0;
	// cross(ddy, ddx) faces the viewer: screen x runs right and y down.
	const float3 tangentZ = NormalizeOr(cross(positionY, positionX), float3(0.0, 0.0, 1.0));
	const float3 tangentX = NormalizeOr(alongU * inverse, float3(1.0, 0.0, 0.0));
	const float3 tangentY = NormalizeOr(-alongV * inverse, float3(0.0, 1.0, 0.0));

	const float alpha = QuadTexture.SampleLevel(QuadSampler, input.TexCoord, 0.0).a * input.Color.a;
	NormalsOutput output;
	output.Coverage = alpha > 0.0 ? 0xFFFFFFFFu : 0u;
	if (Quad.HasNormalMap == 0u)
	{
		output.Normal = float4(0.0, 0.0, 1.0, 0.0);
		return output;
	}
	float3 encoded = QuadLitTexture.SampleLevel(QuadSampler, input.TexCoord, 0.0).rgb * 2.0 - 1.0;
	// A two-channel normal map (BC5, or the decode fallback's RGBA8 copy of one) stores x and y only.
	if (Quad.NormalMapTwoChannel != 0u)
		encoded.z = sqrt(saturate(1.0 - dot(encoded.xy, encoded.xy)));
	const float3 normal = tangentX * encoded.x + tangentY * encoded.y + tangentZ * encoded.z;
	output.Normal = float4(NormalizeOr(normal, tangentZ), 1.0);
	return output;
}
