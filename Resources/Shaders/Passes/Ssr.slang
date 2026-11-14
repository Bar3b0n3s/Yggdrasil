// Ssr: glossy screen-space reflections with the probe fallback (M26, C26.1, R-SSR-IN, R-SSR; Docs/Decisions/0037-m26-contract.md
// decisions 14 and 18; Renderer/SsrPass.h; Architecture §8.3), recorded only with PostProcess's SsrEnabled, after the skybox
// and before the transparent pass, in two steps over the opaque image:
//   - CSTrace writes SsrColor (u0) once per pixel. A pixel traces when its surface reflects (SceneRoughness word 0's
//     multiplier is not 0: the sky and surfaces without specular are 0) and its roughness (word 0's a) is at most
//     MaxRoughness: the view ray's reflection about the pixel's normal (SceneNormals), from the pixel's view-space position
//     (SceneDepth) moved one pixel's footprint along the normal (so the ray does not meet the surface it leaves), runs at most
//     MaxDistance metres, clipped to the depths the view sees (in front of the near plane, within an orthographic view's
//     range) and to the screen. It is followed across the depth pyramid's cells (ViewDepth: linear view depth, each mip
//     holding the nearest depth of the texels below it, Passes/DepthPyramid.slang): a cell of a coarse mip whose nearest depth
//     lies behind the ray's whole stretch across it holds no surface the ray reaches, so it is skipped whole and the next cell
//     is tried a mip coarser; any other cell is looked into a mip finer, down to SceneDepth's pixels, where the ray hits the
//     pixel's surface when its stretch across the pixel reaches between the surface's depth and Thickness behind it. A hit on
//     a surface that faces away from the ray (its back side, which the screen does not show) is no hit. A hit writes
//     SceneColor's radiance at the hit pixel (rgb) and its confidence (a): 1, fading to 0 over the last quarter of
//     MaxDistance, over the outer twentieth of the screen and over the upper half of the roughnesses up to MaxRoughness; a
//     miss, or a pixel that does not trace, writes 0. A ray takes at most SsrMaxSteps steps, and nothing random or temporal
//     enters, so the traced image is a function of the frame's inputs.
//   - VSComposite and PSComposite: one fullscreen triangle added into SceneColor (blend One + One, rgb only). The forward pass's
//     specular term (Passes/Scene.slang's ShadeForward) is the prefiltered radiance at the reflection direction (the
//     environment's cube, or the constant ambient, with the reflection probes over it: Common/Probes.slang) times
//     multiScatter * HorizonOcclusion (SceneRoughness word 0's rgb) times SpecularOcclusion(n.v, min(material occlusion, GTAO),
//     alpha) (word 1's x, GTAO sampled as the forward pass samples it); the composite evaluates it from SceneNormals'
//     normal and word 0's roughness, and adds (traced - prefiltered) times that factor times the confidence times
//     Intensity: in proportion to the confidence the traced radiance replaces the probes' reflection, and where a ray missed
//     (confidence 0) the composite adds 0, so the probes' reflection stays (the probe fallback).

#include "Shared/EnvironmentConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/SsrConstants.h"
#include "Shared/ViewConstants.h"

import Common.Environment;
import Common.Gtao;
import Common.Lighting;

[[vk::push_constant]] ConstantBuffer<SsrConstants> Settings;
ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<EnvironmentConstants> Env : register(b2, space0);
// The view's probes (Common/Probes.slang reads them by name): their constants, the volumes' coefficients (declared for the
// include's irradiance term, which neither entry reads) and the reflection probes' prefiltered specular cubes.
ConstantBuffer<ProbeConstants> Probes : register(b4, space0);
Texture2D<float> SceneDepth : register(t0, space0);
Texture2D<float2> SceneNormals : register(t1, space0);
Texture2D<uint2> SceneRoughness : register(t2, space0);
Texture2D<float> ViewDepth : register(t3, space0);
Texture2D<float4> SceneColor : register(t4, space0);
Texture2D<float4> SsrColor : register(t5, space0);
Texture2D<float> AmbientOcclusion : register(t6, space0);
Texture2D<float> LinearViewDepth : register(t7, space0);
TextureCube<float4> EnvSpecular : register(t8, space0);
StructuredBuffer<float4> IrradianceProbes : register(t13, space0);
TextureCubeArray<float4> ReflectionProbeCubes : register(t14, space0);
SamplerState LinearClamp : register(s0, space0);
[vk::image_format("rgba16f")]
RWTexture2D<float4> TracedColor : register(u0, space0);

