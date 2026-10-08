// The BRDF of the PBR forward passes and of the DFG LUT generator (Architecture §8.5; Renderer/BrdfLut.h): the GGX
// (Trowbridge-Reitz) normal distribution, the height-correlated Smith visibility (Heitz 2014, in Filament's form), Schlick's
// Fresnel with F90 = 1 and Lambert's diffuse, plus the Hammersley sequence and GGX importance sampling (Karis 2013) the LUT
// integrates with. Alpha is always the perceptual roughness squared (Docs/Decisions/0013-m8-decisions.md decision 7).
module Brdf;

public static const float Pi = 3.14159265358979323846;
public static const float InversePi = 0.31830988618379067154;

// The smallest perceptual roughness shading uses (§8.5): smoother values would alias and overflow binary16 highlights.
public static const float MinPerceptualRoughness = 0.045;

// The GGX normal distribution D(h) = alpha² / (pi ((n.h)² (alpha² - 1) + 1)²).
public float DistributionGgx(float nDotH, float alpha)
{
	const float alpha2 = alpha * alpha;
	const float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
	return alpha2 / max(Pi * denominator * denominator, 1e-20);
}

// The height-correlated Smith visibility V = G2 / (4 (n.l) (n.v)) = 0.5 / ((n.l) sqrt((n.v)² (1 - alpha²) + alpha²) +
// (n.v) sqrt((n.l)² (1 - alpha²) + alpha²)).
public float VisibilitySmithGgxCorrelated(float nDotV, float nDotL, float alpha)
{
	const float alpha2 = alpha * alpha;
	const float lambdaV = nDotL * sqrt((nDotV - nDotV * alpha2) * nDotV + alpha2);
	const float lambdaL = nDotV * sqrt((nDotL - nDotL * alpha2) * nDotL + alpha2);
	return 0.5 / max(lambdaV + lambdaL, 1e-20);
}

// (1 - v.h)^5, Schlick's Fresnel weight.
public float FresnelWeight(float vDotH)
{
	const float x = saturate(1.0 - vDotH);
	const float x2 = x * x;
	return x2 * x2 * x;
}

// Schlick's Fresnel F = F0 + (1 - F0) (1 - v.h)^5.
public float3 FresnelSchlick(float3 f0, float vDotH)
{
	return f0 + (float3(1.0) - f0) * FresnelWeight(vDotH);
}

// The i-th point of the `count`-point Hammersley sequence: (i / count, the base-2 radical inverse of i).
public float2 Hammersley(uint index, uint count)
{
	return float2(float(index) / float(count), float(reversebits(index)) * 2.3283064365386963e-10);
}

// A GGX-distributed half vector in tangent space (z along the normal) for the uniform point `xi`: phi = 2 pi xi.x and
// cos(theta) = sqrt((1 - xi.y) / (1 + (alpha² - 1) xi.y)) (Karis 2013).
public float3 ImportanceSampleGgx(float2 xi, float alpha)
{
	const float phi = 2.0 * Pi * xi.x;
	const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
	const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
	return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}
