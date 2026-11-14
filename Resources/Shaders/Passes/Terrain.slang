// Terrain: the terrain passes of the scene renderer (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 13
// and 22; Renderer/TerrainPass.h; Architecture §8.3 and §8.5). A terrain is drawn as its LOD chunks (Renderer/TerrainLod.h):
// each vertex is a heightfield sample, its chunk-local sample coordinates (vertex binding 0, RG16_UINT) plus the chunk's
// origin (the push constants, Shared/TerrainConstants.h), placed by Asset/TerrainGeometry.h's rule from the heightfield's
// R16_UNORM texture (GpuResourceCache::GetHeightfield) and the terrain's World. Every vertex entry computes its clip
// position with Passes/Scene.slang's NoContraction products, so the prepass and the forward pass compute bit-identical
// positions and the forward pass's GreaterOrEqual test against the prepass depth keeps exactly the visible surface:
//   - PSPrepass (TerrainDepthNormal) writes the prepass's targets: the octahedral view-space shading normal (SceneNormals),
//     the camera's motion since the previous frame in scene pixels (SceneVelocity, Common/Motion.slang's ComputeMotionVector
//     of the vertex's SV_Position and its world position through ViewConstants::PreviousViewProjection, both jittered alike:
//     the terrain has no previous world, so its points move only with the camera, and a still camera gives exactly zero),
//     and SSR's composite inputs (SceneRoughness: the forward pass's prefiltered-radiance multiplier and sampled roughness,
//     and the occlusion 1, SceneTargetFormats.h, packed with Common/Motion.slang's PackUnorm4x8), computed with the forward
//     pass's own expressions; PSPicking adds the terrain's pick id (EntityId) in the picking prepass's attachment order;
//   - PSForward (TerrainForward) shades like Passes/Scene.slang's ShadeForward: GGX, the DFG LUT's multi-scatter
//     compensation, geometric specular anti-aliasing, the view's lights through M23's light loop (Common/LightLoop.slang) with
//     the view's shadows when the terrain receives them, the environment's image-based lighting replaced inside their boxes
//     by the baked probes (Common/Probes.slang), and GTAO; a terrain has no emission and no occlusion map. A material debug
//     view (TerrainConstants::DebugView) outputs its quantity, linear, with alpha 1.
// The surface: up to four layers weighted by the splat map's channels (red layer 0 to alpha layer 3) over their sum, layer 0
// alone without a splat map or where the weights sum to 0; each layer's base colour (times its tint) and tangent-space normal
// map tiled every Tiling metres in local XZ, its roughness and metallic constants. The smooth geometric normal is the
// heightfield's central difference at each vertex (one-sided at the edges), so two chunks that share a vertex share its
// normal; the tangent is local +X and the bitangent cross(normal, tangent), flipped with a mirroring World (glTF's
// convention, as Scene.slang's MeshVertex tangents with w = 1). The pipelines cull nothing: a back face (the terrain seen from
// below) is shaded with its normals flipped.
// Separate vertex entries per set of outputs, as Scene.slang has, since the validation layer reports a vertex output a
// fragment shader does not read: VSPrepass (with the clip positions of the velocity) for PSPrepass and PSPicking, VSForward
// for PSForward.

#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/InstanceRecord.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/TerrainConstants.h"
#include "Shared/ViewConstants.h"
#include "Common/Shadows.slang"

import Common.Brdf;
import Common.Environment;
import Common.Lighting;
import Common.Gtao;
import Common.Motion;