#include "Common/Probes.slang"

// The most cells one ray steps through.
static const uint SsrMaxSteps = 128;
// The confidence's fades: over this last share of MaxDistance, this outer share of the screen on each side, and this upper share
// of the roughnesses up to MaxRoughness.
static const float SsrDistanceFade = 0.25;
static const float SsrEdgeFade = 0.05;
static const float SsrRoughnessFade = 0.5;
// The linear depth of the sky (SceneDepth 0), behind every ray.
static const float SsrSkyDepth = 3.0e38;
// The pyramid's R16_FLOAT depths are rounded to the nearest: a coarse cell is skipped only when the ray is in front of its
// nearest depth shortened by one part in 1024, at least twice the largest relative rounding (one part in 2048).
static const float SsrPyramidMargin = 0.9990234375;

// The linear view depth (metres along the view axis) of the reverse-Z depth `depth`, as Passes/DepthPyramid.slang computes
// it: Near / depth (perspective) or Far - depth * (Far - Near) (orthographic); SsrSkyDepth for the sky.
float GetLinearDepth(float depth)
{
	if (!(depth > 0.0))
		return SsrSkyDepth;
	return View.ProjectionKind == ProjectionKindOrthographic ? lerp(View.Far, View.Near, saturate(depth)) : View.Near / depth;
}

// The normalized device coordinates of the scene position `pixel` (pixels from the image's top-left corner).
float2 GetNdc(float2 pixel)
{
	return float2(pixel.x * Settings.InverseImageSize.x * 2.0 - 1.0, 1.0 - pixel.y * Settings.InverseImageSize.y * 2.0);
}

// The view-space point at the scene position `pixel` and the reverse-Z depth `depth`.
float3 GetViewPosition(float2 pixel, float depth)
{
	const float4 position = mul(View.InverseProjection, float4(GetNdc(pixel), depth, 1.0));
	return position.xyz / position.w;
}

// The scene position (xy, pixels) and the linear depth (z) of the view-space point `position`.
float3 ProjectToScene(float3 position)
{
	const float4 clip = mul(View.Projection, float4(position, 1.0));
	const float2 ndc = clip.xy / clip.w;
	return float3((ndc.x * 0.5 + 0.5) * float(Settings.ImageSize.x), (0.5 - ndc.y * 0.5) * float(Settings.ImageSize.y), -position.z);
}

// The linear depth at the screen fraction `s` of a ray from the linear depth `start` to `end`: affine in the screen for an
// orthographic view; for a perspective one its reciprocal is.
float GetRayDepth(float s, float start, float end)
{
	if (View.ProjectionKind == ProjectionKindOrthographic)
		return lerp(start, end, s);
	return 1.0 / lerp(1.0 / start, 1.0 / end, s);
}

// The fraction of the ray's view-space length at its screen fraction `s` (start and end its linear depths, as above).
float GetRayFraction(float s, float start, float end)
{
	if (View.ProjectionKind == ProjectionKindOrthographic)
		return s;
	return s * start / ((1.0 - s) * end + s * start);
}

// The size of the depth pyramid's mip `level`.
float2 GetPyramidSize(uint level)
{
	uint width;
	uint height;
	uint levels;
	ViewDepth.GetDimensions(level, width, height, levels);
	return float2(width, height);
}

