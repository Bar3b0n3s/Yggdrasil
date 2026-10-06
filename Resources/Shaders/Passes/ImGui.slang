// ImGui: Dear ImGui's draw lists through NVRHI (Architecture §8.11; ImGui/ImGuiRenderer). Vertices are ImDrawVert
// (position, UV, RGBA8 colour); the projection of ImGui's display rectangle is in push constants. ImGui works in gamma
// space and the target is UNORM holding display-encoded values (§8.9), so colours pass through unconverted and blending
// is straight alpha.

#include "Shared/ImGuiConstants.h"

[[vk::push_constant]] ConstantBuffer<ImGuiConstants> Projection;

Texture2D Texture : register(t0, space0);
SamplerState Sampler : register(s0, space0);

struct VertexInput
{
	float2 Position : POSITION;
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
	output.Position = float4(input.Position * Projection.Scale + Projection.Translate, 0.0, 1.0);
	output.TexCoord = input.TexCoord;
	output.Color = input.Color;
	return output;
}

[shader("fragment")]
float4 PSMain(VertexOutput input) : SV_Target0
{
	return input.Color * Texture.Sample(Sampler, input.TexCoord);
}
