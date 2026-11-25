// The water's waves on the GPU (M47, Docs/Decisions/0065-m47-water.md decision 3 and the W47.2 record): the mirror of
// Physics/WaterWaves.h's Gerstner sum, which the CPU evaluates in double for the queries, the physics and the scripts. Passes/
// Water.slang includes it after Shared/WaterConstants.h. The CPU finds the surface particle under a point by inverting the
// sum; the GPU only displaces each clipmap vertex's rest point forward, so the drawn surface is the CPU's up to the grid's
// linear interpolation and float rounding ("Water: the GPU surface equals the CPU surface within 1 mm").
//
// A rest point x0 is an offset from the record's Origin (Shared/WaterConstants.h), exact in float for the clipmap's
// vertices. Wave i has the phase th = WavePhases[i].y + k dot(D, x0) (WavePhases[i].z in the previous frame), the CPU's
// k dot(D, x0 - water origin) - w t + p, since the CPU computed the phase at Origin in double. The particle moves to
//   x0 + sum_i f_i Q_i A_i D_i cos(th_i) horizontally and BaseHeight + sum_i f_i A_i sin(th_i),
// where f_i fades wave i with the Chebyshev distance d of x0 from the camera (GetWaterWaveFade): flat where its wavelength
// nears the cell of the clipmap level d lies in (at most max(WaterFinestCellSize, d / WaterLevelDistanceCells)), and every
// wave flat from WaterHorizonFadeEndCells of the last level's cells on, where the flat skirt begins. The fade depends on the
// rest point and the camera alone, so the vertices two levels share displace alike and the levels meet without a crack. The
// CPU never fades (decision 3: distant short waves are drawn flat while the physics keeps them).
#pragma once

#include "Shared/WaterConstants.h"

// The surface at a rest point: the particle's displacement from it (horizontal x and z, and its height above the base in y),
// the surface's unit normal (cross(dP/dz0, dP/dx0), Physics/WaterWaves.h's), the horizontal map's Jacobian determinant (1 on
// calm water, below 1 where the waves crowd the surface together at a crest) and its matrix (dx/dx0, dx/dz0, dz/dx0,
// dz/dz0).
struct WaterWaveSample
{
	float3 Displacement;
	float3 Normal;
	float Jacobian;
	float4 Horizontal;
};

// The fade of a wave of wave number `waveNumber` at rest point `rest` of `record`'s water (see the file comment).
float GetWaterWaveFade(WaterRecord record, float waveNumber, float2 rest)
{
	const float2 offset = abs(rest - record.CameraOffset);
	const float distance = max(offset.x, offset.y);
	const float cell = max(WaterFinestCellSize, distance / WaterLevelDistanceCells);
	const float wavelength = 6.28318530718 / max(waveNumber, 1e-6);
	const float waveFade = saturate((wavelength / cell - WaterFadeCellsPerWave) / WaterFadeCellsPerWave);
	const float lastCell = WaterFinestCellSize * float(1u << (WaterLevelCount - 1u));
	const float horizonFade = saturate((WaterHorizonFadeEndCells * lastCell - distance) / (WaterHorizonFadeCells * lastCell));
	return waveFade * horizonFade;
}