// Set 0: the mesh forward passes' per-view set (Passes/Scene.slang, §8.4), which TerrainForward binds as it is; the
// prepass pipelines' own set 0 holds View, DfgLut, LinearClamp, AnisoWrap and the push constants at the same registers.
ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<ShadowConstants> Shadows : register(b1, space0);
ConstantBuffer<EnvironmentConstants> Env : register(b2, space0);
ConstantBuffer<LightClusterConstants> Clusters : register(b3, space0);
StructuredBuffer<ShaderLight> Lights : register(t0, space0);
Texture2DArray<float> ShadowMaps : register(t1, space0);
Texture2D<float> ShadowAtlas : register(t2, space0);
TextureCube<float4> EnvSpecular : register(t3, space0);
Texture2D<float2> DfgLut : register(t5, space0);
Texture2D<float> AmbientOcclusion : register(t6, space0);
Texture2D<float> LinearViewDepth : register(t8, space0);
Texture2D<float2> SceneNormals : register(t9, space0);
StructuredBuffer<InstanceRecord> Instances : register(t10, space0); // the mesh draws' records, which the terrain does not read
StructuredBuffer<uint> LightClusterMasks : register(t11, space0);
TextureCubeArray<float> PointShadowMaps : register(t12, space0);
ConstantBuffer<ProbeConstants> Probes : register(b4, space0);
StructuredBuffer<float4> IrradianceProbes : register(t13, space0);
TextureCubeArray<float4> ReflectionProbeCubes : register(t14, space0);
SamplerState LinearClamp : register(s0, space0);
SamplerComparisonState ShadowCompare : register(s1, space0);
SamplerState AnisoWrap : register(s2, space0);
[[vk::push_constant]] ConstantBuffer<TerrainDrawConstants> Chunk;

// Set 1: per terrain.
ConstantBuffer<TerrainConstants> Terrain : register(b0, space1);
Texture2D<float> Heights : register(t0, space1);
Texture2D<float4> SplatMap : register(t1, space1);
Texture2D<float4> LayerBaseColor0 : register(t2, space1);
Texture2D<float4> LayerBaseColor1 : register(t3, space1);
Texture2D<float4> LayerBaseColor2 : register(t4, space1);
Texture2D<float4> LayerBaseColor3 : register(t5, space1);
Texture2D<float4> LayerNormal0 : register(t6, space1);
Texture2D<float4> LayerNormal1 : register(t7, space1);
Texture2D<float4> LayerNormal2 : register(t8, space1);
Texture2D<float4> LayerNormal3 : register(t9, space1);

// The forward pass's light loop and the probes' indirect term over the declarations above, as Scene.slang includes them.
#include "Common/LightLoop.slang"
#include "Common/Probes.slang"

struct TerrainVertexInput
{
	uint2 Sample : SAMPLE; // the vertex's sample, relative to the chunk's origin
};

// VSForward's outputs, which PSForward reads (and the surface part of VSPrepass's).
struct TerrainVertexOutput
{
	float4 Position : SV_Position;
	float3 WorldPosition : WORLDPOSITION;
	float3 WorldNormal : NORMAL;
	float3 WorldTangent : TANGENT;
	float2 LocalXZ : TEXCOORD0; // the point's local X and Z, metres
};

// VSPrepass's outputs, which PSPrepass and PSPicking read: VSForward's and the point's unjittered clip positions in this and
// the previous frame.
struct TerrainPrepassVertexOutput
{
	TerrainVertexOutput Surface;
	float4 CurrentClip : CURRENTCLIP;
	float4 PreviousClip : PREVIOUSCLIP;
};

// What the layers say about the shaded point.
struct TerrainSurface
{
	float3 BaseColor;          // linear
	float Metallic;            // [0, 1]
	float PerceptualRoughness; // clamped to [MinPerceptualRoughness, 1]
	float3 Normal;             // the shading normal (normal-mapped), world space, unit, on the viewer's side
	float3 GeometricNormal;    // the interpolated vertex normal, world space, unit, on the same side
};

// The forward pass's specular inputs at a point, shared by PSForward and the prepass's SceneRoughness.
struct TerrainSpecular
{
	float3 View;
	float NDotV;
	float Alpha;     // after geometric specular anti-aliasing
	float Roughness; // sqrt(Alpha): the prefiltered cube's and the DFG LUT's coordinate
	float3 DiffuseColor;
	float3 F0;
	float3 SingleScatter;
	float3 EnergyCompensation;
	float3 Reflection;
};

bool IsMirrored()
{
	return (Terrain.Flags & TerrainFlagMirrored) != 0;
}

// `vector` scaled to unit length, or zero for a zero vector (never NaN).
float3 SafeNormalize(float3 vector)
{
	return vector * rsqrt(max(dot(vector, vector), 1e-20));
}

