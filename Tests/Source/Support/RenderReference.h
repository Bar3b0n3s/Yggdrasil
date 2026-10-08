#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// CPU reference implementations of the renderer's math (Architecture §15.3 "device-independent oracles"; Roadmap M8): the
// GPU oracle tests compare the shaders' results with these. Written from the papers and the formulas the engine headers
// freeze (Renderer/BrdfLut.h, Asset/EnvironmentData.h, Shared/EnvironmentConstants.h, Renderer/TonemapPass.h), never from
// the shaders, so a shared mistake cannot hide; double precision throughout. Deterministic and thread-safe (pure
// functions). Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 14); its own tests
// (RenderReferenceTests.cpp) check each against analytic values, and the renderer's GPU oracle tests compare with it.

namespace Engine {

	namespace Test {

		// The minimum perceptual roughness of §8.5 (shading clamps to it).
		inline constexpr double MinPerceptualRoughness = 0.045;

		// --- BRDF (§8.5) ---------------------------------------------------------------------------------------------------

		// The i-th point of the n-point Hammersley sequence: (i / n, the radical inverse of i in base 2). `index` < `count`.
		[[nodiscard]] glm::dvec2 Hammersley(uint32_t index, uint32_t count);

		// The GGX (Trowbridge-Reitz) normal distribution D(h) for alpha = perceptual roughness squared.
		[[nodiscard]] double DistributionGgx(double nDotH, double alpha);

		// The height-correlated Smith visibility V = G / (4 NdotL NdotV) (Heitz 2014, Filament's form).
		[[nodiscard]] double VisibilitySmithGgxCorrelated(double nDotV, double nDotL, double alpha);

		// Schlick's Fresnel with F90 = 1.
		[[nodiscard]] glm::dvec3 FresnelSchlick(const glm::dvec3& f0, double vDotH);

		// A GGX-distributed half vector in tangent space (z up) for the uniform point `xi` and `alpha`.
		[[nodiscard]] glm::dvec3 ImportanceSampleGgx(const glm::dvec2& xi, double alpha);

		// The DFG terms of Renderer/BrdfLut.h at (NdotV, perceptual roughness): (DFG1, DFG2) = (integral of Fc * Gv, integral of
		// Gv), estimated with `sampleCount` Hammersley GGX samples exactly as the header describes.
		[[nodiscard]] glm::dvec2 ComputeDfg(double nDotV, double perceptualRoughness, uint32_t sampleCount);

		// The whole LUT as BrdfLut stores it: size² (DFG1, DFG2) pairs, rows top first, texel (i, j) at NdotV = (i + 0.5) / size
		// and perceptual roughness (j + 0.5) / size.
		[[nodiscard]] std::vector<glm::dvec2> ComputeDfgLut(uint32_t size, uint32_t sampleCount);

		// The directional albedo of the specular lobe with the multi-scatter energy compensation of §8.5 (single scatter
		// lerp(DFG1, DFG2, f0) times 1 + f0 (1 / DFG2 - 1)) for a white f0 of 1: the "furnace for BRDF" expects 1 for every
		// NdotV and roughness, within the test's tolerance.
		[[nodiscard]] double ComputeCompensatedSpecularAlbedo(double nDotV, double perceptualRoughness, uint32_t sampleCount);

		// --- Environments (§8.6; the conventions of Asset/EnvironmentData.h) -----------------------------------------------

		// A cube of linear RGB, mip 0 only: six faces (+X, -X, +Y, -Y, +Z, -Z) of Size² texels, rows top first.
		struct ReferenceCube
		{
			uint32_t Size = 0;
			std::array<std::vector<glm::dvec3>, 6> Faces{};
		};

		// The unit world direction of texel (column, row) of `face` of a cube of `size` (Vulkan's face selection table).
		[[nodiscard]] glm::dvec3 CubeTexelDirection(uint32_t face, uint32_t column, uint32_t row, uint32_t size);

		// The solid angle of texel (column, row) of a face of `size` (the exact area formula; summed over a cube it is 4 pi).
		[[nodiscard]] double CubeTexelSolidAngle(uint32_t column, uint32_t row, uint32_t size);

