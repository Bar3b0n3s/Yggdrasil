// Blit: §8.3 pass 15's blit (Renderer/BlitPass; Docs/Decisions/0012-m7-decisions.md decision 7). One fullscreen triangle
// samples the source through LinearClamp and writes its values unconverted: the source (LdrColor) and the target (the
// swapchain's BGRA8_UNORM or an RGBA8_UNORM target) both hold display-encoded values (§8.9). The vertices come from the
// vertex index (no input layout). Clip-space +Y is up (NVRHI's viewport performs the Vulkan Y flip, §8.3), and texture
// coordinates start at the top-left, so v = 0.5 - 0.5 * y.

Texture2D<float4> Source : register(t0, space0);
SamplerState LinearClamp : register(s0, space0);

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

[shader("vertex")]
VertexOutput VSMain(uint vertexID : SV_VulkanVertexID)
{
	// (-1, -1), (3, -1), (-1, 3): one triangle covering the viewport.
	const float2 position = float2(float((vertexID & 1u) << 2u) - 1.0, float((vertexID & 2u) << 1u) - 1.0);
	VertexOutput output;
	output.Position = float4(position, 0.0, 1.0);
	output.TexCoord = float2(0.5 + 0.5 * position.x, 0.5 - 0.5 * position.y);
	return output;
}

[shader("fragment")]
float4 PSMain(VertexOutput input) : SV_Target0
{
	return Source.SampleLevel(LinearClamp, input.TexCoord, 0.0);
}