// mul(matrix, vector) with SPIR-V's NoContraction on the product (Passes/Scene.slang's MultiplyExact): every pipeline that
// places a terrain vertex computes the same IEEE operations.
float4 MultiplyExact(float4x4 matrix, float4 vector)
{
	return spirv_asm {
		%product:$$float4 = OpVectorTimesMatrix $vector $matrix;
		OpDecorate %product NoContraction;
		result:$$float4 = OpCopyObject %product
	};
}

// The normal matrix of `world`'s upper 3x3: its cofactor matrix, negated for a mirroring world (Scene.slang's).
float3x3 MakeNormalMatrix(float4x4 world, bool mirrored)
{
	const float3x3 linear = (float3x3)world;
	const float3x3 cofactor = float3x3(cross(linear[1], linear[2]), cross(linear[2], linear[0]), cross(linear[0], linear[1]));
	return mirrored ? -cofactor : cofactor;
}

// The octahedral encoding of a unit vector into [-1, 1]^2 (Scene.slang's).
float2 EncodeOctahedral(float3 normal)
{
	const float3 projected = normal / max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-20);
	if (projected.z >= 0.0)
		return projected.xy;
	const float2 signs = select(projected.xy >= 0.0, float2(1.0, 1.0), float2(-1.0, -1.0));
	return (1.0 - abs(projected.yx)) * signs;
}

// A unit vector perpendicular to the unit `normal`, for a tangent the frame leaves degenerate.
float3 MakePerpendicular(float3 normal)
{
	const float3 axis = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	return normalize(cross(axis, normal));
}

// The height in metres of sample `sample`, clamped to the heightfield.
float LoadHeight(int2 sample)
{
	const int last = int(Terrain.Resolution) - 1;
	return Heights.Load(int3(clamp(sample, int2(0, 0), int2(last, last)), 0)) * Terrain.HeightScale;
}

// The vertex of the draw's chunk: the sample's local position, the heightfield's central-difference normal there and the
// local +X tangent, placed by World.
TerrainVertexOutput TransformVertex(TerrainVertexInput input)
{
	const int2 sample = int2(Chunk.ChunkOrigin + input.Sample);
	const float3 local = float3(float(sample.x) * Terrain.CellSize.x, LoadHeight(sample), float(sample.y) * Terrain.CellSize.y);
	const int last = int(Terrain.Resolution) - 1;
	const int2 left = int2(max(sample.x - 1, 0), sample.y);
	const int2 right = int2(min(sample.x + 1, last), sample.y);
	const int2 back = int2(sample.x, max(sample.y - 1, 0));
	const int2 front = int2(sample.x, min(sample.y + 1, last));
	const float slopeX = (LoadHeight(right) - LoadHeight(left)) / (float(right.x - left.x) * Terrain.CellSize.x);
	const float slopeZ = (LoadHeight(front) - LoadHeight(back)) / (float(front.y - back.y) * Terrain.CellSize.y);
	const float3 localNormal = float3(-slopeX, 1.0, -slopeZ);

	const float4 world = MultiplyExact(Terrain.World, float4(local, 1.0));
	TerrainVertexOutput output;
	output.Position = MultiplyExact(View.ViewProjection, world);
	output.WorldPosition = world.xyz;
	output.WorldNormal = mul(MakeNormalMatrix(Terrain.World, IsMirrored()), localNormal);
	output.WorldTangent = mul((float3x3)Terrain.World, float3(1.0, 0.0, 0.0));
	output.LocalXZ = local.xz;
	return output;
}

[shader("vertex")]
TerrainVertexOutput VSForward(TerrainVertexInput input)
{
	return TransformVertex(input);
}

[shader("vertex")]
TerrainPrepassVertexOutput VSPrepass(TerrainVertexInput input)
{
	TerrainPrepassVertexOutput output;
	output.Surface = TransformVertex(input);
	// TransformVertex's world point (w is 1 for an affine World, exactly, with its NoContraction products).
	const float4 world = float4(output.Surface.WorldPosition, 1.0);
	output.CurrentClip = output.Surface.Position;
	output.PreviousClip = MultiplyExact(View.PreviousViewProjection, world);
	return output;
}

