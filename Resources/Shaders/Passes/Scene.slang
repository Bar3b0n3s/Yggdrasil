// Scene: the mesh passes of the scene renderer (Architecture §8.3 passes 4, 7 and 9, §8.4, §8.5; Renderer/SceneRenderer.h;
// Docs/Decisions/0013-m8-decisions.md decisions 7, 12 and 27). One vertex shader serves every pass and computes its clip
// position with NoContraction products (MultiplyExact), so the prepass and the forward passes compute bit-identical
// positions and the forward pass's GreaterOrEqual test against the prepass depth (reverse-Z, writes off) keeps exactly the
// visible surfaces:
//   - PSPrepass (pass 4) writes the octahedral view-space shading normal (normal-mapped) into SceneNormals (RG16_FLOAT);
//   - PSForward (passes 7 and 9) writes linear HDR radiance into SceneColor (RGBA16_FLOAT): the glTF 2.0 metallic-roughness
//     material (Shared/MaterialConstants.h) shaded with GGX, the height-correlated Smith visibility, Schlick's Fresnel,
//     Lambert's diffuse and the multi-scatter energy compensation of the DFG LUT (Common/Brdf.slang, Renderer/BrdfLut.h),
//     perceptual roughness clamped to at least 0.045 and widened by geometric specular anti-aliasing, every light of the
//     view's light list (Common/Lighting.slang's artist units) and image-based lighting: the environment's SH9 irradiance
//     and prefiltered specular cube (split sum), or the constant ambient AmbientColor treated as a uniform environment
//     without a map (Shared/EnvironmentConstants.h), with multi-bounce, specular and horizon occlusion from the material's
//     occlusion; plus emission. Blend materials output the base colour's alpha for the transparent pass's blending.
// Permutation ALPHA_MASK = 1 is the Mask materials' variant: both entry points drop the fragments below AlphaCutoff by
// writing a zero SV_Coverage, which keeps the sample's colour and depth unwritten exactly as a discard would. A discard
// compiles to OpDemoteToHelperInvocation for SPIR-V 1.6, which needs the shaderDemoteToHelperInvocation feature the device
// does not enable; the coverage needs no feature and leaves the pixel quad's derivatives defined. The forward
// pipelines specialize constant 0 (SceneDebugViewConstantId) with the debug view (Shared/DrawConstants.h): a view other
// than Lit outputs its quantity, linear, with alpha 1.
// Vertices are MeshVertex (§6.8): position, normal, tangent (w: the bitangent sign) and texture coordinates at locations
// 0 to 3. The normal matrix is the cofactor of the world matrix's upper 3x3 (§8.4), negated for a mirroring world
// (DrawFlagMirrored), whose draws use the CullFront pipelines; front faces are SV_IsFrontFace, inverted when mirrored, and
// double-sided materials (CullNone) flip the normals of back faces.

#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/MaterialConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ViewConstants.h"
#include "Common/Shadows.slang"

import Common.Brdf;
import Common.Environment;
import Common.Lighting;
import Common.Gtao;

#if !defined(ALPHA_MASK)
	#define ALPHA_MASK 0
#endif

// Set 0: per view (§8.4).
ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<ShadowConstants> Shadows : register(b1, space0);
ConstantBuffer<EnvironmentConstants> Env : register(b2, space0);
StructuredBuffer<ShaderLight> Lights : register(t0, space0);
Texture2DArray<float> ShadowMaps : register(t1, space0);
Texture2D<float> ShadowAtlas : register(t2, space0);
TextureCube<float4> EnvSpecular : register(t3, space0);
Texture2D<float2> DfgLut : register(t5, space0);
Texture2D<float> AmbientOcclusion : register(t6, space0);
Texture2D<float> LinearViewDepth : register(t8, space0);
Texture2D<float2> SceneNormals : register(t9, space0);
SamplerState LinearClamp : register(s0, space0);
SamplerComparisonState ShadowCompare : register(s1, space0);
SamplerState AnisoWrap : register(s2, space0);
[[vk::push_constant]] ConstantBuffer<DrawConstants> Draw;

