// Upscale: the output stage's integer upscale (Renderer/UpscalePass.h; Docs/Decisions/0029-m18-contract.md decision 6). One
// triangle covers the viewport, which is the scaled scene rectangle (the scissor clips it to the view); each fragment reads
// the scene texel its view pixel shows with texelFetch at floor((pixel - offset) / scale), so every scene pixel becomes
// exactly a Scale x Scale block. The vertices come from the vertex index (no input layout). The letterbox repaint
// (UpscalePass::RecordLetterbox) draws PSColor from a one-texel source over the whole view, offset 0 and the view's larger
// side as the scale (texel 0 for every pixel), with each letterbox bar as the scissor.
//   - PSColor copies the scene image's values unconverted: the scene's LDR target and OutputColor both hold display-encoded
//     values (§8.9);
//   - PSDepth writes the scene depth (reverse-Z) into OutputDepth (SV_Depth), for the view-resolution editor overlays.

#include "Shared/Private/UpscaleConstants.h"

[[vk::push_constant]] ConstantBuffer<UpscaleConstants> Upscale;
Texture2D<float4> Source : register(t0, space0);

struct VertexOutput
{
	float4 Position : SV_Position;
};

[shader("vertex")]
VertexOutput VSMain(uint vertexID : SV_VulkanVertexID)
{
	// (-1, -1), (3, -1), (-1, 3): one triangle covering the viewport.
	const float2 position = float2(float((vertexID & 1u) << 2u) - 1.0, float((vertexID & 2u) << 1u) - 1.0);
	VertexOutput output;
	output.Position = float4(position, 0.0, 1.0);
	return output;
}

// The scene texel the fragment's view pixel shows (integer arithmetic; the viewport keeps the pixel inside the scaled
// scene rectangle, or the letterbox repaint's offset is 0, so the difference is never negative).
int3 GetSceneTexel(float4 position)
{
	const int2 pixel = int2(position.xy);
	const int2 offset = asint(Upscale.Mapping.xy);
	const int scale = int(Upscale.Mapping.z);
	return int3((pixel - offset) / scale, 0);
}

[shader("fragment")]
float4 PSColor(VertexOutput input) : SV_Target0
{
	return Source.Load(GetSceneTexel(input.Position));
}

[shader("fragment")]
float PSDepth(VertexOutput input) : SV_Depth
{
	return Source.Load(GetSceneTexel(input.Position)).r;
}
