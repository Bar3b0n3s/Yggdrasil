// Sky: M45's procedural sky (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Renderer/SkyLighting.h,
// Renderer/SkyPass.h), Hillaire's LUT method (Common/Atmosphere.slang) from the view's SkyConstants (Shared/SkyConstants.h):
//   - CSTransmittanceLut, CSMultipleScatteringLut: the atmosphere's LUTs (SkyLuts), rebuilt when Haze, Overcast or
//     GroundAlbedo change;
//   - CSSkyViewLut: the sky's radiance (before Intensity) seen from SkyDispatchConstants::Altitude: every frame at the
//     camera's altitude, and at sea level with each lighting update;
//   - CSSkyCube: the image-based light's cube, level 0, from the sea-level LUT and the clouds at their stepped offset (no
//     discs, no stars), the ground below the horizon;
//   - CSSkyFinalize: the reduced radiance SH9 (EnvironmentBake's CSProjectIrradiance and CSReduceIrradiance) to the
//     irradiance SH9 times Intensity and the fog's tinted colour, into SkyResults;
//   - CSProbeRelight: the relit irradiance volumes and the reflection probes' relit intensities (ProbeRelight);
//   - VSMain, PSMain: the Sky pass, a fullscreen triangle at depth 0 as the Skybox's.
// Every value is radiance in §8.5's artist units: the lights' authored Color times Intensity scatter.
//
// The test entries CSTestCopyWrite and CSTestCopyRead are the GPU proof of the copy into a view's constant buffers ("SkyLighting:
// a buffer copied into a view's constant buffers after their CPU writes reaches the passes that read them"). No pass creates a
// pipeline for them; their layouts live in Tests/Source/Support/SkyCopyProgram.h.

#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/SkyConstants.h"
#include "Shared/ViewConstants.h"

ConstantBuffer<ViewConstants> View : register(b0, space0);
ConstantBuffer<SkyConstants> Sky : register(b1, space0);
ConstantBuffer<EnvironmentConstants> Environment : register(b2, space0);
ConstantBuffer<ProbeConstants> Probes : register(b4, space0);
ConstantBuffer<FogConstants> Fog : register(b5, space0);
[[vk::push_constant]] ConstantBuffer<SkyDispatchConstants> Dispatch;
Texture2D<float4> TransmittanceLut : register(t0, space0);
Texture2D<float4> MultipleScatteringLut : register(t1, space0);
Texture2D<float4> SkyViewLut : register(t2, space0);
StructuredBuffer<float4> RadianceTotals : register(t3, space0); // CSSkyFinalize, CSProbeRelight: CSReduceIrradiance's ten totals
StructuredBuffer<float4> RelightProbes : register(t4, space0); // CSProbeRelight's volume mode: the volume's relight block
StructuredBuffer<float4> RelightReflections : register(t5, space0); // CSProbeRelight's reflection mode: the view's entries
SamplerState LinearClamp : register(s0, space0);
SamplerState LinearWrapU : register(s1, space0); // the sky-view LUT: wrapping in azimuth, clamped in elevation
[vk::image_format("rgba16f")]
RWTexture2D<float4> LutDestination : register(u0, space0);
[vk::image_format("rgba16f")]
RWTexture2DArray<float4> CubeDestination : register(u1, space0);
RWStructuredBuffer<float4> Results : register(u2, space0);
RWStructuredBuffer<float4> IrradianceProbes : register(u3, space0); // CSProbeRelight: the view's copy of its volumes' probes
RWStructuredBuffer<float4> TestResults : register(u6, space0);
RWStructuredBuffer<float4> TestReadback : register(u7, space0);

#include "Common/Atmosphere.slang"
#include "Common/Fog.slang"

static const float SkyMaxHalf = 65504.0;
// The SH9 projection's terms (EnvironmentBake's ShTerms): the nine coefficients, then the solid angle.
static const uint SkyShTerms = 10;
// EnvironmentData::IrradianceShWindow, the Hanning window's width in bands.
static const float SkyIrradianceShWindow = 4.0;
static const float SkyShConstantTerm = 0.282094791773878;

float3 ClampHalf(float3 value)
{
	return min(max(value, float3(0.0, 0.0, 0.0)), float3(SkyMaxHalf, SkyMaxHalf, SkyMaxHalf));
}

