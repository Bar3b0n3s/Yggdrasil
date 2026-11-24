// Scene: the mesh passes of the scene renderer (Architecture §8.3 passes 4, 7 and 9, §8.4, §8.5; Renderer/SceneRenderer.h;
// Docs/Decisions/0013-m8-decisions.md decisions 7, 12 and 27). One vertex shader serves every pass and computes its clip
// position with NoContraction products (MultiplyExact), so the prepass and the forward passes compute bit-identical
// positions and the forward pass's GreaterOrEqual test against the prepass depth (reverse-Z, writes off) keeps exactly the
// visible surfaces:
//   - PSPrepass (pass 4) writes the octahedral view-space shading normal (normal-mapped) into SceneNormals (RG16_FLOAT)
//     and, from M26 (Docs/Decisions/0037-m26-contract.md decisions 11 and 14), the pixel's motion vector into SceneVelocity
//     (RG16_FLOAT, Common/Motion.slang) and the forward pass's specular inputs into SceneRoughness (RG32_UINT): word 0 the
//     prefiltered radiance's multiplier before its specular occlusion and the roughness it is sampled at, word 1 the
//     material's occlusion (Renderer/SceneTargetFormats.h), from the forward pass's own expressions (SpecularInputs);
//   - PSForward (passes 7 and 9) writes linear HDR radiance into SceneColor (RGBA16_FLOAT): the glTF 2.0 metallic-roughness
//     material (Shared/MaterialConstants.h) shaded with GGX, the height-correlated Smith visibility, Schlick's Fresnel,
//     Lambert's diffuse and the multi-scatter energy compensation of the DFG LUT (Common/Brdf.slang, Renderer/BrdfLut.h),
//     perceptual roughness clamped to at least 0.045 and widened by geometric specular anti-aliasing, the lights of the
//     view's light list that reach the pixel's light cluster (Common/LightLoop.slang, M23; Common/Lighting.slang's artist
//     units) and image-based lighting: the environment's SH9 irradiance and prefiltered specular cube (split sum), or the
//     constant ambient AmbientColor treated as a uniform environment without a map (Shared/EnvironmentConstants.h), replaced
//     inside their boxes by the view's baked irradiance volumes and reflection probes (M26, Common/Probes.slang), with
//     multi-bounce, specular and horizon occlusion from the material's occlusion; plus emission; then, in a fogged view, the
//     fog in front of it (M32, Common/Fog.slang). Blend materials output the base colour's alpha for the transparent pass's
//     blending.
// Permutation ALPHA_MASK = 1 is the Mask materials' variant: both entry points drop the fragments below AlphaCutoff by
// writing a zero SV_Coverage, which keeps the sample's colour and depth unwritten exactly as a discard would. A discard
// compiles to OpDemoteToHelperInvocation for SPIR-V 1.6, which needs the shaderDemoteToHelperInvocation feature the device
// does not enable; the coverage needs no feature and leaves the pixel quad's derivatives defined. The forward
// pipelines specialize constant 0 (SceneDebugViewConstantId) with the debug view (Shared/DrawConstants.h): a view other
// than Lit outputs its quantity, linear, with alpha 1.
// Vertices are MeshVertex (§6.8): position, normal, tangent (w: the bitangent sign) and texture coordinates at locations
// 0 to 3 (vertex binding 0), and the attribute stream (M23, C-MESH2: MeshVertexAttributes, binding 1) at locations 4 and 5:
// the second texture coordinates, which a map whose MaterialFlag<Map>UV1 bit is set samples, and the vertex colour, which
// always multiplies the base colour (RGBA), as glTF defines it; a mesh without the stream binds (0, 0) and opaque white.
// Every draw is instanced (M23, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): instance i of a draw is
// the view's instance record Instances[Draw.FirstInstance + i] (t10, Shared/InstanceRecord.h), whose world matrix places
// it and whose draw flags (and, for the picking prepass, pick id) the vertex shader hands to the fragment shaders, flat.
// Four vertex entries transform a vertex with the same NoContraction products, one per set of outputs its fragment
// shaders read, since the validation layer reports a flat integer output a fragment shader does not read
// (WARNING-Shader-OutputNotConsumed): VSMain for PSForward, VSPrepass (with the current and previous clip positions, M26)
// for PSPrepass,
// VSPicking (with the clip positions and the pick id) for PSPicking and VSOverdraw for PSOverdraw, which reads only
// the position and texture coordinates of MeshVertex and the attribute stream, the attributes the overdraw pipelines bind
// (as the shadow casters' do), since the layer also reports a vertex attribute the vertex shader does not read. M26: the
// prepass pipelines (VSPrepass and VSPicking) bind vertex binding 2, PREVIOUSPOSITION at location 6, which they read for a
// deformed item's previous positions (an instance with DrawFlagPreviousPositions: M23's Skinning pass keeps them); a draw
// without them binds its own vertex buffer there, never read as positions (Renderer/Private/ForwardPipelines.h). The
// previous clip position is the previous local position through the instance record's PreviousWorld and
// ViewConstants::PreviousViewProjection. The normal matrix is the
// cofactor of the world matrix's upper 3x3 (§8.4), negated for a mirroring world (DrawFlagMirrored), whose draws use the
// CullFront pipelines; front faces are SV_IsFrontFace, inverted when mirrored, and double-sided materials (CullNone) flip
// the normals of back faces.
// M31 (Docs/Decisions/0046-m31-material-parameters.md decisions 4 and 5): an instance with DrawFlagMaterialParameters
// applies its record's material parameters on top of its material: the vertex shader hands the record's index to the
// fragment shaders (flat), which multiply the base colour by its Tint, add its Emissive (and, along a dissolve's edge, its
// DissolveEdge) to the emission and add its UVOffset to every map's coordinates after the material's UV transform. The
// Mask variants, which an Opaque material takes while its instance dissolves or fades (Renderer/RenderPrepare.h's
// SelectMeshAlphaMode), test a material's alpha only when it is a Mask material, and clip a dissolving instance's
// fragments whose noise (DissolveNoise, t16, at the first texture coordinates times DissolveNoiseScale) lies below its
// DissolveAmount and a fading instance's fragments by a 4 x 4 ordered dither of its Opacity (screen-door transparency,
// its thresholds moved by a whole eighth at each frame of TAA's 8-frame jitter cycle, so that over the cycle every pixel
// is covered in Opacity of the frames and TAA resolves a uniform fade); the prepass and the forward passes evaluate the
// same clip (the noise sampled at level 0, outside every per-pixel branch), so the forward pass's depth test keeps
// exactly the prepass's surface. A Blend material's instance multiplies its alpha by Opacity and drops it to 0 where it
// dissolves. An instance without the flag reads none of it.
// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3E and 3G): vertex binding 3 is the mesh's wind
// stream (MeshVertexWind, R8G8B8A8_UNORM; the cache's shared zero stream for a mesh without one), WIND after COLOR in every
// entry's inputs (VSOverdraw's also take NORMAL), and an instance with DrawFlagWind is displaced by Common/Wind.slang's
// ApplyWind after the World product and before the view-projection product (the previous clip position by the previous
// render's wind and PreviousWorld); a draw without the flag runs exactly its earlier arithmetic. The Mask variants also
// clip by the record's signed LodFade with Opacity's threshold (Common/LodFade.slang), and a Blend material's alpha drops
// to 0 where the fade removes a pixel, so two levels cross-fading split their pixels exactly.