// The cell (`cell`) of a mip `levelSize` texels across that holds the ray's point at the screen fraction `s` (the ray from
// `start` along `delta`, scene pixels; a point on a cell's boundary is in the cell the ray enters), and the screen fraction
// at which the ray leaves it: at least a hundredth of a pixel further, whatever the rounding. A mip's texel i covers the
// scene positions [i, i + 1) * size / levelSize, which hold every pixel below it (DepthPyramidPass.h's footprints).
float GetCellExit(float2 start, float2 delta, float s, float2 size, float2 levelSize, out int2 cell)
{
	const float2 cellSize = size / levelSize;
	const float2 position = start + delta * s + sign(delta) * 1e-3;
	cell = clamp(int2(floor(position / cellSize)), int2(0, 0), int2(levelSize) - 1);
	const float2 boundary = (float2(cell) + select(delta > float2(0.0), float2(1.0), float2(0.0))) * cellSize;
	float exit = 3.0e38;
	if (delta.x != 0.0)
		exit = min(exit, (boundary.x - start.x) / delta.x);
	if (delta.y != 0.0)
		exit = min(exit, (boundary.y - start.y) / delta.y);
	return max(exit, s + 0.01 / max(max(abs(delta.x), abs(delta.y)), 1e-6));
}

// The traced reflection of `pixel` (CSTrace above): rgb the radiance, a the confidence.
float4 Trace(uint2 pixel)
{
	const float depth = SceneDepth.Load(int3(pixel, 0));
	const uint2 inputs = SceneRoughness.Load(int3(pixel, 0));
	const float roughness = float(inputs.x >> 24) / 255.0;
	if (!(depth > 0.0) || (inputs.x & 0x00FFFFFFu) == 0 || roughness > Settings.MaxRoughness)
		return float4(0.0);

	const bool orthographic = View.ProjectionKind == ProjectionKindOrthographic;
	const float3 surface = GetViewPosition(float2(pixel) + 0.5, depth);
	const float3 normal = DecodeGtaoNormal(SceneNormals.Load(int3(pixel, 0)));
	const float3 direction = reflect(orthographic ? float3(0.0, 0.0, -1.0) : normalize(surface), normal);
	const float footprint = 2.0 * Settings.InverseImageSize.y * (orthographic ? View.OrthoHalfExtents.y : View.TanHalfFovY * -surface.z);
	const float3 origin = surface + normal * footprint;

	// The ray's length: MaxDistance metres, ending at the near plane (a ray towards the camera) or at an orthographic view's far
	// plane.
	const float originDepth = -origin.z;
	float rayLength = Settings.MaxDistance;
	if (direction.z > 0.0)
		rayLength = min(rayLength, (originDepth - View.Near) / direction.z);
	else if (orthographic && direction.z < 0.0)
		rayLength = min(rayLength, (View.Far - originDepth) / -direction.z);
	if (!(rayLength > 0.0))
		return float4(0.0);
	const float3 start = ProjectToScene(origin);
	const float3 end = ProjectToScene(origin + direction * rayLength);
	const float2 delta = end.xy - start.xy;
	// The screen fraction at which the ray leaves the screen, or its end.
	const float2 size = float2(Settings.ImageSize);
	float last = 1.0;
	for (uint axis = 0; axis < 2; ++axis)
	{
		if (delta[axis] > 0.0)
			last = min(last, (size[axis] - start[axis]) / delta[axis]);
		else if (delta[axis] < 0.0)
			last = min(last, -start[axis] / delta[axis]);
	}

	// The cells, from the pixel past the ray's start (whose own surface it leaves) at mip 0.
	const uint levels = max(Settings.PyramidLevels, 1u);
	uint level = 0;
	int2 cell;
	float s = GetCellExit(start.xy, delta, 0.0, size, size, cell);
	bool hit = false;
	float hitFraction = 0.0;
	for (uint step = 0; step < SsrMaxSteps && s < last; ++step)
	{
		const float exit = min(GetCellExit(start.xy, delta, s, size, level == 0 ? size : GetPyramidSize(level), cell), last);
		const float entryDepth = GetRayDepth(s, start.z, end.z);
		const float exitDepth = GetRayDepth(exit, start.z, end.z);
		const float nearest = min(entryDepth, exitDepth);
		const float farthest = max(entryDepth, exitDepth);
		if (level == 0)
		{
			const float surfaceDepth = GetLinearDepth(SceneDepth.Load(int3(cell, 0)));
			if (farthest >= surfaceDepth && nearest <= surfaceDepth + Settings.Thickness)
			{
				hit = true;
				hitFraction = s;
				break;
			}
			s = exit;
			level = min(level + 1, levels - 1);
		}
		else if (farthest < ViewDepth.Load(int3(cell, int(level))) * SsrPyramidMargin)
		{
			s = exit;
			level = min(level + 1, levels - 1);
		}
		else
			--level;
	}
	if (!hit || dot(DecodeGtaoNormal(SceneNormals.Load(int3(cell, 0))), direction) >= 0.0)
		return float4(0.0);

	const float travelled = GetRayFraction(hitFraction, start.z, end.z) * rayLength;
	const float2 uv = (float2(cell) + 0.5) * Settings.InverseImageSize;
	const float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
	const float confidence = saturate((1.0 - travelled / Settings.MaxDistance) / SsrDistanceFade) * saturate(edge / SsrEdgeFade)
		* saturate((Settings.MaxRoughness - roughness) / max(Settings.MaxRoughness * SsrRoughnessFade, 1e-6));
	return float4(SceneColor.Load(int3(cell, 0)).rgb, confidence);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSTrace(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= Settings.ImageSize))
		return;
	TracedColor[id.xy] = Trace(id.xy);
}