// The transmittance LUT: texel (x, y) at its Bruneton coordinates.
[shader("compute")]
[numthreads(8, 8, 1)]
void CSTransmittanceLut(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= TransmittanceLutSize))
		return;
	const float2 unit = float2(id.xy) / float2(TransmittanceLutSize - 1);
	float r;
	float mu;
	GetTransmittanceLutRadiusMu(unit, r, mu);
	LutDestination[id.xy] = float4(ComputeTransmittanceToTop(r, mu), 1.0);
}

// The multiple-scattering LUT (Hillaire's Psi_ms): for a sun at cosine x of the zenith and a height y, the second-order
// isotropic scattering L2 of a unit illuminance gathered over 64 directions (the ground's Lambertian bounce included) and
// the transfer f_ms of a uniform isotropic radiance, giving L2 / (1 - f_ms).
[shader("compute")]
[numthreads(8, 8, 1)]
void CSMultipleScatteringLut(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= uint2(MultipleScatteringLutSize, MultipleScatteringLutSize)))
		return;
	const float2 unit = float2(id.xy) / float(MultipleScatteringLutSize - 1);
	const float cosSun = unit.x * 2.0 - 1.0;
	const float r = Sky.BottomRadius + clamp(unit.y, 0.0005, 0.9995) * (Sky.TopRadius - Sky.BottomRadius);
	const float3 sun = float3(sqrt(saturate(1.0 - cosSun * cosSun)), cosSun, 0.0);
	const float3 origin = float3(0.0, r, 0.0);
	float3 secondOrder = float3(0.0, 0.0, 0.0);
	float3 transfer = float3(0.0, 0.0, 0.0);
	const uint directions = AtmosphereMultipleScatteringDirections;
	for (uint i = 0; i < directions; ++i)
	{
		for (uint j = 0; j < directions; ++j)
		{
			// Uniform over the sphere: cos(theta) in equal steps, phi in equal steps.
			const float cosTheta = 1.0 - 2.0 * (float(i) + 0.5) / float(directions);
			const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
			const float phi = 2.0 * AtmospherePi * (float(j) + 0.5) / float(directions);
			const float3 direction = float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
			const bool ground = RayMeetsGround(r, cosTheta);
			const float distance = ground ? DistanceToGround(r, cosTheta) : DistanceToTop(r, cosTheta);
			const float step = distance / float(AtmosphereMultipleScatteringSteps);
			float3 throughput = float3(1.0, 1.0, 1.0);
			for (uint k = 0; k < AtmosphereMultipleScatteringSteps; ++k)
			{
				const float3 position = origin + direction * ((float(k) + 0.5) * step);
				const float radius = length(position);
				const float height = radius - Sky.BottomRadius;
				const float3 scattering = GetRayleighScattering(height) + GetMieScattering(height);
				const float3 extinction = GetAtmosphereExtinction(height);
				const float3 stepTransmittance = exp(-extinction * step);
				const float3 sunTransmittance = SampleTransmittance(TransmittanceLut, LinearClamp, radius, dot(position / radius, sun));
				const float3 inScattered = scattering * sunTransmittance / (4.0 * AtmospherePi);
				const float3 safeExtinction = max(extinction, float3(1e-6, 1e-6, 1e-6));
				secondOrder += throughput * (inScattered - inScattered * stepTransmittance) / safeExtinction;
				transfer += throughput * (scattering - scattering * stepTransmittance) / safeExtinction;
				throughput *= stepTransmittance;
			}
			if (ground)
			{
				const float3 position = origin + direction * distance;
				const float3 normal = normalize(position);
				const float3 sunTransmittance = SampleTransmittance(TransmittanceLut, LinearClamp, length(position), dot(normal, sun));
				secondOrder += throughput * sunTransmittance * saturate(dot(normal, sun)) * Sky.GroundAlbedo / AtmospherePi;
			}
		}
	}
	const float count = float(directions * directions);
	secondOrder /= count;
	transfer /= count;
	LutDestination[id.xy] = float4(secondOrder / max(float3(1.0, 1.0, 1.0) - transfer, float3(1e-3, 1e-3, 1e-3)), 1.0);
}