// Set 1: per material (§8.4).
ConstantBuffer<MaterialConstants> Material : register(b0, space1);
Texture2D<float4> BaseColorMap : register(t0, space1);
Texture2D<float4> MetallicRoughnessMap : register(t1, space1);
Texture2D<float4> NormalMap : register(t2, space1);
Texture2D<float4> OcclusionMap : register(t3, space1);
Texture2D<float4> EmissiveMap : register(t4, space1);

// The debug view (SceneDebugViewConstantId = 0; SceneDebugViewLit unless the pipeline is specialized).
[vk::constant_id(0)]
const uint DebugView = 0;

struct VertexInput
{
	float3 Position : POSITION;
	float3 Normal : NORMAL;
	float4 Tangent : TANGENT;
	float2 TexCoord : TEXCOORD0;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float3 WorldPosition : WORLDPOSITION;
	float3 WorldNormal : NORMAL;
	float4 WorldTangent : TANGENT;
	float2 TexCoord : TEXCOORD0;
};

// What the material says about the shaded point.
struct Surface
{
	float4 BaseColor;          // linear, factor times map
	float Metallic;            // factor times the map's B
	float PerceptualRoughness; // factor times the map's G, clamped to [MinPerceptualRoughness, 1]
	float Occlusion;           // 1 + OcclusionStrength * (map R - 1)
	float3 Emissive;           // linear radiance
	float3 Normal;             // the shading normal (normal-mapped), world space, unit, on the viewer's side of the face
	float3 GeometricNormal;    // the interpolated vertex normal, world space, unit, on the same side
};

bool IsMirrored()
{
	return (Draw.Flags & DrawFlagMirrored) != 0;
}

// `vector` scaled to unit length, or zero for a zero vector (never NaN).
float3 SafeNormalize(float3 vector)
{
	return vector * rsqrt(max(dot(vector, vector), 1e-20));
}

// mul(matrix, vector), with SPIR-V's NoContraction on the product: no implementation may fuse its multiplies and adds with
// each other or with other operations, so every pipeline that links VSMain, whatever its fragment shader lets the driver
// eliminate, computes the same IEEE operations and bit-identical positions. Vulkan guarantees that across pipelines only
// for a position decorated Invariant, which Slang 2026.8 cannot emit (it has no invariant modifier, and `precise` emits no
// NoContraction either), so the decoration is written in SPIR-V assembly, as the same OpVectorTimesMatrix that mul emits.
float4 MultiplyExact(float4x4 matrix, float4 vector)
{
	return spirv_asm {
		%product:$$float4 = OpVectorTimesMatrix $vector $matrix;
		OpDecorate %product NoContraction;
		result:$$float4 = OpCopyObject %product
	};
}

// The normal matrix of `world`'s upper 3x3: its cofactor matrix (rows r1 x r2, r2 x r0, r0 x r1 of the rows r0..r2), which
// equals det * inverse-transpose, negated for a mirroring world so the normals stay outward.
float3x3 MakeNormalMatrix(float4x4 world, bool mirrored)
{
	const float3x3 linear = (float3x3)world;
	const float3x3 cofactor = float3x3(cross(linear[1], linear[2]), cross(linear[2], linear[0]), cross(linear[0], linear[1]));
	return mirrored ? -cofactor : cofactor;
}

// The octahedral encoding of a unit vector into [-1, 1]^2 (Cigolle et al. 2014).
float2 EncodeOctahedral(float3 normal)
{
	const float3 projected = normal / max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-20);
	if (projected.z >= 0.0)
		return projected.xy;
	const float2 signs = select(projected.xy >= 0.0, float2(1.0, 1.0), float2(-1.0, -1.0));
	return (1.0 - abs(projected.yx)) * signs;
}

// A unit vector perpendicular to the unit `normal`, for a tangent the mesh leaves degenerate.
float3 MakePerpendicular(float3 normal)
{
	const float3 axis = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	return normalize(cross(axis, normal));
}

