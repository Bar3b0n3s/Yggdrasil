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
};

[shader("vertex")]
VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.Position = mul(Caster.ViewProjection, mul(Draw.World, float4(input.Position, 1.0)));
	// NVRHI's Vulkan backend does not expose depth clamp. Pancake directional casters before clipping, while
	// the extended-near fallback leaves their homogeneous position unchanged. Spot maps always use clipping.
	if (Caster.Bias.z != 0.0)
		output.Position.z = min(output.Position.z, output.Position.w);
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
	float slope = max(abs(ddx(input.Position.z)), abs(ddy(input.Position.z)));
	output.Depth = saturate(input.Position.z - Caster.Bias.x * (slope + Caster.Bias.y));
	return output;
}
