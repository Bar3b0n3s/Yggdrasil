// Water: the Water pass's surfaces (M47, Docs/Decisions/0065-m47-water.md decision 3 and the W47.2 record; Renderer/WaterPass.h,
// Renderer/WaterGeometry.h; Architecture §8.3 "Water"), drawn in the Forward stage after the sky and before SSR into SceneColor,
// SceneNormals, SceneVelocity and SceneRoughness (and EntityId when picking), SceneDepth tested and written, nothing culled.
// Each draw is one clipmap level of one water: its cells (vertex binding 0, RG16_SINT) from the level's corner
// (WaterDrawConstants, the push constants), whose rest points (offsets from the view's records' Origin, Shared/
// WaterConstants.h) a Rectangle clamps into its footprint and Common/Waves.slang displaces forward. Every position comes from
// NoContraction products (MultiplyExact), and a water that neither moved nor advanced its phases since the previous frame
// takes this frame's point as its previous one, so a still water under a still camera has exactly zero motion.
//   - The Ocean's mask: an Ocean's fragment inside any Rectangle's volume (IsInWaterVolume) writes zero coverage, so a
//     cave lake below the sea's level shows under a Headroom that reaches its ceiling.
//   - PSSurface shades above (a front face) or below (a back face, the normals flipped; W47.3's Underwater pass adds the
//     view under the surface): the wave normal, two layers of the normal map drifting by DetailOffset in the water's local
//     X and Z, the forward pass's specular inputs with F0 0.02 and geometric specular anti-aliasing, the view's lights
//     (Common/LightLoop.slang: the sun's glint, the clustered lights, the shadows), the environment's or the probes'
//     irradiance and specular, and fog. Under the surface it shows WaterRefraction (the caller's copy of SceneColor) at the
//     pixel moved by the view-space normal times RefractionStrength (WaterRefractionOffset of the view's height at 1), or
//     at the pixel itself where the moved one's opaque depth (ViewDepth, the depth pyramid's linear depth) lies in front
//     of the surface; it is absorbed over the water column (the opaque depth behind the surface less the surface's, along
//     the view ray) by Beer-Lambert, T = exp(-column / Clarity), and the water's body scatters lerp(DeepColor,
//     ShallowColor, T) in its place, lit as a diffuse surface. Foam (FoamAmount) covers the shallows (the column below
//     ShoreFoamDepth) and the crests (the Jacobian below WaterCrestFoamStart), broken up by the foam map: a white diffuse
//     surface over the body. Fog: the water's own terms are fogged, the fog's colour weighted by its opacity, and the
//     refraction, already fogged by the passes that drew it, is added after. The specular term is SSR's input:
//     SceneRoughness word 0 holds its multiplier and roughness, word 1 the occlusion 1 (x) and the water flag 1 (y), so
//     SSR's composite takes the water's own occlusion, not the seabed's GTAO.
//   - PSPicking adds the water's pick id (EntityId) as SV_Target4.
// Set 0 is the mesh forward passes' (Passes/Scene.slang, Private/ForwardPipelines.h), bound with SceneNormals and the ambient
// occlusion swapped for neutral textures: the program declares all of it, M42's planar reflections included, though it reads
// only part (W47.3's planar opt-in samples them).

#include "Shared/DrawConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/InstanceRecord.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/PlanarReflectionConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ViewConstants.h"
#include "Shared/WaterConstants.h"
#include "Shared/WindConstants.h"
#include "Common/Shadows.slang"

import Common.Brdf;
import Common.Environment;
import Common.Lighting;
import Common.Motion;