		// The equirectangular (u, v) of a unit direction and back (EnvironmentData.h's mapping; u in [0, 1), v in [0, 1]).
		[[nodiscard]] glm::dvec2 DirectionToEquirectUv(const glm::dvec3& direction);
		[[nodiscard]] glm::dvec3 EquirectUvToDirection(const glm::dvec2& uv);

		// Resamples an equirectangular RGB image (`width` = 2 * `height`, rows top first) into a cube of `size` (bilinear,
		// wrapping in u, clamped in v), as the bake's step 2 does for mip 0.
		[[nodiscard]] ReferenceCube EquirectToCube(std::span<const float> rgb, uint32_t width, uint32_t height, uint32_t size);

		// The GGX prefilter of `source` for `perceptualRoughness` into a cube of `size` by brute force over every source
		// texel (N = V = R, weight D(h) NdotL times the texel's solid angle, normalized), the reference of the bake's step 3
		// ("prefilter vs CPU at 16²"). Roughness 0 resamples `source` bilinearly at each texel's direction (within the face,
		// clamped at its edges): a copy when the sizes are equal and, for a source of twice the size, the 2x2 box average,
		// which is the bake's mip-0 rule (EnvironmentBaker.h) whenever the sizes are powers of two.
		[[nodiscard]] ReferenceCube PrefilterSpecular(const ReferenceCube& source, double perceptualRoughness, uint32_t size);

		// The SH9 coefficients of irradiance / pi of `source` with the Hanning window of EnvironmentData.h, in its order.
		[[nodiscard]] std::array<glm::dvec3, 9> ProjectIrradianceSH9(const ReferenceCube& source);

		// The SH9 value at the unit direction `normal` (EnvironmentData.h's basis), not clamped.
		[[nodiscard]] glm::dvec3 EvaluateIrradianceSH9(std::span<const glm::dvec3, 9> coefficients, const glm::dvec3& normal);

		// --- Tonemapping (§8.9; Renderer/TonemapPass.h) ---------------------------------------------------------------------

		// The tonemappers on exposed linear radiance, each returning display-linear values in [0, 1] before the OETF, exactly
		// as Renderer/TonemapPass.h's file comment writes them out (the shader implements the same text): AgX (the inset
		// matrix, the log2 encoding over [-12.47393, 4.026069], the 6th-order sigmoid polynomial, the outset matrix and the
		// 2.2 power), ACES (Hill's input matrix, RRT+ODT fit and output matrix, no exposure pre-scale), Khronos PBR Neutral
		// and Linear (clamp).
		[[nodiscard]] glm::dvec3 TonemapAgx(const glm::dvec3& color);
		[[nodiscard]] glm::dvec3 TonemapAces(const glm::dvec3& color);
		[[nodiscard]] glm::dvec3 TonemapPbrNeutral(const glm::dvec3& color);
		[[nodiscard]] glm::dvec3 TonemapLinear(const glm::dvec3& color);
		[[nodiscard]] glm::dvec3 ApplyTonemapper(RenderTonemapper tonemapper, const glm::dvec3& color);

		// The sRGB OETF (IEC 61966-2-1) of a value in [0, 1], and its inverse.
		[[nodiscard]] double LinearToSrgb(double value);
		[[nodiscard]] double SrgbToLinear(double value);

		// The 1,024 sample points of the tonemapper curve test (§15.3): 2^e * c for e = -12 + 24 k / 255 (k = 0..255) and the
		// colours c = (1, 1, 1), (1, 0.25, 0.05), (0.05, 1, 0.25), (0.25, 0.05, 1), colour by colour, so the curves are covered
		// from deep shadow to far above white and every tonemapper's hue handling is exercised.
		[[nodiscard]] std::vector<glm::dvec3> MakeTonemapSamplePoints();

		// IEEE 754 binary16 conversions (round to nearest even), for RGBA16_FLOAT readbacks and uploads.
		[[nodiscard]] double HalfToDouble(uint16_t half);
		[[nodiscard]] uint16_t DoubleToHalf(double value);

	}

}