// Samples the material's maps and builds the normal-mapped shading frame.
Surface EvaluateSurface(VertexOutput input, bool isFrontFace)
{
	const float2 uv = input.TexCoord * Material.UVScale + Material.UVOffset;
	const float4 baseColor = BaseColorMap.Sample(AnisoWrap, uv) * Material.BaseColor;
	const float4 metallicRoughness = MetallicRoughnessMap.Sample(AnisoWrap, uv);
	const float3 tangentNormal = NormalMap.Sample(AnisoWrap, uv).xyz * 2.0 - 1.0;
	const float occlusion = OcclusionMap.Sample(AnisoWrap, uv).r;
	const float3 emissiveMap = EmissiveMap.Sample(AnisoWrap, uv).rgb;

	Surface surface;
	surface.BaseColor = baseColor;
	surface.Metallic = saturate(Material.Metallic * metallicRoughness.b);
	surface.PerceptualRoughness = clamp(Material.Roughness * metallicRoughness.g, MinPerceptualRoughness, 1.0);
	surface.Occlusion = 1.0 + Material.OcclusionStrength * (occlusion - 1.0);
	surface.Emissive = (Material.Flags & MaterialFlagEmissiveMap) != 0 ? Material.Emissive * emissiveMap : Material.Emissive;

	// The tangent frame: the tangent made orthogonal to the interpolated normal (Gram-Schmidt), the bitangent from their
	// cross product and the tangent's sign.
	const float3 geometricNormal = SafeNormalize(input.WorldNormal);
	float3 tangent = input.WorldTangent.xyz - geometricNormal * dot(geometricNormal, input.WorldTangent.xyz);
	const float tangentLengthSquared = dot(tangent, tangent);
	tangent = tangentLengthSquared > 1e-12 ? tangent * rsqrt(tangentLengthSquared) : MakePerpendicular(geometricNormal);
	const float3 bitangent = cross(geometricNormal, tangent) * (input.WorldTangent.w < 0.0 ? -1.0 : 1.0);
	const float3 scaled = float3(tangentNormal.xy * Material.NormalScale, tangentNormal.z);
	const float3 normal = SafeNormalize(scaled.x * tangent + scaled.y * bitangent + scaled.z * geometricNormal);

	// A back face (only double-sided materials draw them) is shaded with its normals flipped.
	const bool frontFacing = isFrontFace != IsMirrored();
	surface.Normal = frontFacing ? normal : -normal;
	surface.GeometricNormal = frontFacing ? geometricNormal : -geometricNormal;
	return surface;
}

// The direct light of one light-list entry at `position`.
float3 ShadeLight(ShaderLight light, float3 position, float3 normal, float3 view, float nDotV, float3 diffuseColor, float3 f0,
	float3 energyCompensation, float alpha, float3 reflection)
{
	if (light.Type == ShaderLightTypeDirectional)
	{
		const float3 direction = -light.Direction;
		return EvaluateDirectLight(normal, view, nDotV, diffuseColor, f0, energyCompensation, alpha, direction, direction, alpha, Pi * light.Radiance);
	}

	const float3 toLight = light.Position - position;
	const float distanceSquared = dot(toLight, toLight);
	const float3 direction = toLight * rsqrt(max(distanceSquared, 1e-12));
	float attenuation = WindowedInverseSquare(distanceSquared, light.InverseRangeSquared, light.SourceRadius);
	if (light.Type == ShaderLightTypeSpot)
		attenuation *= SpotConeAttenuation(dot(-direction, light.Direction), light.SpotCosOuter, light.SpotCosInner);
	if (attenuation <= 0.0)
		return float3(0.0);

	float3 specularDirection = direction;
	float specularAlpha = alpha;
	if (light.SourceRadius > 0.0)
	{
		specularDirection = SphereLightSpecularDirection(toLight, reflection, light.SourceRadius);
		specularAlpha = SphereLightAlpha(alpha, light.SourceRadius, sqrt(distanceSquared));
	}
	return EvaluateDirectLight(normal, view, nDotV, diffuseColor, f0, energyCompensation, alpha, direction, specularDirection, specularAlpha,
		Pi * light.Radiance * attenuation);
}

// The quantity a debug view outputs (RenderDebugView), linear.
float3 GetDebugViewValue(Surface surface)
{
	switch (DebugView)
	{
		case SceneDebugViewAlbedo:    return surface.BaseColor.rgb;
		case SceneDebugViewNormals:   return surface.Normal * 0.5 + 0.5;
		case SceneDebugViewRoughness: return float3(surface.PerceptualRoughness);
		case SceneDebugViewMetallic:  return float3(surface.Metallic);
		case SceneDebugViewEmissive:  return saturate(surface.Emissive);
		default:                      return float3(0.0);
	}
}