struct CompositeVaryings
{
	float4 Position : SV_Position;
};

// The triangle (-1, -1), (3, -1), (-1, 3) covers the viewport.
[shader("vertex")]
CompositeVaryings VSComposite(uint vertexID : SV_VulkanVertexID)
{
	CompositeVaryings output;
	output.Position = float4(vertexID == 1 ? 3.0 : -1.0, vertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);
	return output;
}

// `vector` scaled to unit length, or zero for a zero vector (never NaN), as Passes/Scene.slang's.
float3 SafeNormalize(float3 vector)
{
	return vector * rsqrt(max(dot(vector, vector), 1e-20));
}

// The four unorm8 values of a SceneRoughness word, x in its lowest byte.
float4 UnpackUnorm8(uint word)
{
	return float4(float(word & 0xFFu), float((word >> 8) & 0xFFu), float((word >> 16) & 0xFFu), float(word >> 24)) / 255.0;
}

[shader("fragment")]
float4 PSComposite(CompositeVaryings input) : SV_Target0
{
	const uint2 pixel = uint2(input.Position.xy);
	const float4 traced = SsrColor.Load(int3(pixel, 0));
	const float weight = traced.a * Settings.Intensity;
	if (!(weight > 0.0))
		return float4(0.0);

	// The forward pass's specular term at the pixel (Passes/Scene.slang's ShadeForward), from the prepass's inputs.
	const uint2 inputs = SceneRoughness.Load(int3(pixel, 0));
	const float4 specular = UnpackUnorm8(inputs.x);
	const float roughness = specular.a;
	const float alpha = roughness * roughness;
	const float4 world = mul(View.InverseViewProjection, float4(GetNdc(float2(pixel) + 0.5), SceneDepth.Load(int3(pixel, 0)), 1.0));
	const float3 worldPosition = world.xyz / world.w;
	const float3 normal = SafeNormalize(mul(transpose((float3x3)View.View), DecodeGtaoNormal(SceneNormals.Load(int3(pixel, 0)))));
	const float3 view = View.ProjectionKind == ProjectionKindOrthographic ? SafeNormalize(View.View[2].xyz) : SafeNormalize(View.CameraPosition - worldPosition);
	const float nDotV = max(dot(normal, view), 1e-4);
	const float3 reflection = reflect(-view, normal);
	float3 prefiltered = Env.AmbientColor;
	if (Env.HasEnvironment != 0)
	{
		const float3 lookup = RotateEnvironmentDirection(reflection, Env.RotationSin, Env.RotationCos);
		prefiltered = EnvSpecular.SampleLevel(LinearClamp, lookup, roughness * Env.SpecularMaxLod).rgb * Env.Intensity;
	}
	prefiltered = ShadeProbeSpecular(worldPosition, reflection, roughness, prefiltered);
	const float occlusion = min(UnpackUnorm8(inputs.y).x, SampleGtao(AmbientOcclusion, LinearViewDepth, SceneNormals, pixel));
	const float3 factor = specular.rgb * (SpecularOcclusion(nDotV, occlusion, alpha) * weight);
	return float4((traced.rgb - prefiltered) * factor, 0.0);
}
