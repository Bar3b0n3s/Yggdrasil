// Ssr: glossy screen-space reflections with the probe fallback (M26, C26.1, R-SSR-IN, R-SSR; Docs/Decisions/0037-m26-contract.md
// decisions 14 and 18; Renderer/SsrPass.h; Architecture §8.3), recorded only with PostProcess's SsrEnabled, after the skybox
// and before the transparent pass, in three steps over the opaque image (M32, Docs/Decisions/0047-m32-fog-ssr.md decisions 10
// and 11: the cost follows the glossy area). The trace image is the scene image, or with Settings.HalfResolution its half
// (ceil(width / 2) x ceil(height / 2)), whose pixel p traces the scene pixel GtaoFullPixel maps it to; it is cut into 8 x 8
// tiles:
//   - CSClassify, one group per tile: a tile any of whose trace pixels traces (IsTracedPixel, CSTrace's own test) appends
//     itself to Tiles (u1: the listed count n in element 0, then each tile as x | y << 16) and its tracing pixels' count to
//     Arguments (u2: the trace's dispatch arguments at byte 0, the composite's draw arguments (6, n, 0, 0) at byte 12, the
//     traced pixels at byte 28), which the pass resets before it. The trace runs in rows of at most SsrTraceRowGroups groups
//     (Shared/SsrConstants.h), so its arguments are (min(n, SsrTraceRowGroups), ceil(n / SsrTraceRowGroups), 1): each listing
//     group raises x to min(its slot + 1, SsrTraceRowGroups) and y to its slot's row + 1 (atomic maxima, so the order the
//     groups run in does not matter). The tiles' order follows the atomics, and each tile writes only its own pixels, so the
//     image does not depend on it.
//   - CSTrace, dispatched indirectly, group (x, y) for listed tile y * SsrTraceRowGroups + x (a group past n returns),
//     writes SsrColor (u0) once per pixel of its tile. A pixel
//     traces when its surface reflects (SceneRoughness word 0's multiplier is not 0: the sky and surfaces without specular are
//     0) and its roughness (word 0's a) is at most MaxRoughness: the view ray's reflection about the pixel's normal
//     (SceneNormals), from the pixel's view-space position (SceneDepth) moved one pixel's footprint along the normal (so the
//     ray does not meet the surface it leaves), runs at most MaxDistance metres, clipped to the depths the view sees (in front
//     of the near plane, within an orthographic view's range) and to the screen. It is followed across the depth pyramid's
//     cells (ViewDepth: linear view depth, each mip holding the nearest depth of the texels below it,
//     Passes/DepthPyramid.slang): a cell of a coarse mip whose nearest depth lies behind the ray's whole stretch across it
//     holds no surface the ray reaches, so it is skipped whole and the next cell is tried a mip coarser; any other cell is
//     looked into a mip finer, down to SceneDepth's pixels, where the ray hits the pixel's surface when its stretch across the
//     pixel reaches between the surface's depth and Thickness behind it. A hit on a surface that faces away from the ray (its
//     back side, which the screen does not show) is no hit. A hit writes SceneColor's radiance at the hit pixel (rgb) and its
//     confidence (a): 1, fading to 0 over the last quarter of MaxDistance, over the outer twentieth of the screen and over the
//     upper half of the roughnesses up to MaxRoughness; a miss, or a pixel that does not trace, writes 0. A ray takes at most
//     SsrMaxSteps steps, and nothing random or temporal enters, so the traced image is a function of the frame's inputs.
//   - VSComposite and PSComposite, drawn indirectly, one instance per listed tile: two triangles covering the tile's scene
//     pixels (8 x 8, or 16 x 16 at half resolution) added into SceneColor (blend One + One, rgb only); a pixel of an unlisted
//     tile traced nothing, and the composite added 0 there before M32, so the image is the same. The forward pass's
//     specular term (Passes/Scene.slang's ShadeForward) is the prefiltered radiance at the reflection direction (the
//     environment's cube, or the constant ambient, with the reflection probes over it: Common/Probes.slang) times
//     multiScatter * HorizonOcclusion (SceneRoughness word 0's rgb) times SpecularOcclusion(n.v, min(material occlusion, GTAO),
//     alpha) (word 1's x, GTAO sampled as the forward pass samples it); the composite evaluates it from SceneNormals'
//     normal and word 0's roughness, and adds (traced - prefiltered) times that factor times the confidence times
//     Intensity: in proportion to the confidence the traced radiance replaces the probes' reflection, and where a ray missed
//     (confidence 0) the composite adds 0, so the probes' reflection stays (the probe fallback). At half resolution the
//     traced reflection of a scene pixel that traces itself is reconstructed from SsrColor's four bilateral taps around it,
//     with GTAO's weights (Common/Gtao.slang's GtaoFullPixel and GtaoEdgeWeight over ViewDepth's linear depth and
//     SceneNormals), each tap's radiance weighed by its confidence; a pixel that does not trace adds nothing, and neither
//     does one of an unlisted tile (none of the tile's trace pixels traced, though a tap of a pixel at its edge may lie in a
//     listed neighbour; the pass clears SsrColor at half resolution, so no tap reads an earlier frame's trace). M32
//     (Docs/Decisions/0047-m32-fog-ssr.md decision 6): in a fogged view the addition is multiplied by 1 - the fog's amount at
//     the pixel (Common/Fog.slang), so the forward pass's fogged image and the reflection fog as one surface.