#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/InstanceRecord.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/MaterialConstants.h"
#include "Shared/PlanarReflectionConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ViewConstants.h"
#include "Common/ClipPlane.slang"
#include "Common/LodFade.slang"
#include "Common/Shadows.slang"
#include "Common/Wind.slang"

import Common.Brdf;
import Common.Environment;
import Common.Lighting;
import Common.Gtao;
import Common.Motion;

#if !defined(ALPHA_MASK)
	#define ALPHA_MASK 0
#endif

// Set 0: per view (§8.4).
ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<ShadowConstants> Shadows : register(b1, space0);
ConstantBuffer<EnvironmentConstants> Env : register(b2, space0);
ConstantBuffer<LightClusterConstants> Clusters : register(b3, space0); // M23: the cluster grid (Common/LightLoop.slang)
StructuredBuffer<ShaderLight> Lights : register(t0, space0);
Texture2DArray<float> ShadowMaps : register(t1, space0);
Texture2D<float> ShadowAtlas : register(t2, space0);
TextureCube<float4> EnvSpecular : register(t3, space0);
Texture2D<float2> DfgLut : register(t5, space0);
Texture2D<float> AmbientOcclusion : register(t6, space0);
Texture2D<float> LinearViewDepth : register(t8, space0);
Texture2D<float2> SceneNormals : register(t9, space0);
StructuredBuffer<InstanceRecord> Instances : register(t10, space0);
StructuredBuffer<uint> LightClusterMasks : register(t11, space0); // M23: the cluster masks (Renderer/LightClusterPass.h)
TextureCubeArray<float> PointShadowMaps : register(t12, space0); // M23 (R-PLS): ShadowConstants::Points' cubes
// M29 (Docs/Decisions/0044-m29-light-limits.md decision 2): the half tier's cubes, faces of half PointShadowMaps' size.
TextureCubeArray<float> PointShadowMapsHalf : register(t15, space0);
// M26 (R-GI-PROBE): the view's probes (Common/Probes.slang): their constants, the volumes' SH9 coefficients and the
// reflection probes' prefiltered specular cubes.
ConstantBuffer<ProbeConstants> Probes : register(b4, space0);
StructuredBuffer<float4> IrradianceProbes : register(t13, space0);
TextureCubeArray<float4> ReflectionProbeCubes : register(t14, space0);
// M31: the dissolve noise (Renderer/Private/ForwardPipelines.h's DissolveNoiseRegister), sampled with AnisoWrap.
Texture2D<float> DissolveNoise : register(t16, space0);
// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 5): the view's fog (Common/Fog.slang), read by PSForward's Lit output.
ConstantBuffer<FogConstants> Fog : register(b5, space0);
// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the view's planar reflections (Common/PlanarReflections.slang):
// their constants and the four slots' images, which a mirror view, and a view without a drawn reflection in a slot, bind as
// the neutral 1 x 1 black texture.
ConstantBuffer<PlanarReflectionConstants> PlanarReflection : register(b7, space0);
Texture2D<float4> PlanarReflections0 : register(t17, space0);
Texture2D<float4> PlanarReflections1 : register(t18, space0);
Texture2D<float4> PlanarReflections2 : register(t19, space0);
Texture2D<float4> PlanarReflections3 : register(t20, space0);
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

