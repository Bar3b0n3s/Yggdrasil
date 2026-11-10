// Lights2D: the 2D lights' accumulation (Renderer/Lights2DPass.h; M21, C21.2, R-2D-LIGHT; Docs/Decisions/0032-m21-contract.md
// decision 22). One draw per light, added (one, one) into Lights2DLight at the scene resolution; Renderer/Light2DShading.h
// states the model and computes the CPU reference of the same arithmetic. The vertices come from the vertex index (no input
// layout):
//   - a Point light draws two triangles over its square [x - Radius, x + Radius] x [y - Radius, y + Radius] in its plane
//     z, projected by the view (Light.ViewProjection) with each corner's depth held inside the clip range [0, w], so a
//     light beyond the camera's depth range still lights the view; the fragment takes the point of the light's plane its
//     pixel shows from the interpolated corners (perspective-correct for a planar square), its distance d to the light
//     and the attenuation, and, where Lights2DNormals holds a normal-mapped lit item's normal (w 1), the cosine between
//     that normal and the direction to the light raised Height above the plane;
//   - a Global light draws one triangle over the target and adds its radiance to every pixel, normal or not.

#include "Shared/Light2DConstants.h"

[[vk::push_constant]] ConstantBuffer<Light2DConstants> Light;
// The lit items' world normals (xyz) and whether each pixel's topmost lit item has a normal map (w).
Texture2D<float4> Normals : register(t0, space0);

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 World : TEXCOORD0; // Point: the point of the light's plane the vertex is
};

// A Point light's square as two triangles, in units of its Radius around its position.
static const float2 SquareCorners[6] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0) };

[shader("vertex")]
VertexOutput VSMain(uint vertexID : SV_VulkanVertexID)
{
	VertexOutput output;
	if (Light.Type == Light2DTypeGlobal)
	{
		// (-1, -1), (3, -1), (-1, 3): one triangle covering the viewport.
		const float2 position = float2(float((vertexID & 1u) << 2u) - 1.0, float((vertexID & 2u) << 1u) - 1.0);
		output.Position = float4(position, 0.0, 1.0);
		output.World = float2(0.0);
		return output;
	}
	const float2 world = Light.PositionRadius.xy + SquareCorners[vertexID % 6u] * Light.PositionRadius.w;
	float4 clip = mul(Light.ViewProjection, float4(world, Light.PositionRadius.z, 1.0));
	clip.z = max(0.0, min(clip.z, clip.w));
	output.Position = clip;
	output.World = world;
	return output;
}

[shader("fragment")]
float4 PSMain(VertexOutput input) : SV_Target0
{
	const float3 radiance = Light.RadianceFalloff.rgb;
	if (Light.Type == Light2DTypeGlobal)
		return float4(radiance, 0.0);
	const float radius = Light.PositionRadius.w;
	const float falloff = Light.RadianceFalloff.w;
	const float2 offset = Light.PositionRadius.xy - input.World;
	const float lightDistance = sqrt(dot(offset, offset));
	if (!(lightDistance < radius))
		return float4(0.0);
	float attenuation = 1.0;
	if (falloff > 0.0 && lightDistance > radius * (1.0 - falloff))
	{
		const float t = (radius - lightDistance) / (radius * falloff);
		attenuation = t * t * (3.0 - 2.0 * t);
	}
	float facing = 1.0;
	const float4 normal = Normals.Load(int3(int2(input.Position.xy), 0));
	if (normal.w > 0.5)
	{
		const float3 toLight = float3(offset, Light.Height);
		facing = max(0.0, dot(normalize(normal.xyz), toLight * rsqrt(dot(toLight, toLight))));
	}
	return float4(radiance * attenuation * facing, 0.0);
}
