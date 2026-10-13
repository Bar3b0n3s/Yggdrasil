#include "Shared/DrawConstants.h"
#include "Shared/MaterialConstants.h"
#include "Shared/ShadowCasterConstants.h"

#ifndef ALPHA_MASK
#define ALPHA_MASK 0
#endif

ConstantBuffer<ShadowCasterConstants> Caster : register(b0, space0);
[[vk::push_constant]] ConstantBuffer<DrawConstants> Draw;
#if ALPHA_MASK
SamplerState AnisoWrap : register(s2, space0);
ConstantBuffer<MaterialConstants> Material : register(b0, space1);
Texture2D<float4> BaseColorMap : register(t0, space1);
#endif

struct VertexInput
{
	float3 Position : POSITION;
	float2 TexCoord : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	// The caster's light-space depth, interpolated linearly in screen space exactly as the rasterizer interpolates
	// SV_Position.z. Read only when Caster.Bias.z pancakes (orthographic directional maps, w = 1).
	noperspective float Depth : TEXCOORD1;
};

[shader("vertex")]
VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	float4 clip = mul(Caster.ViewProjection, mul(Draw.World, float4(input.Position, 1.0)));
	output.Position = clip;
	output.Depth = clip.z / clip.w;
	// Pancaking is per fragment, as hardware depth clamp does it (NVRHI's Vulkan backend does not expose depth clamp): the
	// rasterizer sees a constant mid-range depth, so no caster triangle is clipped at the near or far plane, and the
	// fragment shader clamps the exactly interpolated depth. Clamping clip z per vertex would tilt every triangle that
	// crosses the near plane and move its interior away from the light. The extended-near fallback and the perspective
	// spot maps keep their homogeneous position and its clipping.
	if (Caster.Bias.z != 0.0)
		output.Position.z = 0.5 * clip.w;
	output.TexCoord = input.TexCoord;
	return output;
}

struct ShadowPixel
{
	uint Coverage : SV_Coverage;
	float Depth : SV_Depth;
};

[shader("fragment")]
ShadowPixel PSMain(VertexOutput input)
{
	ShadowPixel output;
	output.Coverage = 1;
#if ALPHA_MASK
	float2 uv = input.TexCoord * Material.UVScale + Material.UVOffset;
	float alpha = BaseColorMap.Sample(AnisoWrap, uv).a * Material.BaseColor.a;
	// Match Scene.slang's coverage mask without requiring shader-demote support.
	output.Coverage = alpha < Material.AlphaCutoff ? 0 : 1;
#endif
	float depth = Caster.Bias.z != 0.0 ? input.Depth : input.Position.z;
	float slope = max(abs(ddx(depth)), abs(ddy(depth)));
	// saturate is the pancake: a caster nearer the light than the map's near plane is stored at depth 1.
	output.Depth = saturate(depth - Caster.Bias.x * (slope + Caster.Bias.y));
	return output;
}