// The light one source scatters at `position` (planet-centred, km) toward the viewer along `direction`: single scattering
// through the transmittance LUT, and the multiple-scattering LUT's Psi_ms, per unit of the source's illuminance.
float3 GetScatteredLight(float3 position, float3 direction, float3 toLight, float3 rayleigh, float mie)
{
	const float radius = length(position);
	const float cosLight = dot(position / radius, toLight);
	const float3 transmittance = SampleTransmittance(TransmittanceLut, LinearClamp, radius, cosLight);
	const float cosine = dot(direction, toLight);
	const float3 single = transmittance * (rayleigh * GetRayleighPhase(cosine) + mie * GetMiePhase(cosine, Sky.MieAsymmetry));
	const float3 multiple = MultipleScatteringLut.SampleLevel(LinearClamp, GetMultipleScatteringLutUv(radius, cosLight), 0.0).rgb;
	return single + (rayleigh + mie) * multiple;
}

// The sky's radiance along `direction` from radius `r` (km) on the +Y axis, from the Sun and the Moon light, to the top of
// the atmosphere or the ground, in AtmosphereScatteringSteps steps (Hillaire's energy-conserving step integration).
float3 IntegrateSkyRadiance(float r, float3 direction)
{
	const float3 origin = float3(0.0, r, 0.0);
	const bool ground = RayMeetsGround(r, direction.y);
	const float distance = ground ? DistanceToGround(r, direction.y) : DistanceToTop(r, direction.y);
	const float step = distance / float(AtmosphereScatteringSteps);
	const float3 moonLight = Sky.HasMoon != 0 ? Sky.MoonIlluminance * Sky.MoonFraction : float3(0.0, 0.0, 0.0);
	float3 radiance = float3(0.0, 0.0, 0.0);
	float3 throughput = float3(1.0, 1.0, 1.0);
	for (uint index = 0; index < AtmosphereScatteringSteps; ++index)
	{
		const float3 position = origin + direction * ((float(index) + 0.5) * step);
		const float height = length(position) - Sky.BottomRadius;
		const float3 rayleigh = GetRayleighScattering(height);
		const float mie = GetMieScattering(height);
		const float3 extinction = GetAtmosphereExtinction(height);
		const float3 stepTransmittance = exp(-extinction * step);
		float3 scattered = Sky.SunIlluminance * GetScatteredLight(position, direction, Sky.SunDirection, rayleigh, mie);
		if (any(moonLight > 0.0))
			scattered += moonLight * GetScatteredLight(position, direction, Sky.MoonDirection, rayleigh, mie);
		radiance += throughput * (scattered - scattered * stepTransmittance) / max(extinction, float3(1e-6, 1e-6, 1e-6));
		throughput *= stepTransmittance;
	}
	return radiance;
}

// The sky-view LUT at Dispatch.Altitude (km): radiance in the lights' units, before Intensity.
[shader("compute")]
[numthreads(8, 8, 1)]
void CSSkyViewLut(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= SkyViewLutSize))
		return;
	const float r = Sky.BottomRadius + max(Dispatch.Altitude, 0.0);
	const float2 uv = (float2(id.xy) + 0.5) / float2(SkyViewLutSize);
	LutDestination[id.xy] = float4(ClampHalf(IntegrateSkyRadiance(r, GetSkyViewLutDirection(uv, r))), 1.0);
}

// The sky-view LUT's radiance along `direction`, before Intensity.
float3 SampleSkyView(float3 direction, float r)
{
	return SkyViewLut.SampleLevel(LinearWrapU, GetSkyViewLutUv(direction, r), 0.0).rgb;
}