[shader("vertex")]
VertexOutput VSMain(VertexInput input)
{
	const bool mirrored = IsMirrored();
	const float4 world = MultiplyExact(Draw.World, float4(input.Position, 1.0));
	VertexOutput output;
	output.Position = MultiplyExact(View.ViewProjection, world);
	output.WorldPosition = world.xyz;
	output.WorldNormal = mul(MakeNormalMatrix(Draw.World, mirrored), input.Normal);
	// A mirroring world turns the frame's handedness: the bitangent's sign flips with it.
	output.WorldTangent = float4(mul((float3x3)Draw.World, input.Tangent.xyz), mirrored ? -input.Tangent.w : input.Tangent.w);
	output.TexCoord = input.TexCoord;
	return output;
}

// The coverage of a fragment: all of it, or none for a Mask material's point below the cutoff (ALPHA_MASK).
uint GetCoverage(Surface surface)
{
#if ALPHA_MASK
	return surface.BaseColor.a < Material.AlphaCutoff ? 0u : 0xFFFFFFFFu;
#else
	return 0xFFFFFFFFu;
#endif
}

struct PrepassOutput
{
	float2 Normal : SV_Target0;
#if ALPHA_MASK
	uint Coverage : SV_Coverage;
#endif
};

struct ForwardOutput
{
	float4 Color : SV_Target0;
#if ALPHA_MASK
	uint Coverage : SV_Coverage;
#endif
};

[shader("fragment")]
PrepassOutput PSPrepass(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const Surface surface = EvaluateSurface(input, isFrontFace);
	PrepassOutput output;
	output.Normal = EncodeOctahedral(SafeNormalize(mul((float3x3)View.View, surface.Normal)));
#if ALPHA_MASK
	output.Coverage = GetCoverage(surface);
#endif
	return output;
}