// The tangent-space normal of a layer's normal map texel (z reconstructed from x and y for a two-channel map).
float3 DecodeLayerNormal(float4 texel, uint layer)
{
	float3 normal = texel.xyz * 2.0 - 1.0;
	if (Terrain.LayerSurfaces[layer].z != 0.0)
		normal.z = sqrt(saturate(1.0 - dot(normal.xy, normal.xy)));
	return normal;
}

// The layers' weights at `localXZ`: the splat map's channels over their sum, the layers past LayerCount weighing nothing, or
// layer 0 alone without a splat map or where they sum to 0.
float4 GetLayerWeights(float2 localXZ)
{
	float4 weights = float4(1.0, 0.0, 0.0, 0.0);
	if ((Terrain.Flags & TerrainFlagSplatMap) != 0)
		weights = SplatMap.Sample(LinearClamp, localXZ * Terrain.InverseSize);
	weights *= float4(Terrain.LayerCount > 0 ? 1.0 : 0.0, Terrain.LayerCount > 1 ? 1.0 : 0.0, Terrain.LayerCount > 2 ? 1.0 : 0.0,
		Terrain.LayerCount > 3 ? 1.0 : 0.0);
	const float sum = weights.x + weights.y + weights.z + weights.w;
	return sum > 0.0 ? weights / sum : float4(1.0, 0.0, 0.0, 0.0);
}

// Blends the layers and builds the normal-mapped shading frame. Every map is sampled whatever its weight, so the pixel quad's
// derivatives stay defined.
TerrainSurface EvaluateSurface(TerrainVertexOutput input, bool isFrontFace)
{
	const float4 weights = GetLayerWeights(input.LocalXZ);
	const float4 base0 = LayerBaseColor0.Sample(AnisoWrap, input.LocalXZ * Terrain.LayerTints[0].a);
	const float4 base1 = LayerBaseColor1.Sample(AnisoWrap, input.LocalXZ * Terrain.LayerTints[1].a);
	const float4 base2 = LayerBaseColor2.Sample(AnisoWrap, input.LocalXZ * Terrain.LayerTints[2].a);
	const float4 base3 = LayerBaseColor3.Sample(AnisoWrap, input.LocalXZ * Terrain.LayerTints[3].a);
	const float3 normal0 = DecodeLayerNormal(LayerNormal0.Sample(AnisoWrap, input.LocalXZ * Terrain.LayerTints[0].a), 0);
	const float3 normal1 = DecodeLayerNormal(LayerNormal1.Sample(AnisoWrap, input.LocalXZ * Terrain.LayerTints[1].a), 1);
	const float3 normal2 = DecodeLayerNormal(LayerNormal2.Sample(AnisoWrap, input.LocalXZ * Terrain.LayerTints[2].a), 2);
	const float3 normal3 = DecodeLayerNormal(LayerNormal3.Sample(AnisoWrap, input.LocalXZ * Terrain.LayerTints[3].a), 3);

	TerrainSurface surface;
	surface.BaseColor = weights.x * base0.rgb * Terrain.LayerTints[0].rgb + weights.y * base1.rgb * Terrain.LayerTints[1].rgb
		+ weights.z * base2.rgb * Terrain.LayerTints[2].rgb + weights.w * base3.rgb * Terrain.LayerTints[3].rgb;
	const float4 roughness = float4(Terrain.LayerSurfaces[0].x, Terrain.LayerSurfaces[1].x, Terrain.LayerSurfaces[2].x, Terrain.LayerSurfaces[3].x);
	const float4 metallic = float4(Terrain.LayerSurfaces[0].y, Terrain.LayerSurfaces[1].y, Terrain.LayerSurfaces[2].y, Terrain.LayerSurfaces[3].y);
	surface.PerceptualRoughness = clamp(dot(weights, roughness), MinPerceptualRoughness, 1.0);
	surface.Metallic = saturate(dot(weights, metallic));
	const float3 tangentNormal = weights.x * normal0 + weights.y * normal1 + weights.z * normal2 + weights.w * normal3;

	// The frame: the tangent made orthogonal to the interpolated normal, the bitangent from their cross product.
	const float3 geometricNormal = SafeNormalize(input.WorldNormal);
	float3 tangent = input.WorldTangent - geometricNormal * dot(geometricNormal, input.WorldTangent);
	const float tangentLengthSquared = dot(tangent, tangent);
	tangent = tangentLengthSquared > 1e-12 ? tangent * rsqrt(tangentLengthSquared) : MakePerpendicular(geometricNormal);
	const float3 bitangent = cross(geometricNormal, tangent) * (IsMirrored() ? -1.0 : 1.0);
	const float3 normal = SafeNormalize(tangentNormal.x * tangent + tangentNormal.y * bitangent + tangentNormal.z * geometricNormal);

	// A back face (the terrain seen from below) is shaded with its normals flipped.
	const bool frontFacing = isFrontFace != IsMirrored();
	surface.Normal = frontFacing ? normal : -normal;
	surface.GeometricNormal = frontFacing ? geometricNormal : -geometricNormal;
	return surface;
}

