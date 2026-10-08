// Scene: the mesh passes of the walking skeleton's scene renderer (Roadmap M7; Renderer/SceneRenderer, §8.3 passes 4 and
// 7; Docs/Decisions/0012-m7-decisions.md decision 7). One vertex shader serves both passes, so the prepass and the forward
// pass compute bit-identical positions and the forward pass's GreaterOrEqual test against the prepass depth (reverse-Z,
// writes off) keeps exactly the visible surfaces:
//   - PSPrepass writes the octahedral view-space normal of the interpolated vertex normal into SceneNormals (RG16_FLOAT;
//     normal maps arrive with M8);
//   - PSForward writes linear HDR colour into SceneColor (RGBA16_FLOAT): Lambert diffuse with the one directional light
//     plus the constant ambient term, times the material's base colour factor.
// Vertices are MeshVertex (§6.8): only the position (location 0) and the normal (location 1) are read. The normal matrix
// is the cofactor of the world matrix's upper 3x3 (§8.4), signed by its determinant so a mirrored world keeps outward
// normals (the pipelines of mirrored draws flip the winding instead).

#include "Shared/DrawConstants.h"
#include "Shared/LightingConstants.h"
#include "Shared/MaterialConstants.h"
#include "Shared/ViewConstants.h"

ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<LightingConstants> Lighting : register(b2, space0);
ConstantBuffer<MaterialConstants> Material : register(b0, space1);
[[vk::push_constant]] ConstantBuffer<DrawConstants> Draw;

struct VertexInput
{
	float3 Position : POSITION;
	float3 Normal : NORMAL;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float3 WorldNormal : NORMAL;
};

// The normal matrix of `world`'s upper 3x3: its cofactor matrix (rows r1 x r2, r2 x r0, r0 x r1 of the rows r0..r2),
// which equals det * inverse-transpose, multiplied by the determinant's sign.
float3x3 MakeNormalMatrix(float4x4 world)
{
	const float3x3 linear = (float3x3)world;
	const float3x3 cofactor = float3x3(cross(linear[1], linear[2]), cross(linear[2], linear[0]), cross(linear[0], linear[1]));
	const float determinant = dot(linear[0], cofactor[0]);
	return determinant < 0.0 ? -cofactor : cofactor;
}

// The octahedral encoding of a unit vector into [-1, 1]^2 (Cigolle et al. 2014).
float2 EncodeOctahedral(float3 normal)
{
	const float3 projected = normal / (abs(normal.x) + abs(normal.y) + abs(normal.z));
	if (projected.z >= 0.0)
		return projected.xy;
	const float2 signs = select(projected.xy >= 0.0, float2(1.0, 1.0), float2(-1.0, -1.0));
	return (1.0 - abs(projected.yx)) * signs;
}

[shader("vertex")]
VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	const float4 world = mul(Draw.World, float4(input.Position, 1.0));
	output.Position = mul(View.ViewProjection, world);
	output.WorldNormal = mul(MakeNormalMatrix(Draw.World), input.Normal);
	return output;
}

[shader("fragment")]
float2 PSPrepass(VertexOutput input) : SV_Target0
{
	const float3 viewNormal = normalize(mul((float3x3)View.View, normalize(input.WorldNormal)));
	return EncodeOctahedral(viewNormal);
}

[shader("fragment")]
float4 PSForward(VertexOutput input) : SV_Target0
{
	const float3 normal = normalize(input.WorldNormal);
	const float diffuse = max(dot(normal, -Lighting.LightDirection), 0.0);
	const float3 radiance = Lighting.AmbientRadiance + Lighting.LightRadiance * diffuse;
	return float4(Material.BaseColor.rgb * radiance, 1.0);
}
