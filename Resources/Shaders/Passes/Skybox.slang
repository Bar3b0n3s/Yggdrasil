// Skybox: §8.3 pass 8 (Renderer/SkyboxPass.h). One fullscreen triangle at depth 0, drawn with the depth test GreaterOrEqual
// against SceneDepth and writes off, so it covers exactly the pixels the scene left at the cleared (far) depth. Each pixel
// samples the skybox cube along its view ray, rotated by the environment's Rotation, at SkyboxLod, times Intensity: linear
// radiance into SceneColor (exposure and tonemapping follow).

#include "Shared/EnvironmentConstants.h"
#include "Shared/ViewConstants.h"

import Common.Environment;

ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<EnvironmentConstants> SkyEnvironment : register(b2, space0);
TextureCube<float4> EnvSkybox : register(t4, space0);
SamplerState LinearClamp : register(s0, space0);

struct SkyboxVaryings
{
	float4 Position : SV_Position;
	float2 Ndc : TEXCOORD0;
};

// The world-space point at normalized device coordinates `ndc` and reverse-Z depth `depth`.
float3 Unproject(float2 ndc, float depth)
{
	const float4 position = mul(View.InverseViewProjection, float4(ndc, depth, 1.0));
	return position.xyz / position.w;
}

// The triangle (-1, -1), (3, -1), (-1, 3) covers the viewport; depth 0 is the far plane (reverse-Z, §8.3).
[shader("vertex")]
SkyboxVaryings VSMain(uint vertexID : SV_VulkanVertexID)
{
	const float2 ndc = float2(vertexID == 1 ? 3.0 : -1.0, vertexID == 2 ? 3.0 : -1.0);
	SkyboxVaryings output;
	output.Position = float4(ndc, 0.0, 1.0);
	output.Ndc = ndc;
	return output;
}

// The view ray per §8.3: per pixel for a perspective camera (two points on the pixel's ray: the near plane at depth 1 and twice
// the near distance at depth 0.5, the far plane being at infinity), the constant view direction for an orthographic one (its
// far plane at depth 0 minus its near plane at depth 1).
[shader("fragment")]
float4 PSMain(SkyboxVaryings input) : SV_Target0
{
	float3 ray;
	if (View.ProjectionKind == ProjectionKindOrthographic)
		ray = Unproject(float2(0.0), 0.0) - Unproject(float2(0.0), 1.0);
	else
		ray = Unproject(input.Ndc, 0.5) - Unproject(input.Ndc, 1.0);
	const float3 lookup = RotateEnvironmentDirection(normalize(ray), SkyEnvironment.RotationSin, SkyEnvironment.RotationCos);
	const float3 radiance = EnvSkybox.SampleLevel(LinearClamp, lookup, SkyEnvironment.SkyboxLod).rgb * SkyEnvironment.Intensity;
	return float4(radiance, 1.0);
}