#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
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
// M32: the listed tiles, which CSTrace and VSComposite read (t15), CSClassify's list and the indirect arguments (u1, u2).
StructuredBuffer<uint> Tiles : register(t15, space0);
Texture2D<float> AmbientOcclusion : register(t6, space0);
Texture2D<float> LinearViewDepth : register(t7, space0);
TextureCube<float4> EnvSpecular : register(t8, space0);
StructuredBuffer<float4> IrradianceProbes : register(t13, space0);
TextureCubeArray<float4> ReflectionProbeCubes : register(t14, space0);
// M32: the view's fog, which the composite's addition passes through.
ConstantBuffer<FogConstants> Fog : register(b5, space0);
SamplerState LinearClamp : register(s0, space0);
[vk::image_format("rgba16f")]
RWTexture2D<float4> TracedColor : register(u0, space0);
RWStructuredBuffer<uint> TileList : register(u1, space0);
RWByteAddressBuffer Arguments : register(u2, space0);

#include "Common/Probes.slang"
#include "Common/Fog.slang"

// The most cells one ray steps through.
static const uint SsrMaxSteps = 128;
// The confidence's fades: over this last share of MaxDistance, this outer share of the screen on each side, and this upper share
// of the roughnesses up to MaxRoughness.
static const float SsrDistanceFade = 0.25;
static const float SsrEdgeFade = 0.05;
static const float SsrRoughnessFade = 0.5;
// The linear depth of the sky (SceneDepth 0), behind every ray.
static const float SsrSkyDepth = 3.0e38;
// The trace image's tiles are 8 x 8 trace pixels (CSClassify's and CSTrace's groups).
static const uint SsrTileSize = 8;
// Arguments' byte offsets (Renderer/SsrPass.h's SsrArgumentsLayout): the trace's groups along x and y, the composite's
// instance count and the traced pixels.
static const uint SsrTraceGroupsOffset = 0;
static const uint SsrTraceRowsOffset = 4;
static const uint SsrCompositeInstancesOffset = 16;
static const uint SsrTracedPixelsOffset = 28;
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

// The trace image's size: the scene image's, or its half rounded up.
uint2 GetTraceSize()
{
	return Settings.HalfResolution != 0 ? (Settings.ImageSize + 1u) / 2u : Settings.ImageSize;
}

// The scene pixel a trace pixel traces: itself, or at half resolution the pixel at the centre of its footprint.
uint2 GetTracedScenePixel(uint2 tracePixel)
{
	return Settings.HalfResolution != 0 ? GtaoFullPixel(tracePixel, Settings.ImageSize, GetTraceSize()) : tracePixel;
}

// Whether the scene pixel `pixel` traces: Trace's own test (its surface reflects, is not the sky and is at most MaxRoughness
// rough), which Trace keeps as it is.
bool IsTracedPixel(uint2 pixel)
{
	const float depth = SceneDepth.Load(int3(pixel, 0));
	const uint2 inputs = SceneRoughness.Load(int3(pixel, 0));
	const float roughness = float(inputs.x >> 24) / 255.0;
	return depth > 0.0 && (inputs.x & 0x00FFFFFFu) != 0 && !(roughness > Settings.MaxRoughness);
}

// The tracing trace pixels of the group's tile.
groupshared uint TileTracedPixels;

