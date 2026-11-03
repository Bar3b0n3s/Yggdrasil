// SelectionMask: the selected meshes' coverage for the selection outline (Renderer/SelectionPass.h). Every draw is instanced
// (M23, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): instance i of a draw is the view's instance record
// Instances[MaskDraw.FirstInstance + i] (t10, Shared/InstanceRecord.h), whose world matrix places it. Vertices are
// MeshVertex's position and texture coordinates (binding 0, locations 0 and 1) and the attribute stream (M23, C-MESH2:
// binding 1, locations 2 and 3), which a Mask material's alpha test reads: the base colour map's UV set and the vertex
// colour's alpha. The vertex colour reaches the fragment shader whole: as a scalar alpha, the Opaque variants' fragment
// shader, which does not read it, leaves it out of its interface, which the validation layer reports
// (WARNING-Shader-OutputNotConsumed).

#include "Shared/InstanceRecord.h"
#include "Shared/MaterialConstants.h"
#include "Shared/Private/SelectionPassConstants.h"

#if !defined(ALPHA_MASK)
	#define ALPHA_MASK 0
#endif

Texture2D<float> Source : register(t0, space0);
StructuredBuffer<InstanceRecord> Instances : register(t10, space0);
[[vk::push_constant]] ConstantBuffer<SelectionMaskConstants> MaskDraw;
ConstantBuffer<MaterialConstants> Material : register(b0, space1);
Texture2D<float4> BaseColorMap : register(t0, space1);
SamplerState SelectionSampler : register(s0, space1);

struct SelectionVertexInput
{
	float3 Position : POSITION;
	float2 TexCoord : TEXCOORD0;
	float2 TexCoord1 : TEXCOORD1;
	float4 Color : COLOR;
};

struct SelectionVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float2 TexCoord1 : TEXCOORD1;
	float4 Color : COLOR; // the vertex colour, whose alpha a Mask material's test reads
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

// SV_VulkanInstanceID is gl_InstanceIndex, counted from 0 within the draw (Scene.slang's VSMain).
[shader("vertex")]
SelectionVertexOutput VSMain(SelectionVertexInput input, uint instance : SV_VulkanInstanceID)
{
	SelectionVertexOutput result;
	const float4x4 world = Instances[MaskDraw.FirstInstance + instance].World;
	result.Position = SelectionMultiplyExact(MaskDraw.ViewProjection, SelectionMultiplyExact(world, float4(input.Position, 1.0)));
	result.TexCoord = input.TexCoord;
	result.TexCoord1 = input.TexCoord1;
	result.Color = input.Color;
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
	const float2 uv = ((Material.Flags & MaterialFlagBaseColorUV1) != 0 ? input.TexCoord1 : input.TexCoord) * Material.UVScale + Material.UVOffset;
	const float alpha = BaseColorMap.Sample(SelectionSampler, uv).a * Material.BaseColor.a * input.Color.a;
	if (alpha < Material.AlphaCutoff)
		result.Coverage = 0;
#endif
	return result;
}
