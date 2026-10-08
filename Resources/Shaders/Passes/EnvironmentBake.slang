// EnvironmentBake: the GPU environment bake of §8.6 (Renderer/EnvironmentBaker.h): the equirectangular source to the skybox
// cube, the cube's mip chain, the GGX specular prefilter and the SH9 projection and reduction. Every cube is RGBA16F; a
// level being written is bound as a six-layer array (Destination), the level it reads from as another (SourceLevel). The
// conventions (equirectangular mapping, cube faces and texel directions, SH basis) are Asset/EnvironmentData.h's; the
// per-dispatch values are push constants (Shared/EnvironmentBakeConstants.h). Deterministic for a device: fixed sample
// sequences and a fixed reduction order.

#include "Shared/EnvironmentBakeConstants.h"

[[vk::push_constant]] ConstantBuffer<EnvironmentBakeConstants> Bake;

Texture2D<float4> Equirect : register(t0, space0);            // CSEquirectToCube: the source, RGBA32F, rows top first
Texture2DArray<float4> SourceLevel : register(t1, space0);    // one level of a cube, faces as layers
TextureCube<float4> SourceCube : register(t2, space0);        // CSPrefilterSpecular: the skybox with its mip chain
StructuredBuffer<float4> PartialSums : register(t3, space0);  // CSReduceIrradiance: CSProjectIrradiance's output
SamplerState LinearClamp : register(s0, space0);
[vk::image_format("rgba16f")]
RWTexture2DArray<float4> Destination : register(u0, space0);  // the level being written, faces as layers
RWStructuredBuffer<float4> Partials : register(u1, space0);   // CSProjectIrradiance: ShTerms values per thread group
RWStructuredBuffer<float4> Irradiance : register(u2, space0); // CSReduceIrradiance: the ShTerms totals

static const float Pi = 3.14159265358979;
// The SH9 projection's terms per texel or group: the nine coefficients (RGB in xyz), then the solid angle (x).
static const uint ShTerms = 10;
static const uint GroupSize = 8;
static const uint GroupThreads = GroupSize * GroupSize;

// The unit direction of texel (column, row) of `face` of a cube of `size` (Vulkan's cube-map face selection, inverted: face
// order +X, -X, +Y, -Y, +Z, -Z, (s, t) = ((column + 0.5) / size, (row + 0.5) / size)).
float3 CubeTexelDirection(uint face, uint column, uint row, uint size)
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

// The integral of the solid angle over the face square from (0, 0) to (x, y), in face coordinates [-1, 1] x [-1, 1].
float AreaElement(float x, float y)
{
	return atan2(x * y, sqrt(x * x + y * y + 1.0));
}

// The exact solid angle of texel (column, row) of a face of `size`.
float CubeTexelSolidAngle(uint column, uint row, uint size)
{
	const float scale = 2.0 / float(size);
	const float x0 = float(column) * scale - 1.0;
	const float x1 = float(column + 1) * scale - 1.0;
	const float y0 = float(row) * scale - 1.0;
	const float y1 = float(row + 1) * scale - 1.0;
	return AreaElement(x0, y0) - AreaElement(x0, y1) - AreaElement(x1, y0) + AreaElement(x1, y1);
}

// The radiance of the equirectangular source along the unit `direction`: (u, v) = (atan2(x, -z) / 2 pi + 0.5, acos(y) / pi),
// bilinear over the texels, wrapping in u and clamped in v.
float3 SampleEquirect(float3 direction)
{
	const uint width = Bake.SourceWidth;
	const uint height = width / 2;
	const float u = atan2(direction.x, -direction.z) / (2.0 * Pi) + 0.5;
	const float v = acos(clamp(direction.y, -1.0, 1.0)) / Pi;
	const float x = u * float(width) - 0.5;
	const float y = v * float(height) - 0.5;
	const float x0 = floor(x);
	const float y0 = floor(y);
	const float fx = x - x0;
	const float fy = y - y0;
	const int iw = int(width);
	const int column0 = ((int(x0) % iw) + iw) % iw;
	const int column1 = (column0 + 1) % iw;
	const int row0 = clamp(int(y0), 0, int(height) - 1);
	const int row1 = clamp(int(y0) + 1, 0, int(height) - 1);
	const float3 top = lerp(Equirect.Load(int3(column0, row0, 0)).rgb, Equirect.Load(int3(column1, row0, 0)).rgb, fx);
	const float3 bottom = lerp(Equirect.Load(int3(column0, row1, 0)).rgb, Equirect.Load(int3(column1, row1, 0)).rgb, fx);
	return lerp(top, bottom, fy);
}

float4 ToStoredTexel(float3 radiance)
{
	return float4(min(max(radiance, float3(0.0)), float3(EnvironmentBakeMaxHalf)), 1.0);
}