[shader("compute")]
[numthreads(8, 8, 1)]
void CSClassify(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
	if (index == 0)
		TileTracedPixels = 0;
	GroupMemoryBarrierWithGroupSync();
	if (all(id.xy < GetTraceSize()) && IsTracedPixel(GetTracedScenePixel(id.xy)))
		InterlockedAdd(TileTracedPixels, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (index != 0 || TileTracedPixels == 0)
		return;
	uint slot;
	InterlockedAdd(TileList[0], 1u, slot);
	Arguments.InterlockedMax(SsrTraceGroupsOffset, min(slot + 1u, SsrTraceRowGroups));
	Arguments.InterlockedMax(SsrTraceRowsOffset, slot / SsrTraceRowGroups + 1u);
	Arguments.InterlockedAdd(SsrCompositeInstancesOffset, 1u);
	Arguments.InterlockedAdd(SsrTracedPixelsOffset, TileTracedPixels);
	TileList[1u + slot] = group.x | (group.y << 16);
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

// The listed tile's corner in trace pixels.
uint2 GetTileOrigin(uint tile)
{
	return uint2(tile & 0xFFFFu, tile >> 16) * SsrTileSize;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSTrace(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
	// The last row's groups past the listed count trace nothing.
	const uint listed = group.y * SsrTraceRowGroups + group.x;
	if (listed >= Tiles[0])
		return;
	const uint2 tracePixel = GetTileOrigin(Tiles[1u + listed]) + thread.xy;
	if (any(tracePixel >= GetTraceSize()))
		return;
	TracedColor[tracePixel] = Trace(GetTracedScenePixel(tracePixel));
}

struct CompositeVaryings
{
	float4 Position : SV_Position;
};

// Two triangles, (0, 0), (1, 0), (1, 1) and (0, 0), (1, 1), (0, 1) in tile units, cover the instance's listed tile: its trace
// pixels' scene pixels, 8 x 8, or 16 x 16 at half resolution (the viewport clips the tiles at the image's edges).
[shader("vertex")]
CompositeVaryings VSComposite(uint vertexID : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID)
{
	const float2 corner = float2(vertexID == 1 || vertexID == 2 || vertexID == 4 ? 1.0 : 0.0, vertexID == 2 || vertexID == 4 || vertexID == 5 ? 1.0 : 0.0);
	const float scale = Settings.HalfResolution != 0 ? 2.0 : 1.0;
	const float2 pixel = (float2(GetTileOrigin(Tiles[1u + instance])) + corner * float(SsrTileSize)) * scale;
	CompositeVaryings output;
	output.Position = float4(GetNdc(pixel), 0.0, 1.0);
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

// The traced reflection at the scene pixel `pixel` from the half-resolution SsrColor: the four bilateral taps of SampleGtao
// around it (GtaoFullPixel and GtaoEdgeWeight over ViewDepth's linear depth and SceneNormals), each tap's radiance weighed by
// its confidence too; rgb the radiance, a the confidence. Zero when no tap represents the pixel's surface.
float4 SampleHalfResolutionTrace(uint2 pixel)
{
	const uint2 fullSize = Settings.ImageSize;
	const uint2 traceSize = GetTraceSize();
	const float depth = ViewDepth.Load(int3(pixel, 0));
	const float3 normal = DecodeGtaoNormal(SceneNormals.Load(int3(pixel, 0)));
	const float2 coordinate = (float2(pixel) + 0.5) * float2(traceSize) / float2(fullSize) - 0.5;
	const int2 first = int2(floor(coordinate));
	const float2 fraction = frac(coordinate);
	float3 radiance = float3(0.0);
	float confidence = 0.0;
	float weightSum = 0.0;
	for (int y = 0; y < 2; ++y)
	{
		for (int x = 0; x < 2; ++x)
		{
			const uint2 tap = uint2(clamp(first + int2(x, y), int2(0), int2(traceSize) - 1));
			const uint2 fullTap = GtaoFullPixel(tap, fullSize, traceSize);
			const float weight = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y)
				* GtaoEdgeWeight(depth, ViewDepth.Load(int3(fullTap, 0)), normal, DecodeGtaoNormal(SceneNormals.Load(int3(fullTap, 0))));
			const float4 traced = SsrColor.Load(int3(tap, 0));
			radiance += traced.rgb * (weight * traced.a);
			confidence += weight * traced.a;
			weightSum += weight;
		}
	}
	if (!(weightSum > 1e-6) || !(confidence > 0.0))
		return float4(0.0);
	return float4(radiance / confidence, confidence / weightSum);
}

[shader("fragment")]
float4 PSComposite(CompositeVaryings input) : SV_Target0
{
	const uint2 pixel = uint2(input.Position.xy);
	float4 traced;
	if (Settings.HalfResolution != 0)
		traced = IsTracedPixel(pixel) ? SampleHalfResolutionTrace(pixel) : float4(0.0);
	else
		traced = SsrColor.Load(int3(pixel, 0));
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
	const float3 added = (traced.rgb - prefiltered) * factor;
	if (Fog.Enabled != 0)
		return float4(added * (1.0 - EvaluateFog(worldPosition).a), 0.0);
	return float4(added, 0.0);
}