// The forward passes' colour: the debug view's quantity, or the shaded radiance.
float4 ShadeForward(VertexOutput input, Surface surface)
{
	if (DebugView != SceneDebugViewLit)
		return float4(GetDebugViewValue(surface), 1.0);

	// Towards the viewer: per pixel for perspective cameras, the camera's backward axis (the view matrix's third row) for
	// orthographic ones (§8.3).
	const float3 view = View.ProjectionKind == ProjectionKindOrthographic ? SafeNormalize(View.View[2].xyz)
																		 : SafeNormalize(View.CameraPosition - input.WorldPosition);
	const float3 normal = surface.Normal;
	const float nDotV = max(dot(normal, view), 1e-4);
	const float alpha = ApplySpecularAntiAliasing(surface.GeometricNormal, surface.PerceptualRoughness * surface.PerceptualRoughness);
	const float roughness = sqrt(alpha);
	const float3 baseColor = surface.BaseColor.rgb;
	const float3 diffuseColor = baseColor * (1.0 - surface.Metallic);
	const float3 f0 = lerp(float3(0.04), baseColor, surface.Metallic);

	// The DFG LUT (Renderer/BrdfLut.h): single-scatter specular albedo lerp(DFG1, DFG2, f0) and the multi-scatter energy
	// compensation 1 + f0 (1 / DFG2 - 1).
	const float2 dfg = DfgLut.SampleLevel(LinearClamp, float2(nDotV, roughness), 0.0);
	const float3 singleScatter = lerp(dfg.xxx, dfg.yyy, f0);
	const float3 energyCompensation = float3(1.0) + f0 * (1.0 / max(dfg.y, 1e-4) - 1.0);
	const float3 reflection = reflect(-view, normal);

	// Receiver depth follows the triangle plane, not interpolated or normal-mapped shading normals. Evaluate
	// derivatives before any per-light/cascade divergence, then orient the plane for the receiver normal offset.
	float3 shadowNormal = cross(ddx(input.WorldPosition), ddy(input.WorldPosition));
	float shadowNormalLength = dot(shadowNormal, shadowNormal);
	shadowNormal = shadowNormalLength > 1e-20 ? shadowNormal * rsqrt(shadowNormalLength) : surface.GeometricNormal;
	shadowNormal *= dot(shadowNormal, surface.GeometricNormal) < 0.0 ? -1.0 : 1.0;

	float3 radiance = float3(0.0);
	for (uint index = 0; index < View.LightCount; ++index)
	{
		const ShaderLight light = Lights[index];
		float visibility = 1.0;
		if ((Draw.Flags & DrawFlagReceiveShadows) != 0)
		{
			if (light.Type == ShaderLightTypeDirectional && Shadows.Counts.x != 0 && index == Shadows.Counts.z)
				visibility = SampleDirectionalShadow(ShadowMaps, ShadowCompare, Shadows, input.WorldPosition, shadowNormal, -light.Direction,
					-mul(View.View, float4(input.WorldPosition, 1.0)).z, uint2(input.Position.xy));
			else if (light.Type == ShaderLightTypeSpot && Shadows.Counts.y != 0)
				visibility = SampleSpotShadow(ShadowAtlas, ShadowCompare, Shadows, index, input.WorldPosition, shadowNormal,
					SafeNormalize(light.Position - input.WorldPosition), uint2(input.Position.xy));
		}
		radiance += visibility * ShadeLight(light, input.WorldPosition, normal, view, nDotV, diffuseColor, f0, energyCompensation, alpha, reflection);
	}

	// Image-based lighting (§8.6), or the constant ambient as a uniform environment.
	float3 irradiance = Env.AmbientColor;
	float3 prefiltered = Env.AmbientColor;
	if (Env.HasEnvironment != 0)
	{
		irradiance = EvaluateIrradianceSH9(Env.IrradianceSH9, RotateEnvironmentDirection(normal, Env.RotationSin, Env.RotationCos));
		const float3 lookup = RotateEnvironmentDirection(reflection, Env.RotationSin, Env.RotationCos);
		prefiltered = EnvSpecular.SampleLevel(LinearClamp, lookup, roughness * Env.SpecularMaxLod).rgb * Env.Intensity;
	}
	const float3 multiScatter = singleScatter * energyCompensation;
	const float occlusion = min(surface.Occlusion, SampleGtao(AmbientOcclusion, LinearViewDepth, SceneNormals, uint2(input.Position.xy)));
	radiance += irradiance * diffuseColor * saturate(float3(1.0) - multiScatter) * MultiBounceAmbientOcclusion(occlusion, diffuseColor);
	radiance += prefiltered * multiScatter * (SpecularOcclusion(nDotV, occlusion, alpha) * HorizonOcclusion(reflection, surface.GeometricNormal));
	radiance += surface.Emissive;

	const float outputAlpha = Material.AlphaMode == MaterialAlphaModeBlend ? surface.BaseColor.a : 1.0;
	return float4(radiance, outputAlpha);
}

[shader("fragment")]
ForwardOutput PSForward(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const Surface surface = EvaluateSurface(input, isFrontFace);
	ForwardOutput output;
	output.Color = ShadeForward(input, surface);
#if ALPHA_MASK
	output.Coverage = GetCoverage(surface);
#endif
	return output;
}

struct PickingOutput
{
	float2 Normal : SV_Target0;
	uint EntityId : SV_Target1;
	uint Coverage : SV_Coverage;
};

[shader("fragment")]
PickingOutput PSPicking(VertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const Surface surface = EvaluateSurface(input, isFrontFace);
	PickingOutput output;
	output.Normal = EncodeOctahedral(SafeNormalize(mul((float3x3)View.View, surface.Normal)));
	output.EntityId = Draw.EntityId;
	output.Coverage = GetCoverage(surface);
	return output;
}

struct OverdrawOutput
{
	float4 Count : SV_Target0;
	uint Coverage : SV_Coverage;
};

[shader("fragment")]
OverdrawOutput PSOverdraw(VertexOutput input)
{
	const float alpha = BaseColorMap.Sample(AnisoWrap, input.TexCoord * Material.UVScale + Material.UVOffset).a * Material.BaseColor.a;
	const bool rejected = (Material.AlphaMode == MaterialAlphaModeMask && alpha < Material.AlphaCutoff)
		|| (Material.AlphaMode == MaterialAlphaModeBlend && alpha <= 0.0);
	OverdrawOutput output;
	output.Count = float4(1.0, 0.0, 0.0, 0.0);
	output.Coverage = rejected ? 0u : 0xffffffffu;
	return output;
}