// The surface of `record`'s water at rest point `rest` in this frame, or with `previous` in the previous one (its previous
// phases; the displacement is in this frame's world, WaterRecord::PreviousFromCurrent moves it to the previous one).
WaterWaveSample EvaluateWaterWaves(WaterRecord record, float2 rest, bool previous)
{
	float3 displacement = float3(0.0);
	// The Jacobian of the horizontal map and the slope of the height, by the rest point's x and z.
	float dxdx = 1.0;
	float dxdz = 0.0;
	float dzdx = 0.0;
	float dzdz = 1.0;
	float dydx = 0.0;
	float dydz = 0.0;
	for (uint index = 0; index < min(record.WaveCount, MaxWaterRecordWaves); ++index)
	{
		const float4 wave = record.Waves[index];
		const float4 shape = record.WavePhases[index];
		const float2 direction = wave.xy;
		const float waveNumber = wave.z;
		const float amplitude = wave.w * GetWaterWaveFade(record, waveNumber, rest);
		const float steepness = shape.x;
		const float phase = (previous ? shape.z : shape.y) + waveNumber * dot(direction, rest);
		const float s = sin(phase);
		const float c = cos(phase);
		const float along = steepness * amplitude * c;
		displacement += float3(along * direction.x, amplitude * s, along * direction.y);
		const float crowd = steepness * amplitude * waveNumber * s;
		dxdx -= crowd * direction.x * direction.x;
		dxdz -= crowd * direction.x * direction.y;
		dzdx -= crowd * direction.y * direction.x;
		dzdz -= crowd * direction.y * direction.y;
		const float slope = amplitude * waveNumber * c;
		dydx += slope * direction.x;
		dydz += slope * direction.y;
	}
	WaterWaveSample sample;
	sample.Displacement = displacement;
	const float3 alongX = float3(dxdx, dydx, dzdx);
	const float3 alongZ = float3(dxdz, dydz, dzdz);
	const float3 normal = cross(alongZ, alongX);
	sample.Normal = normal * rsqrt(max(dot(normal, normal), 1e-20));
	sample.Jacobian = dxdx * dzdz - dxdz * dzdx;
	sample.Horizontal = float4(dxdx, dxdz, dzdx, dzdz);
	return sample;
}

// W47.3's Underwater pass: the newton steps that find the rest point under a point (WaterSurfaceAt).
static const uint WaterSurfaceNewtonSteps = 6;

// The surface of `record`'s water in this frame over `target` (x and z from the record's Origin): the rest point whose
// displaced point lies over it, found by WaterSurfaceNewtonSteps Newton steps on the horizontal map from the target itself,
// and the surface's height there (world y). The drawn surface's waves, faded as the clipmap fades them (GetWaterWaveFade), so
// the Underwater pass's waterline follows what the view shows rather than the CPU's unfaded surface; the sum of Q k A below
// 0.9 keeps the map's Jacobian invertible.
float GetWaterSurfaceHeight(WaterRecord record, float2 target)
{
	float2 rest = target;
	WaterWaveSample sample = EvaluateWaterWaves(record, rest, false);
	for (uint step = 0; step < WaterSurfaceNewtonSteps; ++step)
	{
		const float2 residual = rest + sample.Displacement.xz - target;
		const float determinant = sample.Jacobian;
		if (!(abs(determinant) > 1e-6))
			break;
		const float4 m = sample.Horizontal; // (dx/dx0, dx/dz0, dz/dx0, dz/dz0)
		rest -= float2(m.w * residual.x - m.y * residual.y, m.x * residual.y - m.z * residual.x) / determinant;
		sample = EvaluateWaterWaves(record, rest, false);
	}
	return record.BaseHeight + sample.Displacement.y;
}

// A Rectangle's rest point `rest` clamped into its footprint (its local X and Z within HalfExtents of FrameOrigin), so a
// clipmap draws exactly the footprint: a vertex outside it lands on its edge. An Ocean's rest point unchanged.
float2 ClampToWaterFootprint(WaterRecord record, float2 rest)
{
	if ((record.Flags & WaterRecordFlagRectangle) == 0)
		return rest;
	const float2 axisX = record.AxisX;
	const float2 axisZ = float2(-axisX.y, axisX.x);
	const float2 relative = rest - record.FrameOrigin;
	const float2 local = clamp(float2(dot(relative, axisX), dot(relative, axisZ)), -record.HalfExtents, record.HalfExtents);
	return record.FrameOrigin + local.x * axisX + local.y * axisZ;
}

// Whether `point` (x and z from the record's Origin, y the world height) lies in `record`'s volume: a Rectangle's footprint
// from Depth below its base to Headroom above it (Scene/WaterQueries.h's IsInWaterVolume, boundaries included). An Ocean's
// volume is everywhere.
bool IsInWaterVolume(WaterRecord record, float3 point)
{
	if ((record.Flags & WaterRecordFlagRectangle) == 0)
		return true;
	const float2 axisX = record.AxisX;
	const float2 relative = point.xz - record.FrameOrigin;
	const float2 local = float2(dot(relative, axisX), dot(relative, float2(-axisX.y, axisX.x)));
	return all(abs(local) <= record.HalfExtents) && point.y >= record.BaseHeight - record.Depth && point.y <= record.BaseHeight + record.Headroom;
}