// Set 0: the mesh forward passes' per-view set.
ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<ShadowConstants> Shadows : register(b1, space0);
ConstantBuffer<EnvironmentConstants> Env : register(b2, space0);
ConstantBuffer<LightClusterConstants> Clusters : register(b3, space0);
StructuredBuffer<ShaderLight> Lights : register(t0, space0);
Texture2DArray<float> ShadowMaps : register(t1, space0);
Texture2D<float> ShadowAtlas : register(t2, space0);
TextureCube<float4> EnvSpecular : register(t3, space0);
Texture2D<float2> DfgLut : register(t5, space0);
Texture2D<float> AmbientOcclusion : register(t6, space0);           // the neutral white texture, unread
Texture2D<float> LinearViewDepth : register(t8, space0);            // unread: set 1 binds the depth pyramid itself
Texture2D<float2> SceneNormals : register(t9, space0);              // a neutral texture, unread (the water writes SceneNormals)
StructuredBuffer<InstanceRecord> Instances : register(t10, space0); // the mesh draws' records, unread
StructuredBuffer<uint> LightClusterMasks : register(t11, space0);
TextureCubeArray<float> PointShadowMaps : register(t12, space0);
TextureCubeArray<float> PointShadowMapsHalf : register(t15, space0);
ConstantBuffer<ProbeConstants> Probes : register(b4, space0);
StructuredBuffer<float4> IrradianceProbes : register(t13, space0);
TextureCubeArray<float4> ReflectionProbeCubes : register(t14, space0);
Texture2D<float> DissolveNoise : register(t16, space0); // the mesh draws' dissolve noise, unread
ConstantBuffer<FogConstants> Fog : register(b5, space0);
ConstantBuffer<PlanarReflectionConstants> PlanarReflection : register(b7, space0);
Texture2D<float4> PlanarReflections0 : register(t17, space0);
Texture2D<float4> PlanarReflections1 : register(t18, space0);
Texture2D<float4> PlanarReflections2 : register(t19, space0);
Texture2D<float4> PlanarReflections3 : register(t20, space0);
// M43's static records and their indices and M46's wind (phase 8's landing): the mesh draws' items of the forward set the
// water shares, unread, declared so the program's reflection covers the forward layout.
StructuredBuffer<InstanceRecord> StaticInstances : register(t21, space0);
StructuredBuffer<InstanceIndex> InstanceIndices : register(t22, space0);
ConstantBuffer<WindConstants> Wind : register(b8, space0);
SamplerState LinearClamp : register(s0, space0);
SamplerComparisonState ShadowCompare : register(s1, space0);
SamplerState AnisoWrap : register(s2, space0);
[[vk::push_constant]] ConstantBuffer<WaterDrawConstants> Draw;

// Set 1: the water's (Renderer/WaterPass.h).
StructuredBuffer<WaterRecord> Records : register(t0, space1);
Texture2D<float4> Refraction : register(t1, space1);
Texture2D<float> WaterViewDepth : register(t2, space1);
Texture2D<float4> WaterNormalMap : register(t3, space1);
Texture2D<float4> WaterFoamMap : register(t4, space1);
// The Underwater pipeline's set 1 (W47.3): the records (t0) and SceneDepth, read as a texture (t5).
Texture2D<float> UnderwaterSceneDepth : register(t5, space1);

#include "Common/LightLoop.slang"
#include "Common/Probes.slang"
#include "Common/Fog.slang"
#include "Common/PlanarReflections.slang"
#include "Common/Waves.slang"

// The share of the view's height a fully tilted view-space normal moves the refraction at a RefractionStrength of 1.
static const float WaterRefractionOffset = 0.05;
// The crest foam's Jacobian range: none above the start, full at and below the full.
static const float WaterCrestFoamStart = 0.75;
static const float WaterCrestFoamFull = 0.35;
// The foam's diffuse albedo.
static const float WaterFoamAlbedo = 0.85;
// The second detail layer's scale and drift against the first's, and its turn (cos, sin of about 37 degrees).
static const float WaterDetailSecondScale = 1.7;
static const float WaterDetailSecondDrift = 0.75;
static const float2 WaterDetailSecondTurn = float2(0.8, 0.6);
// The foam map's tiling, in DetailScale.
static const float WaterFoamTiling = 2.0;
// W47.3: water's refractive index against air (Snell's window from below). The planar reflection's sampling offset is
// Shared/WaterConstants.h's WaterPlanarDistortion, which the renderer's mirror rectangle allows for.
static const float WaterRefractiveIndex = 1.333;
// The Underwater pass: no water holds the point.
static const uint WaterNone = 0xFFFFFFFF;

struct WaterVertexInput
{
	int2 Cell : CELL; // the vertex's cell from the level's corner
};