// The forward passes' light loop over the declarations above (M23, Docs/Decisions/0034-m23-contract.md decision 4).
#include "Common/LightLoop.slang"
// The probes' indirect term over the declarations above (M26, Docs/Decisions/0037-m26-contract.md decision 16).
#include "Common/Probes.slang"
// The fog over the declarations above (M32, Docs/Decisions/0047-m32-fog-ssr.md decision 3).
#include "Common/Fog.slang"
// The planar reflections' term over the declarations above (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3).
#include "Common/PlanarReflections.slang"

struct VertexInput
{
	float3 Position : POSITION;
	float3 Normal : NORMAL;
	float4 Tangent : TANGENT;
	float2 TexCoord : TEXCOORD0;
	float2 TexCoord1 : TEXCOORD1; // M23: the attribute stream (binding 1)
	float4 Color : COLOR;
	float4 Wind : WIND; // M46: the wind stream (binding 3), read only with DrawFlagWind
};

// M26: the prepass entries' inputs: VertexInput's, then the instance's previous local position (vertex binding 2, location
// 7 since M46's WIND took 6), read only with DrawFlagPreviousPositions.
struct PrepassVertexInput
{
	VertexInput Vertex;
	float3 PreviousPosition : PREVIOUSPOSITION;
};

// VSMain's outputs, which PSForward reads (and PSPrepass and PSPicking through their own entries' outputs).
struct VertexOutput
{
	float4 Position : SV_Position;
	// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the vertex's signed distance to the view's
	// ViewConstants::ClipPlane, which clips a mirror view's geometry at its glass; 1 in every other view, which clips nothing.
	float ClipDistance : SV_ClipDistance0;
	float3 WorldPosition : WORLDPOSITION;
	float3 WorldNormal : NORMAL;
	float4 WorldTangent : TANGENT;
	float2 TexCoord : TEXCOORD0;
	float2 TexCoord1 : TEXCOORD1;
	float4 Color : COLOR;
	// The instance record's draw flags (a fragment shader has no instance index to read the record with).
	nointerpolation uint Flags : DRAWFLAGS;
	// M31: the instance record's index in Instances, whose material parameters the fragment shaders read with
	// DrawFlagMaterialParameters.
	nointerpolation uint Instance : INSTANCE;
};

// VSPrepass's outputs, which PSPrepass reads: VSMain's and the surface point's current and previous clip positions (M26).
struct PrepassVertexOutput
{
	VertexOutput Surface;
	float4 CurrentClip : CURRENTCLIP;
	float4 PreviousClip : PREVIOUSCLIP;
};

// VSPicking's outputs, which PSPicking reads: VSPrepass's and the instance record's pick id.
struct PickingVertexOutput
{
	VertexOutput Surface;
	float4 CurrentClip : CURRENTCLIP;
	float4 PreviousClip : PREVIOUSCLIP;
	nointerpolation uint EntityId : ENTITYID;
};

// VSOverdraw's inputs: MeshVertex's position and texture coordinates at locations 0 and 1, the attribute stream at 2 and 3,
// and (M46) MeshVertex's normal and the wind stream at 4 and 5, which the wind's displacement reads.
struct OverdrawVertexInput
{
	float3 Position : POSITION;
	float2 TexCoord : TEXCOORD0;
	float2 TexCoord1 : TEXCOORD1;
	float4 Color : COLOR;
	float3 Normal : NORMAL;
	float4 Wind : WIND;
};

// VSOverdraw's outputs, which PSOverdraw reads: the position, and the texture coordinates and vertex alpha of its alpha test.
struct OverdrawVertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float2 TexCoord1 : TEXCOORD1;
	float Alpha : COLOR;
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