// Step 2: the skybox's level 0 from the equirectangular source, one bilinear sample at each texel's direction.
[shader("compute")]
[numthreads(8, 8, 1)]
void CSEquirectToCube(uint3 threadID : SV_DispatchThreadID)
{
	if (threadID.x >= Bake.FaceSize || threadID.y >= Bake.FaceSize)
		return;
	const float3 direction = CubeTexelDirection(threadID.z, threadID.x, threadID.y, Bake.FaceSize);
	Destination[threadID] = ToStoredTexel(SampleEquirect(direction));
}

// Step 2: one level of the skybox's mip chain, the 2x2 box average of the level above it.
[shader("compute")]
[numthreads(8, 8, 1)]
void CSDownsampleCube(uint3 threadID : SV_DispatchThreadID)
{
	if (threadID.x >= Bake.FaceSize || threadID.y >= Bake.FaceSize)
		return;
	const int2 source = int2(threadID.xy) * 2;
	const int last = int(Bake.SourceFaceSize) - 1;
	const int face = int(threadID.z);
	const float3 sum = SourceLevel.Load(int4(source.x, source.y, face, 0)).rgb + SourceLevel.Load(int4(min(source.x + 1, last), source.y, face, 0)).rgb
		+ SourceLevel.Load(int4(source.x, min(source.y + 1, last), face, 0)).rgb
		+ SourceLevel.Load(int4(min(source.x + 1, last), min(source.y + 1, last), face, 0)).rgb;
	Destination[threadID] = ToStoredTexel(sum * 0.25);
}

// The i-th point of the n-point Hammersley sequence: (i / n, the radical inverse of i in base 2).
float2 Hammersley(uint index, uint count)
{
	return float2(float(index) / float(count), float(reversebits(index)) * 2.3283064365386963e-10);
}

// The GGX normal distribution D(h) for `alpha` (perceptual roughness squared).
float DistributionGgx(float nDotH, float alpha)
{
	const float alpha2 = alpha * alpha;
	const float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
	return alpha2 / (Pi * denominator * denominator);
}

// A GGX-distributed half vector around the unit `normal` for the uniform point `xi` (Karis 2013).
float3 ImportanceSampleGgx(float2 xi, float alpha, float3 normal)
{
	const float phi = 2.0 * Pi * xi.x;
	const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
	const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
	const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	const float3 tangentX = normalize(cross(up, normal));
	const float3 tangentY = cross(normal, tangentX);
	return normalize(tangentX * (sinTheta * cos(phi)) + tangentY * (sinTheta * sin(phi)) + normal * cosTheta);
}

// Step 3: one mip of the specular cube. SampleCount 0 (mip 0, roughness 0) resamples SourceLevel bilinearly at each texel's
// face position, clamped at the face's edges (a copy for equal sizes, the 2x2 average for a source of twice the size).
// Otherwise GGX importance sampling with N = V = R over the Hammersley sequence, weighted by NdotL, each sample reading the
// skybox at the mip its PDF and solid angle choose (filtered importance sampling, Krivanek and Colbert 2008:
// lod = log2(OmegaS / OmegaP) / 2 + 1, with OmegaS the sample's solid angle and OmegaP that of a level-0 texel).
[shader("compute")]
[numthreads(8, 8, 1)]
void CSPrefilterSpecular(uint3 threadID : SV_DispatchThreadID)
{
	if (threadID.x >= Bake.FaceSize || threadID.y >= Bake.FaceSize)
		return;
	if (Bake.SampleCount == 0)
	{
		const float scale = float(Bake.SourceFaceSize) / float(Bake.FaceSize);
		const float x = (float(threadID.x) + 0.5) * scale - 0.5;
		const float y = (float(threadID.y) + 0.5) * scale - 0.5;
		const float x0 = floor(x);
		const float y0 = floor(y);
		const float fx = x - x0;
		const float fy = y - y0;
		const int last = int(Bake.SourceFaceSize) - 1;
		const int column0 = clamp(int(x0), 0, last);
		const int column1 = clamp(int(x0) + 1, 0, last);
		const int row0 = clamp(int(y0), 0, last);
		const int row1 = clamp(int(y0) + 1, 0, last);
		const int face = int(threadID.z);
		const float3 top = lerp(SourceLevel.Load(int4(column0, row0, face, 0)).rgb, SourceLevel.Load(int4(column1, row0, face, 0)).rgb, fx);
		const float3 bottom = lerp(SourceLevel.Load(int4(column0, row1, face, 0)).rgb, SourceLevel.Load(int4(column1, row1, face, 0)).rgb, fx);
		Destination[threadID] = ToStoredTexel(lerp(top, bottom, fy));
		return;
	}

	const float3 normal = CubeTexelDirection(threadID.z, threadID.x, threadID.y, Bake.FaceSize);
	float3 sum = float3(0.0);
	float weight = 0.0;
	for (uint index = 0; index < Bake.SampleCount; ++index)
	{
		const float3 halfVector = ImportanceSampleGgx(Hammersley(index, Bake.SampleCount), Bake.Alpha, normal);
		const float vDotH = dot(normal, halfVector);
		const float3 light = 2.0 * vDotH * halfVector - normal;
		const float nDotL = dot(normal, light);
		if (nDotL <= 0.0)
			continue;
		// pdf(l) = D(h) NdotH / (4 VdotH) = D(h) / 4 with N = V.
		const float pdf = DistributionGgx(saturate(vDotH), Bake.Alpha) * 0.25;
		const float sampleSolidAngle = 1.0 / max(float(Bake.SampleCount) * pdf, 1e-20);
		const float lod = clamp(0.5 * log2(sampleSolidAngle / Bake.SourceTexelSolidAngle) + 1.0, 0.0, Bake.SourceMaxLod);
		sum += SourceCube.SampleLevel(LinearClamp, light, lod).rgb * nDotL;
		weight += nDotL;
	}
	Destination[threadID] = ToStoredTexel(weight > 0.0 ? sum / weight : float3(0.0));
}