struct WaterVertexOutput
{
	float4 Position : SV_Position;
	float3 Relative : RELATIVEPOSITION; // the surface point: x and z from the records' Origin, y the world height
	float2 Rest : REST;                 // the rest point it was displaced from, from the records' Origin
	float4 CurrentClip : CURRENTCLIP;
	float4 PreviousClip : PREVIOUSCLIP;
	nointerpolation uint Water : WATER; // the record's index
};

// mul(matrix, vector) with SPIR-V's NoContraction on the product (Passes/Scene.slang's MultiplyExact).
float4 MultiplyExact(float4x4 matrix, float4 vector)
{
	return spirv_asm {
		%product:$$float4 = OpVectorTimesMatrix $vector $matrix;
		OpDecorate %product NoContraction;
		result:$$float4 = OpCopyObject %product
	};
}

// `vector` scaled to unit length, or zero for a zero vector (never NaN).
float3 SafeNormalize(float3 vector)
{
	return vector * rsqrt(max(dot(vector, vector), 1e-20));
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

// The world point of a displaced surface point of `record`'s water: rest point `rest` plus `displacement`, its x and z from
// the records' Origin.
float4 ToWorld(WaterRecord record, float2 rest, float3 displacement)
{
	return float4((rest.x + displacement.x) + record.Origin.x, record.BaseHeight + displacement.y, (rest.y + displacement.z) + record.Origin.y, 1.0);
}

[shader("vertex")]
WaterVertexOutput VSSurface(WaterVertexInput input)
{
	const WaterRecord record = Records[Draw.Water];
	const float2 rest = ClampToWaterFootprint(record, Draw.LevelOrigin + float2(input.Cell) * Draw.CellSize);
	const WaterWaveSample current = EvaluateWaterWaves(record, rest, false);
	const float4 world = ToWorld(record, rest, current.Displacement);
	float4 previous = world;
	if ((record.Flags & WaterRecordFlagAdvanced) != 0)
		previous = ToWorld(record, rest, EvaluateWaterWaves(record, rest, true).Displacement);
	if ((record.Flags & WaterRecordFlagMoved) != 0)
		previous = MultiplyExact(record.PreviousFromCurrent, previous);
	WaterVertexOutput output;
	output.Position = MultiplyExact(View.ViewProjection, world);
	output.Relative = float3(rest.x + current.Displacement.x, world.y, rest.y + current.Displacement.z);
	output.Rest = rest;
	output.CurrentClip = output.Position;
	output.PreviousClip = MultiplyExact(View.PreviousViewProjection, previous);
	output.Water = Draw.Water;
	return output;
}

// A tangent-space normal of a normal map's texel: x and y from its red and green, z reconstructed (so a two-channel map
// reads alike).
float3 DecodeWaterNormal(float4 texel)
{
	const float2 xy = texel.xy * 2.0 - 1.0;
	return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}

// What the surface writes at a fragment.
struct WaterShading
{
	float3 Radiance;
	float3 Normal; // the shading normal, world space, unit, on the viewer's side
	uint2 Roughness;
	uint Coverage;
};

WaterShading ShadeWater(WaterVertexOutput input, bool isFrontFace)
{
	const WaterRecord record = Records[input.Water];
	WaterShading shading;

	// The Ocean's mask: zero coverage inside any Rectangle's volume.
	bool masked = false;
	if ((record.Flags & WaterRecordFlagRectangle) == 0)
	{
		for (uint other = 0; other < record.WaterCount; ++other)
		{
			const WaterRecord rectangle = Records[other];
			masked = masked || ((rectangle.Flags & WaterRecordFlagRectangle) != 0 && IsInWaterVolume(rectangle, input.Relative));
		}
	}
	shading.Coverage = masked ? 0u : 0xFFFFFFFFu;

	// The waves' normal and Jacobian at the fragment's rest point, then two layers of detail drifting in the water's frame.
	const WaterWaveSample waves = EvaluateWaterWaves(record, input.Rest, false);
	const float2 axisX = record.AxisX;
	const float2 frame = input.Rest - record.FrameOrigin;
	const float2 local = float2(dot(frame, axisX), dot(frame, float2(-axisX.y, axisX.x)));
	const float detailScale = record.Detail.x;
	const float2 detailOffset = record.Detail.yz;
	const float2 firstUv = (local + detailOffset) / detailScale;
	const float2 turned = float2(local.x * WaterDetailSecondTurn.x - local.y * WaterDetailSecondTurn.y, local.x * WaterDetailSecondTurn.y + local.y * WaterDetailSecondTurn.x);
	const float2 secondUv = (turned - detailOffset * WaterDetailSecondDrift) / (detailScale * WaterDetailSecondScale);
	const float3 first = DecodeWaterNormal(WaterNormalMap.Sample(AnisoWrap, firstUv));
	const float3 second = DecodeWaterNormal(WaterNormalMap.Sample(AnisoWrap, secondUv));
	const float foamTexel = WaterFoamMap.Sample(AnisoWrap, firstUv / WaterFoamTiling).r;
	const float3 detail = SafeNormalize(float3((first.xy + second.xy) * record.Look.w, first.z * second.z));
	const float3 waveNormal = waves.Normal;
	const float3 axis = float3(axisX.x, 0.0, axisX.y);
	const float3 tangent = SafeNormalize(axis - waveNormal * dot(waveNormal, axis));
	const float3 bitangent = cross(waveNormal, tangent);
	const float3 detailed = SafeNormalize(detail.x * tangent + detail.y * bitangent + detail.z * waveNormal);
	const bool above = isFrontFace;
	const float3 normal = above ? detailed : -detailed;
	const float3 geometricNormal = above ? waveNormal : -waveNormal;
	shading.Normal = normal;

	// The forward pass's specular inputs (Passes/Scene.slang), with a dielectric's F0 of 0.02.
	const float3 worldPosition = float3(input.Relative.x + record.Origin.x, input.Relative.y, input.Relative.z + record.Origin.y);
	const float3 view = View.ProjectionKind == ProjectionKindOrthographic ? SafeNormalize(View.View[2].xyz) : SafeNormalize(View.CameraPosition - worldPosition);
	const float nDotV = max(dot(normal, view), 1e-4);
	const float perceptualRoughness = clamp(record.DeepColor.a, MinPerceptualRoughness, 1.0);
	const float alpha = ApplySpecularAntiAliasing(geometricNormal, perceptualRoughness * perceptualRoughness);
	const float roughness = sqrt(alpha);
	const float3 f0 = float3(0.02);
	const float2 dfg = DfgLut.SampleLevel(LinearClamp, float2(nDotV, roughness), 0.0);
	const float3 energyCompensation = float3(1.0) + f0 * (1.0 / max(dfg.y, 1e-4) - 1.0);
	const float3 multiScatter = lerp(dfg.xxx, dfg.yyy, f0) * energyCompensation;
	const float3 reflection = reflect(-view, normal);

	// The refracted scene under the surface and the water column it is seen through.
	const int2 pixel = int2(input.Position.xy);
	const int2 lastPixel = int2(View.ViewportSize) - 1;
	const float4 viewPosition = mul(View.View, float4(worldPosition, 1.0));
	const float surfaceDepth = -viewPosition.z;
	const float rayScale = View.ProjectionKind == ProjectionKindOrthographic ? 1.0 : length(viewPosition.xyz) / max(surfaceDepth, 1e-6);
	const float3 viewNormal = mul((float3x3)View.View, normal);
	const float2 shift = float2(viewNormal.x, -viewNormal.y) * (record.Look.x * WaterRefractionOffset * View.ViewportSize.y);
	int2 refracted = clamp(int2(input.Position.xy + shift), int2(0, 0), lastPixel);
	float behind = WaterViewDepth.Load(int3(refracted, 0));
	if (behind < surfaceDepth)
	{
		refracted = pixel;
		behind = WaterViewDepth.Load(int3(pixel, 0));
	}
	const float column = max(behind - surfaceDepth, 0.0) * rayScale;
	const float3 refractedColor = Refraction.Load(int3(refracted, 0)).rgb;
	const float transmittance = above ? exp(-column / record.ShallowColor.a) : 1.0;

	// From below (W47.3): inside Snell's window, where the ray leaves the water into the air (sin of the angle in the air
	// below 1), the world above refracted, less the share Fresnel's law (Schlick's, at the angle in the air) reflects back;
	// outside it the surface reflects the water below wholly (total internal reflection), tinted UnderwaterColor. The
	// Underwater pass fogs the path to it. SSR traces nothing here.
	if (!above)
	{
		const float cosIncident = saturate(dot(normal, view));
		const float sinOutside2 = WaterRefractiveIndex * WaterRefractiveIndex * (1.0 - cosIncident * cosIncident);
		const float reflected = sinOutside2 < 1.0 ? 0.02 + 0.98 * pow(1.0 - sqrt(1.0 - sinOutside2), 5.0) : 1.0;
		shading.Radiance = refractedColor * (1.0 - reflected) + record.Underwater.rgb * reflected;
		shading.Roughness = uint2(PackUnorm4x8(float4(0.0, 0.0, 0.0, roughness)), PackUnorm4x8(float4(1.0, 1.0, 0.0, 0.0)));
		return shading;
	}

	// Foam on the shallows and the crests, a white diffuse surface over the body.
	const float shore = above ? saturate(1.0 - column / max(record.Look.z, 1e-4)) : 0.0;
	const float crest = saturate((WaterCrestFoamStart - waves.Jacobian) / (WaterCrestFoamStart - WaterCrestFoamFull));
	const float foam = record.Look.y * saturate(max(shore, crest) * 2.0 * foamTexel);
	const float3 body = lerp(record.DeepColor.rgb, record.ShallowColor.rgb, transmittance) * (1.0 - transmittance);
	const float3 diffuseColor = lerp(body, float3(WaterFoamAlbedo), foam);

	// The view's lights, then the environment's or the probes' irradiance and specular (§8.6), then the refraction.
	const float viewDepth = surfaceDepth;
	float3 radiance = ShadeDirectLights(worldPosition, uint2(pixel), viewDepth, GetCascadeDepth(worldPosition, viewDepth), true, geometricNormal, normal, view, nDotV,
		diffuseColor, f0 * (1.0 - foam), energyCompensation, alpha, reflection);
	float3 irradiance = Env.AmbientColor;
	float3 prefiltered = Env.AmbientColor;
	if (Env.HasEnvironment != 0)
	{
		irradiance = EvaluateIrradianceSH9(Env.IrradianceSH9, RotateEnvironmentDirection(normal, Env.RotationSin, Env.RotationCos));
		const float3 lookup = RotateEnvironmentDirection(reflection, Env.RotationSin, Env.RotationCos);
		prefiltered = EnvSpecular.SampleLevel(LinearClamp, lookup, roughness * Env.SpecularMaxLod).rgb * Env.Intensity;
	}
	irradiance = ShadeProbeIrradiance(worldPosition, normal, irradiance);
	prefiltered = ShadeProbeSpecular(worldPosition, reflection, roughness, prefiltered);
	// W47.3's planar opt-in: a water with a drawn planar reflection slot reflects its base plane's mirror view in place of the
	// probe term, sampled at the point moved by the wave normal (M42's glass, Common/PlanarReflections.slang), and SSR
	// neither traces nor composites over it (a zero multiplier, as M42's glass).
	const bool planar = record.PlanarSlot != WaterNoPlanarSlot;
	if (planar)
	{
		const float3 planarPoint = float3(worldPosition.x + normal.x * WaterPlanarDistortion, record.BaseHeight, worldPosition.z + normal.z * WaterPlanarDistortion);
		prefiltered = ShadePlanarReflection(record.PlanarSlot, planarPoint, roughness, alpha, prefiltered);
	}
	const float3 transmitted = saturate(float3(1.0) - multiScatter);
	const float3 specularMultiplier = multiScatter * ((1.0 - foam) * HorizonOcclusion(reflection, geometricNormal));
	radiance += irradiance * diffuseColor * transmitted;
	radiance += prefiltered * specularMultiplier * SpecularOcclusion(nDotV, 1.0, alpha);
	// The refraction is SceneColor's, which the opaque, terrain and sky passes fogged over the whole path to what lies behind:
	// only the water's own terms are fogged, the fog's colour weighted by the water's opacity (1 - the transmission), as the
	// transparent pass blends a fogged surface over its fogged background, and the refraction is added after.
	const float3 transmission = (transmittance * (1.0 - foam)) * transmitted;
	if (Fog.Enabled != 0)
	{
		const float4 fog = EvaluateViewFog(worldPosition, View.ReflectionView != 0, View.ClipPlane);
		radiance = radiance * (1.0 - fog.a) + fog.rgb * (fog.a * (float3(1.0) - transmission));
	}
	radiance += refractedColor * transmission;
	shading.Radiance = radiance;
	// SceneRoughness (SceneTargetFormats.h): the specular multiplier and roughness; the occlusion 1 and the water flag.
	shading.Roughness = uint2(PackUnorm4x8(float4(planar ? float3(0.0) : specularMultiplier, roughness)), PackUnorm4x8(float4(1.0, 1.0, 0.0, 0.0)));
	return shading;
}

// The surfaces' attachments: SceneColor, SceneNormals, SceneVelocity, SceneRoughness (WaterPass's framebuffer order).
struct WaterSurfaceOutput
{
	float4 Color : SV_Target0;
	float2 Normal : SV_Target1;
	float2 Velocity : SV_Target2;
	uint2 Roughness : SV_Target3;
	uint Coverage : SV_Coverage;
};

// The picking surfaces' attachments: the surfaces', then EntityId.
struct WaterPickingOutput
{
	float4 Color : SV_Target0;
	float2 Normal : SV_Target1;
	float2 Velocity : SV_Target2;
	uint2 Roughness : SV_Target3;
	uint EntityId : SV_Target4;
	uint Coverage : SV_Coverage;
};

[shader("fragment")]
WaterSurfaceOutput PSSurface(WaterVertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const WaterShading shading = ShadeWater(input, isFrontFace);
	WaterSurfaceOutput output;
	output.Color = float4(shading.Radiance, 1.0);
	output.Normal = EncodeOctahedral(SafeNormalize(mul((float3x3)View.View, shading.Normal)));
	output.Velocity = ComputeMotionVector(input.CurrentClip, input.PreviousClip, View.ViewportSize);
	output.Roughness = shading.Roughness;
	output.Coverage = shading.Coverage;
	return output;
}

[shader("fragment")]
WaterPickingOutput PSPicking(WaterVertexOutput input, bool isFrontFace : SV_IsFrontFace)
{
	const WaterShading shading = ShadeWater(input, isFrontFace);
	WaterPickingOutput output;
	output.Color = float4(shading.Radiance, 1.0);
	output.Normal = EncodeOctahedral(SafeNormalize(mul((float3x3)View.View, shading.Normal)));
	output.Velocity = ComputeMotionVector(input.CurrentClip, input.PreviousClip, View.ViewportSize);
	output.Roughness = shading.Roughness;
	output.EntityId = Records[input.Water].PickId;
	output.Coverage = shading.Coverage;
	return output;
}

// The Underwater pass (W47.3; Renderer/WaterPass.h's RecordUnderwater): one triangle over the viewport, first in HdrPost,
// blended into SceneColor (SrcAlpha, InvSrcAlpha; its alpha kept). A pixel whose point on the near plane lies under the
// water that holds it (the point rule over the view's records: the Rectangle whose volume holds it with the highest surface
// there, ties to the first in snapshot order, else the Ocean; the surface the clipmap draws, GetWaterSurfaceHeight) fogs
// its view ray from the near plane to SceneDepth's point, a Rectangle's clipped to the sides and floor of its volume,
// toward the water's UnderwaterColor: amount 1 - exp(-length / UnderwaterVisibility). Every other pixel writes zero
// coverage, so a camera straddling the surface draws one waterline where the near plane crosses the waves.
struct UnderwaterVaryings
{
	float4 Position : SV_Position;
};

[shader("vertex")]
UnderwaterVaryings VSUnderwater(uint vertexID : SV_VulkanVertexID)
{
	UnderwaterVaryings output;
	output.Position = float4(vertexID == 1 ? 3.0 : -1.0, vertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);
	return output;
}

// The world point at the scene position `pixel` (pixels from the top-left corner) and the reverse-Z depth `depth` (1 the
// near plane).
float3 GetViewPoint(float2 pixel, float depth)
{
	const float2 ndc = float2(pixel.x * View.InverseViewportSize.x * 2.0 - 1.0, 1.0 - pixel.y * View.InverseViewportSize.y * 2.0);
	const float4 world = mul(View.InverseViewProjection, float4(ndc, depth, 1.0));
	return world.xyz / world.w;
}

// The metres the ray from `start` along the unit `direction` (both relative to the records' Origin in x and z) runs inside
// the Rectangle `record`'s footprint and above its floor (Depth below its base): its exit through a side or the floor.
float GetRectangleExitDistance(WaterRecord record, float3 start, float3 direction)
{
	const float2 axisX = record.AxisX;
	const float2 axisZ = float2(-axisX.y, axisX.x);
	const float2 relative = start.xz - record.FrameOrigin;
	const float2 local = float2(dot(relative, axisX), dot(relative, axisZ));
	const float2 along = float2(dot(direction.xz, axisX), dot(direction.xz, axisZ));
	float exit = 3.0e38;
	for (uint axis = 0; axis < 2; ++axis)
	{
		if (along[axis] > 1e-6)
			exit = min(exit, (record.HalfExtents[axis] - local[axis]) / along[axis]);
		else if (along[axis] < -1e-6)
			exit = min(exit, (-record.HalfExtents[axis] - local[axis]) / along[axis]);
	}
	if (direction.y < -1e-6)
		exit = min(exit, (record.BaseHeight - record.Depth - start.y) / direction.y);
	return max(exit, 0.0);
}

struct UnderwaterOutput
{
	float4 Color : SV_Target0;
	uint Coverage : SV_Coverage;
};

[shader("fragment")]
UnderwaterOutput PSUnderwater(UnderwaterVaryings input)
{
	UnderwaterOutput output;
	output.Color = float4(0.0);
	output.Coverage = 0u;
	const WaterRecord first = Records[0];
	const float3 nearPoint = GetViewPoint(input.Position.xy, 1.0);
	const float3 start = float3(nearPoint.x - first.Origin.x, nearPoint.y, nearPoint.z - first.Origin.y);

	// The point rule.
	uint holder = WaterNone;
	uint ocean = WaterNone;
	float surface = 0.0;
	for (uint index = 0; index < first.WaterCount; ++index)
	{
		const WaterRecord record = Records[index];
		if ((record.Flags & WaterRecordFlagRectangle) == 0)
		{
			ocean = ocean == WaterNone ? index : ocean;
			continue;
		}
		if (!IsInWaterVolume(record, start))
			continue;
		const float height = GetWaterSurfaceHeight(record, start.xz);
		if (holder == WaterNone || height > surface)
		{
			holder = index;
			surface = height;
		}
	}
	if (holder == WaterNone && ocean != WaterNone)
	{
		holder = ocean;
		surface = GetWaterSurfaceHeight(Records[ocean], start.xz);
	}
	if (holder == WaterNone || !(start.y < surface))
		return output;

	// The fog of the under-water segment: to SceneDepth's point, or without one (the far plane) without end.
	const WaterRecord record = Records[holder];
	const float depth = UnderwaterSceneDepth.Load(int3(int2(input.Position.xy), 0));
	float distance = 3.0e38;
	if (depth > 0.0)
		distance = length(GetViewPoint(input.Position.xy, depth) - nearPoint);
	if ((record.Flags & WaterRecordFlagRectangle) != 0)
	{
		const float3 direction = SafeNormalize(GetViewPoint(input.Position.xy, 0.5) - nearPoint);
		distance = min(distance, GetRectangleExitDistance(record, start, direction));
	}
	output.Color = float4(record.Underwater.rgb, 1.0 - exp(-distance / max(record.Underwater.a, 1e-3)));
	output.Coverage = 0xFFFFFFFFu;
	return output;
}
