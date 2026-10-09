#include "Shared/MaterialConstants.h"
#include "Shared/Private/SelectionPassConstants.h"

#if !defined(ALPHA_MASK)
	#define ALPHA_MASK 0
#endif

Texture2D<float> Source : register(t0, space0);
[[vk::push_constant]] ConstantBuffer<SelectionMaskConstants> MaskDraw;
ConstantBuffer<MaterialConstants> Material : register(b0, space1);
Texture2D<float4> BaseColorMap : register(t0, space1);
SamplerState SelectionSampler : register(s0, space1);

struct SelectionVertexInput
{
	float3 Position : POSITION;
	float2 TexCoord : TEXCOORD0;
};

struct SelectionVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

// Match the Scene vertex shader's separate, NoContraction world and view-projection products.
float4 SelectionMultiplyExact(float4x4 matrix, float4 vector)
{
	return spirv_asm {
		%product:$$float4 = OpVectorTimesMatrix $vector $matrix;
		OpDecorate %product NoContraction;
		result:$$float4 = OpCopyObject %product
	};
}

[shader("vertex")]
SelectionVertexOutput VSMain(SelectionVertexInput input)
{
	SelectionVertexOutput result;
	result.Position = SelectionMultiplyExact(MaskDraw.ViewProjection, SelectionMultiplyExact(MaskDraw.World, float4(input.Position, 1.0)));
	result.TexCoord = input.TexCoord;
	return result;
}

struct SelectionMaskOutput
{
	float Mask : SV_Target0;
	uint Coverage : SV_Coverage;
};

[shader("fragment")]
SelectionMaskOutput PSMask(SelectionVertexOutput input)
{
	SelectionMaskOutput result;
	const float depth = Source.Load(int3(int2(input.Position.xy), 0));
	result.Mask = input.Position.z >= depth ? 1.0 : 0.5;
	result.Coverage = 0xffffffff;
#if ALPHA_MASK
	const float alpha = BaseColorMap.Sample(SelectionSampler, input.TexCoord * Material.UVScale + Material.UVOffset).a * Material.BaseColor.a;
	if (alpha < Material.AlphaCutoff)
		result.Coverage = 0;
#endif
	return result;
}