// ShadeForward's specular inputs (Passes/Scene.slang) for `surface` at `worldPosition`.
TerrainSpecular EvaluateSpecular(TerrainSurface surface, float3 worldPosition)
{
	TerrainSpecular specular;
	// Towards the viewer: per pixel for perspective cameras, the camera's backward axis for orthographic ones (§8.3).
	specular.View = View.ProjectionKind == ProjectionKindOrthographic ? SafeNormalize(View.View[2].xyz) : SafeNormalize(View.CameraPosition - worldPosition);
	specular.NDotV = max(dot(surface.Normal, specular.View), 1e-4);
	specular.Alpha = ApplySpecularAntiAliasing(surface.GeometricNormal, surface.PerceptualRoughness * surface.PerceptualRoughness);
	specular.Roughness = sqrt(specular.Alpha);
	specular.DiffuseColor = surface.BaseColor * (1.0 - surface.Metallic);
	specular.F0 = lerp(float3(0.04), surface.BaseColor, surface.Metallic);
	const float2 dfg = DfgLut.SampleLevel(LinearClamp, float2(specular.NDotV, specular.Roughness), 0.0);
	specular.SingleScatter = lerp(dfg.xxx, dfg.yyy, specular.F0);
	specular.EnergyCompensation = float3(1.0) + specular.F0 * (1.0 / max(dfg.y, 1e-4) - 1.0);
	specular.Reflection = reflect(-specular.View, surface.Normal);
	return specular;
}

// SceneRoughness (SceneTargetFormats.h): word 0 the prefiltered radiance's multiplier before its specular occlusion (rgb) and
// the sampled roughness (a), word 1 the occlusion (1: a terrain has no occlusion map).
uint2 EncodeSpecularInputs(TerrainSurface surface, TerrainSpecular specular)
{
	const float3 multiplier = specular.SingleScatter * specular.EnergyCompensation * HorizonOcclusion(specular.Reflection, surface.GeometricNormal);
	return uint2(PackUnorm4x8(float4(multiplier, specular.Roughness)), PackUnorm4x8(float4(1.0, 0.0, 0.0, 0.0)));
}

struct TerrainPrepassOutput
{
	float2 Normal : SV_Target0;
	float2 Velocity : SV_Target1;
	uint2 Roughness : SV_Target2;
};

struct TerrainPickingOutput
{
	float2 Normal : SV_Target0;
	uint EntityId : SV_Target1;
	float2 Velocity : SV_Target2;
	uint2 Roughness : SV_Target3;
};

[shader("fragment")]
TerrainPrepassOutput PSPrepass(TerrainPrepassVertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const TerrainSurface surface = EvaluateSurface(input.Surface, isFrontFace);
	const TerrainSpecular specular = EvaluateSpecular(surface, input.Surface.WorldPosition);
	TerrainPrepassOutput output;
	output.Normal = EncodeOctahedral(SafeNormalize(mul((float3x3)View.View, surface.Normal)));
	output.Velocity = ComputeMotionVector(input.CurrentClip, input.PreviousClip, View.ViewportSize);
	output.Roughness = EncodeSpecularInputs(surface, specular);
	return output;
}

