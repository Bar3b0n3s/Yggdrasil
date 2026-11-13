// The probes' indirect term (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; Architecture §8.6):
// the baked irradiance volumes and reflection probes of the view, added after M23's light loop and outside it
// (Common/LightLoop.slang). Passes/Scene.slang includes it after its resource declarations, which it reads by name: the view's
// Probes (ConstantBuffer<ProbeConstants>, b4 of set 0, Shared/ProbeConstants.h), IrradianceProbes (the volumes' SH9
// coefficients, StructuredBuffer<float4>, t13), ReflectionProbeCubes (the reflection probes' prefiltered specular cubes,
// TextureCubeArray<float4>, t14) and LinearClamp (s0); and EvaluateIrradianceSH9, which the includer imports from
// Common.Environment. A later pass that evaluates the probes' reflection (SSR's composite, decision 18) declares the same.
//
// Each probe's box weighs it by the distance of the shaded point from its nearest face, in metres: 0 outside the box, rising
// linearly to 1 at BlendDistance inside it (1 everywhere inside with BlendDistance 0). The terms start from the Environment's
// (its SH9 irradiance, or the constant ambient, and its prefiltered specular) and blend each probe over them in the order of
// ProbeConstants' lists, the largest box first, as term * (1 - weight) + probe * weight: inside one box the probe replaces
// the Environment, fading back to it over BlendDistance, and where boxes overlap the smaller, blended last, wins, fading into
// the larger one at its faces. A volume's irradiance is its eight probes around the point, trilinear in its grid (the
// coefficients interpolated, then evaluated at the shading normal, as the Environment's SH9 is), times its Intensity; a
// reflection probe's is its cube sampled at the reflected direction, box-projected with BoxProjection (the ray from the
// point to the box's face it leaves through, seen from the probe's origin, where the cube was captured), at the roughness's
// lod, times its Intensity. With no probe (Counts 0) neither term changes.
#pragma once

#include "Shared/ProbeConstants.h"

// The weight of the box `halfSize` about the origin of the local space `worldToLocal` maps into, at the world point
// `worldPosition`, and the point in that local space (`local`).
float GetProbeBoxWeight(float4x4 worldToLocal, float4 halfSize, float4 axisScale, float3 worldPosition, out float3 local)
{
	local = mul(worldToLocal, float4(worldPosition, 1.0)).xyz;
	// How far inside each pair of faces the point is, in metres.
	const float3 inside = (halfSize.xyz - abs(local)) * axisScale.xyz;
	const float nearest = min(inside.x, min(inside.y, inside.z));
	if (nearest < 0.0)
		return 0.0;
	return halfSize.w > 0.0 ? saturate(nearest / halfSize.w) : 1.0;
}

// The irradiance / pi the volume `volume` gives at its local point `local` (inside its box) for the unit world normal `normal`:
// its probes' coefficients interpolated trilinearly in its grid, evaluated at `normal`, times its Intensity.
float3 EvaluateVolumeIrradiance(ProbeVolumeConstants volume, float3 local, float3 normal)
{
	const uint3 counts = volume.Probes.xyz;
	const float3 cells = float3(counts - uint3(1, 1, 1));
	// The grid's corner probes sit at the box's corners: -HalfSize maps to grid position 0 and +HalfSize to counts - 1.
	const float3 grid = clamp((local / (2.0 * volume.HalfSize.xyz) + 0.5) * cells, float3(0.0), cells);
	const uint3 base = min(uint3(grid), counts - uint3(2, 2, 2));
	const float3 fraction = grid - float3(base);
	float4 coefficients[IrradianceProbeCoefficientCount];
	for (uint coefficient = 0; coefficient < IrradianceProbeCoefficientCount; ++coefficient)
		coefficients[coefficient] = float4(0.0);
	for (uint corner = 0; corner < 8; ++corner)
	{
		const uint3 step = uint3(corner & 1, (corner >> 1) & 1, corner >> 2);
		const float3 weights = select(step != uint3(0, 0, 0), fraction, float3(1.0) - fraction);
		const float weight = weights.x * weights.y * weights.z;
		const uint3 probe = base + step;
		const uint first = volume.Probes.w + IrradianceProbeCoefficientCount * (probe.x + counts.x * (probe.y + counts.y * probe.z));
		for (uint coefficient = 0; coefficient < IrradianceProbeCoefficientCount; ++coefficient)
			coefficients[coefficient] += IrradianceProbes[first + coefficient] * weight;
	}
	return EvaluateIrradianceSH9(coefficients, normal) * volume.AxisScale.w;
}

// The diffuse irradiance / pi at `worldPosition` for the unit shading normal `normal`: `environment` (the Environment's, or
// the constant ambient) with the view's irradiance volumes blended over it.
float3 ShadeProbeIrradiance(float3 worldPosition, float3 normal, float3 environment)
{
	float3 irradiance = environment;
	for (uint index = 0; index < Probes.Counts.x; ++index)
	{
		const ProbeVolumeConstants volume = Probes.Volumes[index];
		float3 local;
		const float weight = GetProbeBoxWeight(volume.WorldToLocal, volume.HalfSize, volume.AxisScale, worldPosition, local);
		if (weight > 0.0)
			irradiance = irradiance * (1.0 - weight) + EvaluateVolumeIrradiance(volume, local, normal) * weight;
	}
	return irradiance;
}

// The direction the reflection probe `probe` is looked up at for the world reflection direction `reflection` from its local
// point `local` (inside its box): `reflection` itself, or with BoxProjection the point where the ray leaves the box, seen
// from the probe's origin.
float3 GetProbeLookupDirection(ProbeReflectionConstants probe, float3 local, float3 reflection)
{
	if (probe.Indices.y == 0)
		return reflection;
	const float3 direction = mul((float3x3)probe.WorldToLocal, reflection);
	// The face the ray moves towards along each axis; an axis it does not move along never stops it.
	const float3 planes = select(direction >= float3(0.0), probe.HalfSize.xyz, -probe.HalfSize.xyz);
	const bool3 moving = abs(direction) > float3(1e-20);
	const float3 distances = select(moving, (planes - local) / select(moving, direction, float3(1.0)), float3(3.0e38));
	const float travel = max(min(distances.x, min(distances.y, distances.z)), 0.0);
	return mul((float3x3)probe.LocalToWorld, local + direction * travel);
}

// The prefiltered specular radiance at `worldPosition` for the world reflection direction `reflection` and the (widened)
// roughness `roughness`: `environment` (the Environment's, or the constant ambient) with the view's reflection probes
// blended over it.
float3 ShadeProbeSpecular(float3 worldPosition, float3 reflection, float roughness, float3 environment)
{
	float3 prefiltered = environment;
	for (uint index = 0; index < Probes.Counts.y; ++index)
	{
		const ProbeReflectionConstants probe = Probes.Reflections[index];
		float3 local;
		const float weight = GetProbeBoxWeight(probe.WorldToLocal, probe.HalfSize, probe.AxisScale, worldPosition, local);
		if (weight > 0.0)
		{
			const float3 lookup = GetProbeLookupDirection(probe, local, reflection);
			const float3 sampled =
				ReflectionProbeCubes.SampleLevel(LinearClamp, float4(lookup, float(probe.Indices.x)), roughness * probe.Lookup.x).rgb * probe.AxisScale.w;
			prefiltered = prefiltered * (1.0 - weight) + sampled * weight;
		}
	}
	return prefiltered;
}