groupshared float4 ShShared[GroupThreads][ShTerms];

// Step 4: the SH9 projection of one 8x8 tile of a face of the level SourceLevel holds (FaceSize): each texel's radiance times
// the real SH basis (EnvironmentData.h) at its direction times its solid angle, and the solid angle, summed over the tile in
// a fixed tree order into ShTerms values of Partials per thread group (groups numbered x, then y, then face).
[shader("compute")]
[numthreads(8, 8, 1)]
void CSProjectIrradiance(uint3 threadID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
	float4 terms[ShTerms];
	for (uint term = 0; term < ShTerms; ++term)
		terms[term] = float4(0.0);
	if (threadID.x < Bake.FaceSize && threadID.y < Bake.FaceSize)
	{
		const float3 d = CubeTexelDirection(threadID.z, threadID.x, threadID.y, Bake.FaceSize);
		const float solidAngle = CubeTexelSolidAngle(threadID.x, threadID.y, Bake.FaceSize);
		const float3 radiance = SourceLevel.Load(int4(int2(threadID.xy), int(threadID.z), 0)).rgb * solidAngle;
		terms[0] = float4(radiance * 0.282095, 0.0);
		terms[1] = float4(radiance * (0.488603 * d.y), 0.0);
		terms[2] = float4(radiance * (0.488603 * d.z), 0.0);
		terms[3] = float4(radiance * (0.488603 * d.x), 0.0);
		terms[4] = float4(radiance * (1.092548 * d.x * d.y), 0.0);
		terms[5] = float4(radiance * (1.092548 * d.y * d.z), 0.0);
		terms[6] = float4(radiance * (0.315392 * (3.0 * d.z * d.z - 1.0)), 0.0);
		terms[7] = float4(radiance * (1.092548 * d.x * d.z), 0.0);
		terms[8] = float4(radiance * (0.546274 * (d.x * d.x - d.y * d.y)), 0.0);
		terms[9] = float4(solidAngle, 0.0, 0.0, 0.0);
	}
	for (uint term = 0; term < ShTerms; ++term)
		ShShared[groupIndex][term] = terms[term];
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = GroupThreads / 2; stride > 0; stride /= 2)
	{
		if (groupIndex < stride)
		{
			for (uint term = 0; term < ShTerms; ++term)
				ShShared[groupIndex][term] += ShShared[groupIndex + stride][term];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (groupIndex < ShTerms)
	{
		const uint groupsPerSide = (Bake.FaceSize + GroupSize - 1) / GroupSize;
		const uint group = groupID.x + (groupID.y + groupID.z * groupsPerSide) * groupsPerSide;
		Partials[group * ShTerms + groupIndex] = ShShared[0][groupIndex];
	}
}

// Step 4: the totals of CSProjectIrradiance's GroupCount partial sums, each term summed by one thread in group order; the
// cosine convolution, the division by pi, the normalization of the solid angles and the Hanning window are applied by the
// baker on the CPU (EnvironmentBaker.cpp).
[shader("compute")]
[numthreads(16, 1, 1)]
void CSReduceIrradiance(uint3 threadID : SV_DispatchThreadID)
{
	const uint term = threadID.x;
	if (term >= ShTerms)
		return;
	float4 total = float4(0.0);
	for (uint group = 0; group < Bake.GroupCount; ++group)
		total += PartialSums[group * ShTerms + term];
	Irradiance[term] = total;
}