[shader("fragment")]
TerrainPickingOutput PSPicking(TerrainPrepassVertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const TerrainSurface surface = EvaluateSurface(input.Surface, isFrontFace);
	const TerrainSpecular specular = EvaluateSpecular(surface, input.Surface.WorldPosition);
	TerrainPickingOutput output;
	output.Normal = EncodeOctahedral(SafeNormalize(mul((float3x3)View.View, surface.Normal)));
	output.EntityId = Terrain.PickId;
	output.Velocity = ComputeMotionVector(input.CurrentClip, input.PreviousClip, View.ViewportSize);
	output.Roughness = EncodeSpecularInputs(surface, specular);
	return output;
}

// The quantity a material debug view outputs (RenderDebugView), linear: a terrain has no emission.
float3 GetDebugViewValue(TerrainSurface surface)
{
	switch (Terrain.DebugView)
	{
		case SceneDebugViewAlbedo:    return surface.BaseColor;
		case SceneDebugViewNormals:   return surface.Normal * 0.5 + 0.5;
		case SceneDebugViewRoughness: return float3(surface.PerceptualRoughness);
		case SceneDebugViewMetallic:  return float3(surface.Metallic);
		default:                      return float3(0.0);
	}
}

[shader("fragment")]
float4 PSForward(TerrainVertexOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
	const TerrainSurface surface = EvaluateSurface(input, isFrontFace);
	const TerrainSpecular specular = EvaluateSpecular(surface, input.WorldPosition);
	if (Terrain.DebugView != SceneDebugViewLit)
		return float4(GetDebugViewValue(surface), 1.0);

	// Receiver depth follows the triangle plane (Scene.slang's shadow normal), oriented to the geometric normal's side.
	float3 shadowNormal = cross(ddx(input.WorldPosition), ddy(input.WorldPosition));
	const float shadowNormalLength = dot(shadowNormal, shadowNormal);
	shadowNormal = shadowNormalLength > 1e-20 ? shadowNormal * rsqrt(shadowNormalLength) : surface.GeometricNormal;
	shadowNormal *= dot(shadowNormal, surface.GeometricNormal) < 0.0 ? -1.0 : 1.0;

	const float viewDepth = -mul(View.View, float4(input.WorldPosition, 1.0)).z;
	float3 radiance = ShadeDirectLights(input.WorldPosition, uint2(input.Position.xy), viewDepth, (Terrain.Flags & TerrainFlagReceiveShadows) != 0,
		shadowNormal, surface.Normal, specular.View, specular.NDotV, specular.DiffuseColor, specular.F0, specular.EnergyCompensation, specular.Alpha,
		specular.Reflection);

	// Image-based lighting (§8.6), or the constant ambient as a uniform environment, then the probes inside their boxes.
	float3 irradiance = Env.AmbientColor;
	float3 prefiltered = Env.AmbientColor;
	if (Env.HasEnvironment != 0)
	{
		irradiance = EvaluateIrradianceSH9(Env.IrradianceSH9, RotateEnvironmentDirection(surface.Normal, Env.RotationSin, Env.RotationCos));
		const float3 lookup = RotateEnvironmentDirection(specular.Reflection, Env.RotationSin, Env.RotationCos);
		prefiltered = EnvSpecular.SampleLevel(LinearClamp, lookup, specular.Roughness * Env.SpecularMaxLod).rgb * Env.Intensity;
	}
	irradiance = ShadeProbeIrradiance(input.WorldPosition, surface.Normal, irradiance);
	prefiltered = ShadeProbeSpecular(input.WorldPosition, specular.Reflection, specular.Roughness, prefiltered);
	const float3 multiScatter = specular.SingleScatter * specular.EnergyCompensation;
	const float occlusion = SampleGtao(AmbientOcclusion, LinearViewDepth, SceneNormals, uint2(input.Position.xy));
	radiance += irradiance * specular.DiffuseColor * saturate(float3(1.0) - multiScatter) * MultiBounceAmbientOcclusion(occlusion, specular.DiffuseColor);
	radiance += prefiltered * multiScatter * (SpecularOcclusion(specular.NDotV, occlusion, specular.Alpha) * HorizonOcclusion(specular.Reflection, surface.GeometricNormal));
	return float4(radiance, 1.0);
}