bool IsMirrored(uint flags)
{
	return (flags & DrawFlagMirrored) != 0;
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

// The texture coordinates a map samples (M23, Material version 2): the second set when the material's `uv1Flag` is set,
// else the first, through the material's one UV transform.
float2 GetMapTexCoord(float2 texCoord, float2 texCoord1, uint uv1Flag)
{
	return ((Material.Flags & uv1Flag) != 0 ? texCoord1 : texCoord) * Material.UVScale + Material.UVOffset;
}

// M31: whether the instance applies its record's material parameters.
bool HasMaterialParameters(VertexOutput input)
{
	return (input.Flags & DrawFlagMaterialParameters) != 0;
}

// The coordinates a map samples: GetMapTexCoord's, plus the instance's UVOffset with DrawFlagMaterialParameters (M31).
float2 GetSurfaceTexCoord(VertexOutput input, uint uv1Flag)
{
	float2 texCoord = GetMapTexCoord(input.TexCoord, input.TexCoord1, uv1Flag);
	if (HasMaterialParameters(input))
		texCoord += Instances[input.Instance].UVOffset;
	return texCoord;
}

// M31: the dissolve noise of the instance's fragment, at its first texture coordinates times its DissolveNoiseScale. The
// noise has one level, so it is sampled at level 0: no implicit derivative takes part, and the prepass, the forward
// passes and the shadow casters read the same value wherever in their control flow they sample it.
float SampleDissolveNoise(VertexOutput input, InstanceRecord record)
{
	return DissolveNoise.SampleLevel(AnisoWrap, input.TexCoord * record.DissolveNoiseScale, 0.0);
}

// M31: whether a dissolve of `record` removes a fragment of noise `noise`: below DissolveAmount, and everywhere at 1.
bool IsDissolved(InstanceRecord record, float noise)
{
	return record.DissolveAmount >= 1.0 || noise < record.DissolveAmount;
}

// M31: the 4 x 4 ordered-dither threshold of `pixel` in (0, 1) for this frame. The Bayer matrix's index plus one half over
// 16 (its 2 x 2 level from the coordinates' low bits, then the next level from their second bits), moved by the frame's
// phase in TAA's 8-frame jitter cycle, in whole eighths and wrapped into (0, 1). The phase is the frame's index in the
// cycle as the jitter's x gives it: Halton (2, 3)'s base-2 coordinate puts each of the cycle's 8 frames in a different
// eighth, floor((Jitter.x + 0.5) * 8). Over the cycle a pixel's threshold visits 8 evenly spaced values, so it is covered
// in floor or ceil of Opacity x 8 of the frames and TAA resolves a uniform fade; without TAA the jitter is 0 (phase 4),
// a fixed dither.
float GetFadeThreshold(uint2 pixel)
{
	const uint2 p = pixel & 3u;
	const uint low = (((p.x ^ p.y) & 1u) << 1) | (p.y & 1u);
	const uint high = ((((p.x ^ p.y) >> 1) & 1u) << 1) | ((p.y >> 1) & 1u);
	const float threshold = (float((low << 2) | high) + 0.5) / 16.0;
	const float phase = clamp(floor((View.Jitter.x + 0.5) * 8.0), 0.0, 7.0);
	return frac(threshold + phase / 8.0);
}

// Samples the material's maps, each with its UV set, and builds the normal-mapped shading frame. The vertex colour
// multiplies the base colour (M23). M31: an instance with DrawFlagMaterialParameters offsets the maps' coordinates, tints
// the base colour, adds its emission and its dissolve's edge, and, for a Blend material, scales the alpha by its Opacity
// and drops it where it dissolves.
Surface EvaluateSurface(VertexOutput input, bool isFrontFace)
{
	const float4 baseColor = BaseColorMap.Sample(AnisoWrap, GetSurfaceTexCoord(input, MaterialFlagBaseColorUV1)) * Material.BaseColor * input.Color;
	const float4 metallicRoughness = MetallicRoughnessMap.Sample(AnisoWrap, GetSurfaceTexCoord(input, MaterialFlagMetallicRoughnessUV1));
	float3 tangentNormal = NormalMap.Sample(AnisoWrap, GetSurfaceTexCoord(input, MaterialFlagNormalUV1)).xyz * 2.0 - 1.0;
	// A two-channel normal map (BC5, or the decode fallback's RGBA8 copy of one, MaterialFlagNormalMapTwoChannel) stores x
	// and y only: z is reconstructed from them, before NormalScale scales x and y, as glTF's unit normals have it.
	if ((Material.Flags & MaterialFlagNormalMapTwoChannel) != 0)
		tangentNormal.z = sqrt(saturate(1.0 - dot(tangentNormal.xy, tangentNormal.xy)));
	const float occlusion = OcclusionMap.Sample(AnisoWrap, GetSurfaceTexCoord(input, MaterialFlagOcclusionUV1)).r;
	const float3 emissiveMap = EmissiveMap.Sample(AnisoWrap, GetSurfaceTexCoord(input, MaterialFlagEmissiveUV1)).rgb;

	Surface surface;
	surface.BaseColor = baseColor;
	surface.Metallic = saturate(Material.Metallic * metallicRoughness.b);
	surface.PerceptualRoughness = clamp(Material.Roughness * metallicRoughness.g, MinPerceptualRoughness, 1.0);
	surface.Occlusion = 1.0 + Material.OcclusionStrength * (occlusion - 1.0);
	surface.Emissive = (Material.Flags & MaterialFlagEmissiveMap) != 0 ? Material.Emissive * emissiveMap : Material.Emissive;
	if (HasMaterialParameters(input))
	{
		const InstanceRecord record = Instances[input.Instance];
		surface.BaseColor.rgb *= record.Tint;
		surface.Emissive += record.Emissive;
		if (record.DissolveAmount > 0.0)
		{
			const float noise = SampleDissolveNoise(input, record);
			if (noise < record.DissolveAmount + record.DissolveEdgeWidth)
				surface.Emissive += record.DissolveEdge;
			if (Material.AlphaMode == MaterialAlphaModeBlend && IsDissolved(record, noise))
				surface.BaseColor.a = 0.0;
		}
		if (Material.AlphaMode == MaterialAlphaModeBlend)
			surface.BaseColor.a *= record.Opacity;
	}
	// M46: a Blend material's pixel that its level fade removes is fully transparent.
	if (Material.AlphaMode == MaterialAlphaModeBlend && IsLodFaded(Instances[input.Instance].LodFade, GetFadeThreshold(uint2(input.Position.xy))))
		surface.BaseColor.a = 0.0;

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
	const bool frontFacing = isFrontFace != IsMirrored(input.Flags);
	surface.Normal = frontFacing ? normal : -normal;
	surface.GeometricNormal = frontFacing ? geometricNormal : -geometricNormal;
	return surface;
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

// The index in Instances of instance `instance` of the draw. SV_VulkanInstanceID is gl_InstanceIndex itself, which counts
// from 0 within the draw since every draw's own first instance is 0; SV_InstanceID would subtract the base instance and
// need the DrawParameters capability, which needs the shaderDrawParameters device feature.
uint GetInstanceIndex(uint instance)
{
	return Draw.FirstInstance + instance;
}

// Instance `instance` of the draw: its record.
InstanceRecord GetInstance(uint instance)
{
	return Instances[GetInstanceIndex(instance)];
}

// M46: the world position `world` (record.World times the vertex's position) of a vertex of `record`, with normal `normal`
// and wind weights `wind`, displaced by the current render's wind, or with `previous` by the previous render's through
// record.PreviousWorld, when the instance has DrawFlagWind; `world` itself otherwise.
float4 ApplyInstanceWind(float4 world, InstanceRecord record, float3 normal, float4 wind, bool previous)
{
	if (!HasWind(record.Flags))
		return world;
	const float4x4 matrix = previous ? record.PreviousWorld : record.World;
	return float4(ApplyWind(GetWindFrame(previous), world.xyz, WindTransform(matrix, float4(0.0, 0.0, 0.0, 1.0)).xyz,
					  WindTransform(matrix, float4(normal, 0.0)).xyz, wind, record.WindResponse),
		1.0);
}

// The vertex placed by `record`, the record at `recordIndex` of Instances: every vertex entry's position comes from these
// two products, and (M46) the wind's displacement between them.
VertexOutput TransformVertex(VertexInput input, InstanceRecord record, uint recordIndex)
{
	const bool mirrored = IsMirrored(record.Flags);
	const float4 world = ApplyInstanceWind(MultiplyExact(record.World, float4(input.Position, 1.0)), record, input.Normal, input.Wind, false);
	VertexOutput output;
	output.Position = MultiplyExact(View.ViewProjection, world);
	output.ClipDistance = GetClipDistance(View.ClipPlane, world);
	output.WorldPosition = world.xyz;
	output.WorldNormal = mul(MakeNormalMatrix(record.World, mirrored), input.Normal);
	// A mirroring world turns the frame's handedness: the bitangent's sign flips with it.
	output.WorldTangent = float4(mul((float3x3)record.World, input.Tangent.xyz), mirrored ? -input.Tangent.w : input.Tangent.w);
	output.TexCoord = input.TexCoord;
	output.TexCoord1 = input.TexCoord1;
	output.Color = input.Color;
	output.Flags = record.Flags;
	output.Instance = recordIndex;
	return output;
}

// M26: the clip position the vertex had in the previous rendered frame (moved by this frame's jitter, as the current one
// is): its previous local position (a deformed item's previous deformation with DrawFlagPreviousPositions, else its current
// one) through the record's PreviousWorld and the view's PreviousViewProjection, with TransformVertex's NoContraction
// products, so a vertex whose previous inputs equal its current ones gets exactly its SV_Position.
float4 GetPreviousClip(PrepassVertexInput input, InstanceRecord record)
{
	const float3 local = (record.Flags & DrawFlagPreviousPositions) != 0 ? input.PreviousPosition : input.Vertex.Position;
	const float4 world = ApplyInstanceWind(MultiplyExact(record.PreviousWorld, float4(local, 1.0)), record, input.Vertex.Normal, input.Vertex.Wind, true);
	return MultiplyExact(View.PreviousViewProjection, world);
}

[shader("vertex")]
VertexOutput VSMain(VertexInput input, uint instance : SV_VulkanInstanceID)
{
	return TransformVertex(input, GetInstance(instance), GetInstanceIndex(instance));
}

[shader("vertex")]
PrepassVertexOutput VSPrepass(PrepassVertexInput input, uint instance : SV_VulkanInstanceID)
{
	const InstanceRecord record = GetInstance(instance);
	PrepassVertexOutput output;
	output.Surface = TransformVertex(input.Vertex, record, GetInstanceIndex(instance));
	output.CurrentClip = output.Surface.Position;
	output.PreviousClip = GetPreviousClip(input, record);
	return output;
}

[shader("vertex")]
PickingVertexOutput VSPicking(PrepassVertexInput input, uint instance : SV_VulkanInstanceID)
{
	const InstanceRecord record = GetInstance(instance);
	PickingVertexOutput output;
	output.Surface = TransformVertex(input.Vertex, record, GetInstanceIndex(instance));
	output.CurrentClip = output.Surface.Position;
	output.PreviousClip = GetPreviousClip(input, record);
	output.EntityId = record.EntityId;
	return output;
}

[shader("vertex")]
OverdrawVertexOutput VSOverdraw(OverdrawVertexInput input, uint instance : SV_VulkanInstanceID)
{
	const InstanceRecord record = GetInstance(instance);
	OverdrawVertexOutput output;
	const float4 world = ApplyInstanceWind(MultiplyExact(record.World, float4(input.Position, 1.0)), record, input.Normal, input.Wind, false);
	output.Position = MultiplyExact(View.ViewProjection, world);
	output.TexCoord = input.TexCoord;
	output.TexCoord1 = input.TexCoord1;
	output.Alpha = input.Color.a;
	return output;
}

// The coverage of a fragment: all of it, or (ALPHA_MASK) none for a Mask material's point below the cutoff, (M31) for a
// point its instance's dissolve or fade removes and (M46) for a point its level fade removes. An Opaque material draws
// with the Mask variants only for its instance's parameters or level fade, so only a Mask material tests its alpha. The
// parameters' and the level fade's clips are evaluated first, under conditions that hold for the whole primitive, and the
// coverage is returned once at the end.
uint GetCoverage(VertexOutput input, Surface surface)
{
#if ALPHA_MASK
	bool clipped = false;
	const InstanceRecord record = Instances[input.Instance];
	const float threshold = GetFadeThreshold(uint2(input.Position.xy));
	if (HasMaterialParameters(input))
	{
		if (record.DissolveAmount > 0.0)
			clipped = IsDissolved(record, SampleDissolveNoise(input, record));
		clipped = clipped || record.Opacity < threshold;
	}
	clipped = clipped || IsLodFaded(record.LodFade, threshold);
	clipped = clipped || (Material.AlphaMode == MaterialAlphaModeMask && surface.BaseColor.a < Material.AlphaCutoff);
	return clipped ? 0u : 0xFFFFFFFFu;
#else
	return 0xFFFFFFFFu;
#endif
}

// M26: the prepass's attachments (Renderer/SceneTargetFormats.h's order): SceneNormals, SceneVelocity, SceneRoughness.
struct PrepassOutput
{
	float2 Normal : SV_Target0;
	float2 Velocity : SV_Target1;
	uint2 Roughness : SV_Target2;
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

// The view-dependent specular inputs of a shaded point (§8.5): the forward pass shades with them, and the prepass writes
// the ones SSR's composite needs into SceneRoughness (M26), so both evaluate the same expressions.
struct SpecularInputs
{
	float3 ToViewer;           // unit
	float NDotV;               // max(n.v, 1e-4) with the shading normal
	float Alpha;               // GGX alpha, widened by geometric specular anti-aliasing
	float Roughness;           // sqrt(Alpha): the prefiltered cube's and the DFG LUT's roughness coordinate
	float3 F0;                 // the specular reflectance at normal incidence
	float3 SingleScatter;      // the DFG split sum's single-scatter specular albedo lerp(DFG1, DFG2, F0)
	float3 EnergyCompensation; // the multi-scatter energy compensation 1 + F0 (1 / DFG2 - 1)
	float3 Reflection;         // the view direction reflected about the shading normal
};

SpecularInputs EvaluateSpecularInputs(VertexOutput input, Surface surface)
{
	SpecularInputs inputs;
	// Towards the viewer: per pixel for perspective cameras, the camera's backward axis (the view matrix's third row) for
	// orthographic ones (§8.3).
	inputs.ToViewer = View.ProjectionKind == ProjectionKindOrthographic ? SafeNormalize(View.View[2].xyz)
																		: SafeNormalize(View.CameraPosition - input.WorldPosition);
	inputs.NDotV = max(dot(surface.Normal, inputs.ToViewer), 1e-4);
	inputs.Alpha = ApplySpecularAntiAliasing(surface.GeometricNormal, surface.PerceptualRoughness * surface.PerceptualRoughness);
	inputs.Roughness = sqrt(inputs.Alpha);
	inputs.F0 = lerp(float3(0.04), surface.BaseColor.rgb, surface.Metallic);
	// The DFG LUT (Renderer/BrdfLut.h): single-scatter specular albedo lerp(DFG1, DFG2, f0) and the multi-scatter energy
	// compensation 1 + f0 (1 / DFG2 - 1).
	const float2 dfg = DfgLut.SampleLevel(LinearClamp, float2(inputs.NDotV, inputs.Roughness), 0.0);
	inputs.SingleScatter = lerp(dfg.xxx, dfg.yyy, inputs.F0);
	inputs.EnergyCompensation = float3(1.0) + inputs.F0 * (1.0 / max(dfg.y, 1e-4) - 1.0);
	inputs.Reflection = reflect(-inputs.ToViewer, surface.Normal);
	return inputs;
}

// M26 (decision 14): SceneRoughness's two words (Renderer/SceneTargetFormats.h): word 0 rgb the prefiltered radiance's
// multiplier before its specular occlusion (multiScatter times HorizonOcclusion, as ShadeForward multiplies it) and a the
// roughness the forward pass samples the prefiltered cube at; word 1 x the material's occlusion. M42: a drawn planar
// reflection's glass (DrawFlagPlanarReflection) writes a zero multiplier, so SSR neither traces nor composites over it.
uint2 EncodeSpecularInputs(VertexOutput input, Surface surface)
{
	const SpecularInputs specular = EvaluateSpecularInputs(input, surface);
	const float3 multiScatter = specular.SingleScatter * specular.EnergyCompensation;
	const float3 multiplier = (input.Flags & DrawFlagPlanarReflection) != 0 ? float3(0.0)
																			 : multiScatter * HorizonOcclusion(specular.Reflection, surface.GeometricNormal);
	return uint2(PackUnorm4x8(float4(multiplier, specular.Roughness)), PackUnorm4x8(float4(surface.Occlusion, 0.0, 0.0, 0.0)));
}

[shader("fragment")]
PrepassOutput PSPrepass(PrepassVertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const Surface surface = EvaluateSurface(input.Surface, isFrontFace);
	PrepassOutput output;
	output.Normal = EncodeOctahedral(SafeNormalize(mul((float3x3)View.View, surface.Normal)));
	output.Velocity = ComputeMotionVector(input.CurrentClip, input.PreviousClip, View.ViewportSize);
	output.Roughness = EncodeSpecularInputs(input.Surface, surface);
#if ALPHA_MASK
	output.Coverage = GetCoverage(input.Surface, surface);
#endif
	return output;
}

// The forward passes' colour: the debug view's quantity, or the shaded radiance.
float4 ShadeForward(VertexOutput input, Surface surface)
{
	if (DebugView != SceneDebugViewLit)
		return float4(GetDebugViewValue(surface), 1.0);

	const SpecularInputs specular = EvaluateSpecularInputs(input, surface);
	const float3 view = specular.ToViewer;
	const float3 normal = surface.Normal;
	const float nDotV = specular.NDotV;
	const float alpha = specular.Alpha;
	const float roughness = specular.Roughness;
	const float3 baseColor = surface.BaseColor.rgb;
	const float3 diffuseColor = baseColor * (1.0 - surface.Metallic);
	const float3 f0 = specular.F0;
	const float3 singleScatter = specular.SingleScatter;
	const float3 energyCompensation = specular.EnergyCompensation;
	const float3 reflection = specular.Reflection;

	// Receiver depth follows the triangle plane, not interpolated or normal-mapped shading normals. Evaluate
	// derivatives before any per-light/cascade divergence, then orient the plane for the receiver normal offset.
	float3 shadowNormal = cross(ddx(input.WorldPosition), ddy(input.WorldPosition));
	float shadowNormalLength = dot(shadowNormal, shadowNormal);
	shadowNormal = shadowNormalLength > 1e-20 ? shadowNormal * rsqrt(shadowNormalLength) : surface.GeometricNormal;
	shadowNormal *= dot(shadowNormal, surface.GeometricNormal) < 0.0 ? -1.0 : 1.0;

	// The direct light of the pixel's lights (Common/LightLoop.slang): its cluster's, or every light of the list.
	const float viewDepth = -mul(View.View, float4(input.WorldPosition, 1.0)).z;
	float3 radiance = ShadeDirectLights(input.WorldPosition, uint2(input.Position.xy), viewDepth, GetCascadeDepth(input.WorldPosition, viewDepth),
		(input.Flags & DrawFlagReceiveShadows) != 0, shadowNormal, normal, view, nDotV, diffuseColor, f0, energyCompensation, alpha, reflection);

	// Image-based lighting (§8.6), or the constant ambient as a uniform environment.
	float3 irradiance = Env.AmbientColor;
	float3 prefiltered = Env.AmbientColor;
	if (Env.HasEnvironment != 0)
	{
		irradiance = EvaluateIrradianceSH9(Env.IrradianceSH9, RotateEnvironmentDirection(normal, Env.RotationSin, Env.RotationCos));
		const float3 lookup = RotateEnvironmentDirection(reflection, Env.RotationSin, Env.RotationCos);
		prefiltered = EnvSpecular.SampleLevel(LinearClamp, lookup, roughness * Env.SpecularMaxLod).rgb * Env.Intensity;
	}
	// M26 (R-GI-PROBE): the probes' indirect term (Common/Probes.slang), after the light loop: inside their boxes the irradiance
	// volumes replace the diffuse term's irradiance and the reflection probes the specular term's prefiltered radiance.
	irradiance = ShadeProbeIrradiance(input.WorldPosition, normal, irradiance);
	prefiltered = ShadeProbeSpecular(input.WorldPosition, reflection, roughness, prefiltered);
	// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): on a drawn planar reflection's glass, its reflection in
	// place of the probes' or environment's specular, under the same energy terms below.
	if ((input.Flags & DrawFlagPlanarReflection) != 0)
		prefiltered = ShadePlanarReflection(Instances[input.Instance].PlanarReflectionSlot, input.WorldPosition, roughness, alpha, prefiltered);
	const float3 multiScatter = singleScatter * energyCompensation;
	const float occlusion = min(surface.Occlusion, SampleGtao(AmbientOcclusion, LinearViewDepth, SceneNormals, uint2(input.Position.xy)));
	radiance += irradiance * diffuseColor * saturate(float3(1.0) - multiScatter) * MultiBounceAmbientOcclusion(occlusion, diffuseColor);
	radiance += prefiltered * multiScatter * (SpecularOcclusion(nDotV, occlusion, alpha) * HorizonOcclusion(reflection, surface.GeometricNormal));
	radiance += surface.Emissive;
	// M32: the fog in front of the surface, opaque or transparent (a transparent surface blends its fogged colour over the
	// fogged surface behind it). M42: a mirror view's from its glass on.
	if (Fog.Enabled != 0)
		radiance = ApplyFog(radiance, EvaluateViewFog(input.WorldPosition, View.ReflectionView != 0, View.ClipPlane));

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
	output.Coverage = GetCoverage(input, surface);
#endif
	return output;
}

// M26: the picking prepass's attachments (Renderer/SceneTargetFormats.h's order): SceneNormals, EntityId, SceneVelocity,
// SceneRoughness.
struct PickingOutput
{
	float2 Normal : SV_Target0;
	uint EntityId : SV_Target1;
	float2 Velocity : SV_Target2;
	uint2 Roughness : SV_Target3;
	uint Coverage : SV_Coverage;
};

[shader("fragment")]
PickingOutput PSPicking(PickingVertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const Surface surface = EvaluateSurface(input.Surface, isFrontFace);
	PickingOutput output;
	output.Normal = EncodeOctahedral(SafeNormalize(mul((float3x3)View.View, surface.Normal)));
	output.EntityId = input.EntityId;
	output.Velocity = ComputeMotionVector(input.CurrentClip, input.PreviousClip, View.ViewportSize);
	output.Roughness = EncodeSpecularInputs(input.Surface, surface);
	output.Coverage = GetCoverage(input.Surface, surface);
	return output;
}

struct OverdrawOutput
{
	float4 Count : SV_Target0;
	uint Coverage : SV_Coverage;
};

[shader("fragment")]
OverdrawOutput PSOverdraw(OverdrawVertexOutput input)
{
	const float alpha = BaseColorMap.Sample(AnisoWrap, GetMapTexCoord(input.TexCoord, input.TexCoord1, MaterialFlagBaseColorUV1)).a * Material.BaseColor.a
		* input.Alpha;
	const bool rejected = (Material.AlphaMode == MaterialAlphaModeMask && alpha < Material.AlphaCutoff)
		|| (Material.AlphaMode == MaterialAlphaModeBlend && alpha <= 0.0);
	OverdrawOutput output;
	output.Count = float4(1.0, 0.0, 0.0, 0.0);
	output.Coverage = rejected ? 0u : 0xffffffffu;
	return output;
}