// A 32-bit integer hash (PCG's output permutation), the cloud noise's and the stars' seeded randomness.
uint HashSky(uint value)
{
	const uint state = value * 747796405u + 2891336453u;
	const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float HashSkyUnit(uint value)
{
	return float(HashSky(value) >> 8) / 16777216.0;
}

// The cloud layer's value noise: lattice values hashed on a SkyCloudNoisePeriod-periodic grid, smoothly interpolated.
static const uint SkyCloudNoisePeriod = 256;
static const float SkyCloudCellSize = 4000.0; // metres per lattice cell at the first octave

float GetCloudLattice(int2 cell)
{
	const uint2 wrapped = uint2(cell & int(SkyCloudNoisePeriod - 1));
	return HashSkyUnit(wrapped.x + wrapped.y * SkyCloudNoisePeriod + 0x5ca1ab1eu);
}

float GetCloudNoise(float2 position)
{
	const float2 cell = floor(position);
	const float2 fraction = position - cell;
	const float2 smooth = fraction * fraction * (3.0 - 2.0 * fraction);
	const int2 base = int2(cell);
	const float a = GetCloudLattice(base);
	const float b = GetCloudLattice(base + int2(1, 0));
	const float c = GetCloudLattice(base + int2(0, 1));
	const float d = GetCloudLattice(base + int2(1, 1));
	return lerp(lerp(a, b, smooth.x), lerp(c, d, smooth.x), smooth.y);
}

// Five octaves, each twice as fine (a power-of-two multiple of the period, so the layer keeps its period).
float GetCloudDensity(float2 position)
{
	float value = 0.0;
	float amplitude = 0.5;
	float2 point = position;
	for (uint octave = 0; octave < 5; ++octave)
	{
		value += amplitude * GetCloudNoise(point);
		point *= 2.0;
		amplitude *= 0.5;
	}
	return value / 0.96875;
}

// The cloud layer seen along `direction` from `eye` (world metres) at `offset`: its colour (rgb, premultiplied) and coverage
// (a). Clouds is the coverage; the layer is lit by the Sun and the Moon light through the transmittance at its height and by
// the sky's ambient (the zenith's radiance), and fades toward the horizon.
float4 EvaluateClouds(float3 direction, float3 eye, float2 offset, float r)
{
	if (Sky.CloudCoverage <= 0.0 || direction.y <= 0.0 || eye.y >= Sky.CloudHeight)
		return float4(0.0, 0.0, 0.0, 0.0);
	const float distance = (Sky.CloudHeight - eye.y) / direction.y;
	const float2 position = (eye.xz + direction.xz * distance + offset) / SkyCloudCellSize;
	const float density = GetCloudDensity(position);
	const float coverage = Sky.CloudCoverage;
	float alpha = smoothstep(1.0 - coverage, saturate(1.0 - coverage + 0.35), density) * smoothstep(0.0, 0.12, direction.y);
	alpha *= saturate(1.0 - distance / 60000.0);
	if (alpha <= 0.0)
		return float4(0.0, 0.0, 0.0, 0.0);
	const float cloudRadius = Sky.BottomRadius + Sky.CloudHeight / 1000.0;
	const float3 sunTransmittance = SampleTransmittance(TransmittanceLut, LinearClamp, cloudRadius, Sky.SunDirection.y);
	const float forward = GetMiePhase(dot(direction, Sky.SunDirection), 0.6) * 4.0 * AtmospherePi;
	float3 light = Sky.SunIlluminance * sunTransmittance * (0.35 + 0.65 * forward) / (4.0 * AtmospherePi);
	if (Sky.HasMoon != 0)
	{
		const float3 moonTransmittance = SampleTransmittance(TransmittanceLut, LinearClamp, cloudRadius, Sky.MoonDirection.y);
		light += Sky.MoonIlluminance * Sky.MoonFraction * moonTransmittance / (4.0 * AtmospherePi);
	}
	light += SampleSkyView(float3(0.0, 1.0, 0.0), r);
	const float3 color = light * 0.9 * lerp(1.0, 0.55, density);
	return float4(color * alpha, alpha);
}

// The stars along `direction`: cells of a cube grid in the stars' frame (Sky.StarFrame), about one in SkyStarDensity holding
// a star at a hashed place with a hashed brightness and tint, generated in code as the blue noise is; Stars times their
// brightness, faded as the sky's own luminance `skyLuminance` rises.
static const uint SkyStarCells = 512;
static const float SkyStarDensity = 0.06;

float3 EvaluateStars(float3 direction, float skyLuminance)
{
	if (Sky.Stars <= 0.0)
		return float3(0.0, 0.0, 0.0);
	const float3 celestial = float3(dot(Sky.StarFrame0.xyz, direction), dot(Sky.StarFrame1.xyz, direction), dot(Sky.StarFrame2.xyz, direction));
	const float3 magnitude = abs(celestial);
	uint face;
	float2 uv;
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
	{
		face = celestial.x > 0.0 ? 0u : 1u;
		uv = celestial.yz / magnitude.x;
	}
	else if (magnitude.y >= magnitude.z)
	{
		face = celestial.y > 0.0 ? 2u : 3u;
		uv = celestial.xz / magnitude.y;
	}
	else
	{
		face = celestial.z > 0.0 ? 4u : 5u;
		uv = celestial.xy / magnitude.z;
	}
	const float2 grid = (uv * 0.5 + 0.5) * float(SkyStarCells);
	const uint2 cell = min(uint2(grid), uint2(SkyStarCells - 1, SkyStarCells - 1));
	const uint seed = (face * SkyStarCells + cell.y) * SkyStarCells + cell.x;
	if (HashSkyUnit(seed) >= SkyStarDensity)
		return float3(0.0, 0.0, 0.0);
	const float2 centre = float2(cell) + 0.2 + 0.6 * float2(HashSkyUnit(seed ^ 0x9e3779b9u), HashSkyUnit(seed ^ 0x7f4a7c15u));
	const float distance = length(grid - centre);
	const float brightness = pow(HashSkyUnit(seed ^ 0x2545f491u), 6.0) * 2.0 + 0.02;
	const float tint = HashSkyUnit(seed ^ 0x68e31da4u);
	const float3 color = lerp(float3(1.0, 0.8, 0.6), float3(0.7, 0.8, 1.0), tint);
	const float fade = saturate(1.0 - skyLuminance * 20.0);
	return color * (brightness * Sky.Stars * 0.05 * exp(-distance * distance * 8.0) * fade);
}

// The sun's disc along `direction`: SunDiscRadiance times the Sun light's illuminance through the atmosphere, limb-darkened
// (I = 1 - 0.6 (1 - mu), mu across the disc), dimmed by Overcast.
float3 EvaluateSunDisc(float3 direction, float r)
{
	const float cosine = dot(direction, Sky.SunDirection);
	if (Sky.SunDiscRadiance <= 0.0 || cosine <= Sky.SunCosRadius)
		return float3(0.0, 0.0, 0.0);
	const float radius = acos(clamp(Sky.SunCosRadius, -1.0, 1.0));
	const float angle = acos(clamp(cosine, -1.0, 1.0));
	const float across = sqrt(saturate(1.0 - (angle * angle) / max(radius * radius, 1e-12)));
	const float limb = 1.0 - 0.6 * (1.0 - across);
	const float3 transmittance = SampleTransmittance(TransmittanceLut, LinearClamp, r, direction.y);
	return Sky.SunIlluminance * transmittance * (Sky.SunDiscRadiance * limb * (1.0 - Sky.Overcast));
}

// The moon along `direction`: a sphere lit by the sun (its phase from their elongation), its procedural maria from the cloud
// noise, the moon light's illuminance over the disc's solid angle, through the atmosphere, dimmed by Overcast.
float3 EvaluateMoon(float3 direction, float r)
{
	if (Sky.HasMoon == 0)
		return float3(0.0, 0.0, 0.0);
	const float cosine = dot(direction, Sky.MoonDirection);
	if (cosine <= Sky.MoonCosRadius)
		return float3(0.0, 0.0, 0.0);
	const float3 helper = abs(Sky.MoonDirection.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	const float3 right = normalize(cross(helper, Sky.MoonDirection));
	const float3 up = cross(Sky.MoonDirection, right);
	const float sinRadius = sqrt(saturate(1.0 - Sky.MoonCosRadius * Sky.MoonCosRadius));
	const float2 disc = float2(dot(direction, right), dot(direction, up)) / max(sinRadius, 1e-6);
	const float depth = sqrt(saturate(1.0 - dot(disc, disc)));
	const float3 normal = disc.x * right + disc.y * up - depth * Sky.MoonDirection;
	const float lit = saturate(dot(normal, Sky.SunDirection));
	const float maria = 0.6 + 0.4 * GetCloudDensity(disc * 3.0 + 17.0);
	const float solidAngle = 2.0 * AtmospherePi * (1.0 - Sky.MoonCosRadius);
	const float3 transmittance = SampleTransmittance(TransmittanceLut, LinearClamp, r, direction.y);
	return Sky.MoonIlluminance * transmittance * (lit * maria / max(solidAngle, 1e-8) * (1.0 - Sky.Overcast));
}

// The unit direction of texel (column, row) of `face` of a cube of `size` (EnvironmentBake's CubeTexelDirection: Vulkan's
// cube-map face selection, inverted).
float3 GetSkyCubeTexelDirection(uint face, uint column, uint row, uint size)
{
	const float u = 2.0 * (float(column) + 0.5) / float(size) - 1.0;
	const float v = 2.0 * (float(row) + 0.5) / float(size) - 1.0;
	float3 direction;
	switch (face)
	{
		case 0: direction = float3(1.0, -v, -u); break;
		case 1: direction = float3(-1.0, -v, u); break;
		case 2: direction = float3(u, 1.0, v); break;
		case 3: direction = float3(u, -1.0, -v); break;
		case 4: direction = float3(u, -v, 1.0); break;
		default: direction = float3(-u, -v, -1.0); break;
	}
	return normalize(direction);
}

// The atmosphere's ground, below the horizon: GroundAlbedo / pi times the light reaching it (the Sun's and the Moon light's
// through the transmittance at the ground, and the sky's zenith radiance, from the bound sky-view LUT at radius `r`, times
// pi), before Intensity. The image-based light's cube and the Sky pass add it to the sky-view LUT's in-scattering along a
// ray that meets the ground, so neither shows a black ground; seen without extinction on its way up (the camera stands low
// against the scale heights, and distance haze is the Fog's, decision 3).
float3 EvaluateSkyGround(float r)
{
	float3 irradiance = Sky.SunIlluminance * SampleTransmittance(TransmittanceLut, LinearClamp, Sky.BottomRadius, Sky.SunDirection.y)
		* saturate(Sky.SunDirection.y);
	if (Sky.HasMoon != 0)
		irradiance += Sky.MoonIlluminance * Sky.MoonFraction * SampleTransmittance(TransmittanceLut, LinearClamp, Sky.BottomRadius, Sky.MoonDirection.y)
			* saturate(Sky.MoonDirection.y);
	irradiance += SampleSkyView(float3(0.0, 1.0, 0.0), r) * AtmospherePi;
	return Sky.GroundAlbedo * irradiance / AtmospherePi;
}

// The image-based light's cube, level 0 (SkyCubeSize², six faces in z): the sea-level sky-view LUT and the clouds at
// Dispatch.CloudOffset above the horizon; below it, the atmosphere's ground: GroundAlbedo / pi times the light reaching it
// (the Sun and the Moon light through the transmittance, and the sky's zenith radiance times pi). No discs, no stars (the
// lights carry the discs; stars alias at this size). Before Intensity.
[shader("compute")]
[numthreads(8, 8, 1)]
void CSSkyCube(uint3 id : SV_DispatchThreadID)
{
	uint width;
	uint height;
	uint faces;
	CubeDestination.GetDimensions(width, height, faces);
	if (id.x >= width || id.y >= height || id.z >= 6)
		return;
	const float3 direction = GetSkyCubeTexelDirection(id.z, id.x, id.y, width);
	const float r = Sky.BottomRadius + max(Dispatch.Altitude, 0.0);
	float3 radiance = SampleSkyView(direction, r);
	if (direction.y < 0.0)
		radiance += EvaluateSkyGround(r);
	const float4 clouds = EvaluateClouds(direction, float3(0.0, max(Dispatch.Altitude, 0.0) * 1000.0, 0.0), Dispatch.CloudOffset, r);
	radiance = radiance * (1.0 - clouds.a) + clouds.rgb;
	CubeDestination[id] = float4(ClampHalf(radiance), 1.0);
}

// The irradiance SH9 of the sky's radiance totals (EnvironmentBaker's MakeIrradianceSH9 on the GPU: normalized by the solid
// angle, the clamped cosine's lobe per band, the Hanning window) times Intensity, and the fog's colour tinted by its constant
// term (MakeFogConstants' rule), into SkyResults. One thread.
[shader("compute")]
[numthreads(1, 1, 1)]
void CSSkyFinalize(uint3 id : SV_DispatchThreadID)
{
	if (id.x != 0)
		return;
	const float solidAngle = RadianceTotals[SkyShTerms - 1].x;
	const float normalization = solidAngle > 0.0 ? 4.0 * AtmospherePi / solidAngle : 0.0;
	const float lobes[3] = { 1.0, 2.0 / 3.0, 0.25 };
	float3 constantTerm = float3(0.0, 0.0, 0.0);
	for (uint index = 0; index < 9; ++index)
	{
		const uint band = index == 0 ? 0u : (index < 4 ? 1u : 2u);
		const float window = (1.0 + cos(AtmospherePi * float(band) / SkyIrradianceShWindow)) * 0.5;
		const float3 coefficient = RadianceTotals[index].xyz * (normalization * lobes[band] * window * Sky.Intensity);
		Results[SkyResultsIrradiance + index] = float4(coefficient, 0.0);
		if (index == 0)
			constantTerm = coefficient;
	}
	const float3 ambient = max(constantTerm * SkyShConstantTerm, float3(0.0, 0.0, 0.0));
	Results[SkyResultsFogColor] = float4(Sky.FogColor * lerp(float3(1.0, 1.0, 1.0), ambient, Sky.FogAmbientTint), 0.0);
}

// The largest scale a reflection probe inside a relit volume takes (Renderer/ProbeRelight.h's MaxReflectionRelightScale).
static const float SkyRelightMaxScale = 4.0;

// T_ij of the probe whose relight block starts at element `probe` of RelightProbes: its Transfer's upper triangle, row by row,
// four values a float4 after the nine local coefficients (Renderer/ProbeRelight.h's GetSkyTransferIndex).
float GetRelightTransfer(uint probe, uint i, uint j)
{
	const uint row = min(i, j);
	const uint column = max(i, j);
	const uint index = row * (19u - row) / 2u + (column - row);
	return RelightProbes[probe + 9u + index / 4u][index % 4u];
}

// The probes' relight (Renderer/ProbeRelight.h, decision 3), one thread per item:
//   - SkyRelightModeVolume: probe `item` of a relightable volume's Count probes gets Local + conv(T S) into the view's
//     IrradianceProbes copy from element Face, S the sky's radiance SH9 (its totals normalized as CSSkyFinalize normalizes
//     them, times Intensity), conv the clamped cosine's lobe and the Hanning window per band (RelightIrradianceSH9);
//   - SkyRelightModeReflections: reflection probe `item` of the view's Count gets its Intensity times the luminance ratio of
//     the relit to the baked constant term of the volume probe nearest its centre, clamped to [0, 4] (1 for a probe baked in
//     the dark, its Intensity alone outside every relit volume), into SkyResults (ComputeReflectionRelightScale), after the
//     volumes' relight wrote that term.
[shader("compute")]
[numthreads(64, 1, 1)]
void CSProbeRelight(uint3 id : SV_DispatchThreadID)
{
	const uint item = id.x;
	if (item >= Dispatch.Count)
		return;
	if (Dispatch.Mode == SkyRelightModeReflections)
	{
		const float4 entry = RelightReflections[item];
		float scale = entry.z;
		if (entry.x >= 0.0 && entry.y >= 1e-6)
		{
			const float relit = dot(IrradianceProbes[uint(entry.x)].xyz, float3(0.2126, 0.7152, 0.0722));
			scale *= clamp(relit / entry.y, 0.0, SkyRelightMaxScale);
		}
		Results[SkyResultsReflectionScales + item] = float4(scale, 0.0, 0.0, 0.0);
		return;
	}
	const float solidAngle = RadianceTotals[SkyShTerms - 1].x;
	const float normalization = solidAngle > 0.0 ? 4.0 * AtmospherePi / solidAngle * Sky.Intensity : 0.0;
	float3 sky[9];
	for (uint j = 0; j < 9; ++j)
		sky[j] = RadianceTotals[j].xyz * normalization;
	const float lobes[3] = { 1.0, 2.0 / 3.0, 0.25 };
	const uint probe = item * SkyRelightProbeVectors;
	for (uint i = 0; i < 9; ++i)
	{
		float3 seen = float3(0.0, 0.0, 0.0);
		for (uint j = 0; j < 9; ++j)
			seen += sky[j] * GetRelightTransfer(probe, i, j);
		const uint band = i == 0 ? 0u : (i < 4 ? 1u : 2u);
		const float window = (1.0 + cos(AtmospherePi * float(band) / SkyIrradianceShWindow)) * 0.5;
		IrradianceProbes[Dispatch.Face + item * 9u + i] = float4(RelightProbes[probe + i].xyz + seen * (lobes[band] * window), 0.0);
	}
}

struct SkyVaryings
{
	float4 Position : SV_Position;
	float2 Ndc : TEXCOORD0;
};

// The world-space point at normalized device coordinates `ndc` and reverse-Z depth `depth`.
float3 UnprojectSky(float2 ndc, float depth)
{
	const float4 position = mul(View.InverseViewProjection, float4(ndc, depth, 1.0));
	return position.xyz / position.w;
}

// The triangle (-1, -1), (3, -1), (-1, 3) covers the viewport at depth 0, the far plane (reverse-Z, §8.3), as the Skybox's.
[shader("vertex")]
SkyVaryings VSMain(uint vertexID : SV_VulkanVertexID)
{
	const float2 ndc = float2(vertexID == 1 ? 3.0 : -1.0, vertexID == 2 ? 3.0 : -1.0);
	SkyVaryings output;
	output.Position = float4(ndc, 0.0, 1.0);
	output.Ndc = ndc;
	return output;
}

// Each pixel at the cleared depth: the camera's sky-view LUT, the clouds at their live offset, the stars, the moon and the
// sun's disc above the horizon (none in an orthographic view; no star behind the moon) and the atmosphere's ground below it,
// times Intensity, fogged with FogSkybox as the skybox is (Common/Fog.slang; a mirror view from its glass on).
[shader("fragment")]
float4 PSMain(SkyVaryings input) : SV_Target0
{
	float3 ray;
	if (View.ProjectionKind == ProjectionKindOrthographic)
		ray = UnprojectSky(float2(0.0), 0.0) - UnprojectSky(float2(0.0), 1.0);
	else
		ray = UnprojectSky(input.Ndc, 0.5) - UnprojectSky(input.Ndc, 1.0);
	const float3 direction = normalize(ray);
	const float r = Sky.BottomRadius + Sky.CameraAltitude;
	float3 radiance = SampleSkyView(direction, r);
	const float skyLuminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
	if (direction.y > 0.0)
	{
		// An orthographic view's one direction would magnify a disc or a star over the whole background, so it draws neither;
		// the moon hides the stars behind its dark limb.
		if (View.ProjectionKind != ProjectionKindOrthographic)
		{
			const bool behindMoon = Sky.HasMoon != 0 && dot(direction, Sky.MoonDirection) > Sky.MoonCosRadius;
			if (!behindMoon)
				radiance += EvaluateStars(direction, skyLuminance);
			radiance += EvaluateMoon(direction, r) + EvaluateSunDisc(direction, r);
		}
	}
	else
		radiance += EvaluateSkyGround(r);
	const float4 clouds = EvaluateClouds(direction, Fog.CameraPosition, Sky.CloudOffset, r);
	radiance = (radiance * (1.0 - clouds.a) + clouds.rgb) * Sky.Intensity;
	radiance = min(radiance, float3(SkyMaxHalf, SkyMaxHalf, SkyMaxHalf));
	if (Fog.Enabled != 0 && Fog.FogSkybox != 0)
	{
		float3 eye = Fog.CameraPosition;
		if (Fog.Orthographic != 0)
		{
			const float3 nearPoint = UnprojectSky(input.Ndc, 1.0);
			eye = nearPoint - Fog.ViewForward * dot(nearPoint - Fog.CameraPosition, Fog.ViewForward);
		}
		if (View.ReflectionView != 0)
			radiance = ApplyFog(radiance, EvaluateSkyFogFrom(direction, eye, GetMirrorSkyFogStart(direction, eye, View.ClipPlane)));
		else
			radiance = ApplyFog(radiance, EvaluateSkyFog(direction, eye));
	}
	return float4(radiance, 1.0);
}

// The copy's GPU proof (W45.1).

// Element i of SkyResults, component c, is Sky.Intensity + 4 i + c: distinct values that follow the frame's constants.
[shader("compute")]
[numthreads(32, 1, 1)]
void CSTestCopyWrite(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	const uint index = dispatchThreadID.x;
	if (index >= SkyResultsCount)
		return;
	const float base = Sky.Intensity + 4.0 * float(index);
	TestResults[index] = float4(base, base + 1.0, base + 2.0, base + 3.0);
}

// The constant buffers as the passes see them: EnvironmentConstants' members in order (the nine SH9, then AmbientColor with
// Intensity, the rotation with the lods, and the flag's bits), FogConstants' Color with Density and CameraPosition with
// StartDistance, then each reflection probe's AxisScale and Lookup.
[shader("compute")]
[numthreads(1, 1, 1)]
void CSTestCopyRead(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x != 0)
		return;
	for (uint index = 0; index < 9; ++index)
		TestReadback[index] = Environment.IrradianceSH9[index];
	TestReadback[9] = float4(Environment.AmbientColor, Environment.Intensity);
	TestReadback[10] = float4(Environment.RotationSin, Environment.RotationCos, Environment.SpecularMaxLod, Environment.SkyboxLod);
	TestReadback[11] = float4(asfloat(Environment.HasEnvironment), asfloat(Environment.Padding0), asfloat(Environment.Padding1), asfloat(Environment.Padding2));
	TestReadback[12] = float4(Fog.Color, Fog.Density);
	TestReadback[13] = float4(Fog.CameraPosition, Fog.StartDistance);
	for (uint probe = 0; probe < MaxViewReflectionProbes; ++probe)
	{
		TestReadback[14 + 2 * probe] = Probes.Reflections[probe].AxisScale;
		TestReadback[15 + 2 * probe] = Probes.Reflections[probe].Lookup;
	}
}
