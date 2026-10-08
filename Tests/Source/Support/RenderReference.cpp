#include "TestsPCH.h"
#include "Support/RenderReference.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Core/Assert.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

// The CPU references (RenderReference.h), written from the papers and the formulas the engine headers freeze, in double
// precision. Conventions the headers leave open, which GPU code compared with these sample for sample must share:
//   - ImportanceSampleGgx takes phi = 2 pi xi.x and cos(theta) from xi.y (Karis 2013, "Real Shading in Unreal Engine 4"),
//     so with Hammersley points the sample index drives the azimuth and the radical inverse the polar angle;
//   - ComputeDfg places V = (sqrt(1 - NdotV²), 0, NdotV) in tangent space (N = +Z) and drops samples with NdotL <= 0, the
//     estimator of Karis 2014 and Filament ("Physically Based Rendering in Filament", section 5.3.4.3), whose 4 / N factor
//     turns D * NdotH / (4 VdotH), the PDF of the reflected direction, into Gv = V * 4 NdotL VdotH / NdotH;
//   - ComputeCompensatedSpecularAlbedo integrates the single-scatter albedo independently of the DFG estimator (a midpoint
//     rule over the half-vector hemisphere with D, V and F evaluated explicitly) and applies the compensation from DFG2, so
//     the furnace checks that the BRDF terms and the LUT's estimator agree instead of cancelling DFG2 against itself;
//   - the SH basis constants are EnvironmentData.h's six-digit values, as Common/Environment.slang uses them.

namespace Engine {

	namespace Test {

		namespace {

			constexpr double Pi = glm::pi<double>();
			constexpr double TwoPi = glm::two_pi<double>();

			// The real SH basis of EnvironmentData.h, digit for digit.
			constexpr double ShBand0 = 0.282095;
			constexpr double ShBand1 = 0.488603;
			constexpr double ShBand2Cross = 1.092548;
			constexpr double ShBand2Zonal = 0.315392;
			constexpr double ShBand2Sectoral = 0.546274;

			// The clamped-cosine convolution of SH band l divided by pi (Ramamoorthi and Hanrahan 2001: A0 = pi, A1 = 2 pi / 3,
			// A2 = pi / 4), so the projected coefficients are those of irradiance / pi.
			constexpr std::array<double, 3> CosineLobeOverPi = { 1.0, 2.0 / 3.0, 0.25 };

			// The band of each of the nine coefficients, in EnvironmentData.h's order.
			constexpr std::array<uint32_t, 9> ShBands = { 0, 1, 1, 1, 2, 2, 2, 2, 2 };

			// Resolution of the midpoint rule of the compensated albedo's single-scatter integral, per angle.
			constexpr uint32_t AlbedoPolarSteps = 384;
			constexpr uint32_t AlbedoAzimuthSteps = 384;

			// The AgX constants of Renderer/TonemapPass.h.
			constexpr double AgxMinEv = -12.47393;
			constexpr double AgxMaxEv = 4.026069;
			// PBR Neutral's compression start (0.8 - 0.04) and desaturation.
			constexpr double NeutralStartCompression = 0.76;
			constexpr double NeutralDesaturation = 0.15;

			// A 3x3 matrix written row by row, applied as out = M * in (Renderer/TonemapPass.h).
			using RowMatrix = std::array<glm::dvec3, 3>;

			constexpr RowMatrix AgxInset = { {
				{ 0.842479062253094, 0.0784335999999992, 0.0792237451477643 },
				{ 0.0423282422610123, 0.878468636469772, 0.0791661274605434 },
				{ 0.0423756549057051, 0.0784336, 0.879142973793104 },
			} };
			constexpr RowMatrix AgxOutset = { {
				{ 1.19687900512017, -0.0980208811401368, -0.0990297440797205 },
				{ -0.0528968517574562, 1.15190312990417, -0.0989611768448433 },
				{ -0.0529716355144438, -0.0980434501171241, 1.15107367264116 },
			} };
			constexpr RowMatrix AcesInput = { {
				{ 0.59719, 0.35458, 0.04823 },
				{ 0.07600, 0.90834, 0.01566 },
				{ 0.02840, 0.13383, 0.83777 },
			} };
			constexpr RowMatrix AcesOutput = { {
				{ 1.60475, -0.53108, -0.07367 },
				{ -0.10208, 1.10813, -0.00605 },
				{ -0.00327, -0.07276, 1.07602 },
			} };

			glm::dvec3 Multiply(const RowMatrix& matrix, const glm::dvec3& value)
			{
				return glm::dvec3(glm::dot(matrix[0], value), glm::dot(matrix[1], value), glm::dot(matrix[2], value));
			}

			// The van der Corput radical inverse of `bits` in base 2: the bits mirrored about the binary point.
			double RadicalInverseBase2(uint32_t bits)
			{
				bits = (bits << 16U) | (bits >> 16U);
				bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
				bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
				bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
				bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
				return static_cast<double>(bits) / 4294967296.0;
			}

			// The nine basis values at the unit direction `direction`, in EnvironmentData.h's order.
			std::array<double, 9> EvaluateShBasis(const glm::dvec3& direction)
			{
				const double x = direction.x;
				const double y = direction.y;
				const double z = direction.z;
				return {
					ShBand0,
					ShBand1 * y,
					ShBand1 * z,
					ShBand1 * x,
					ShBand2Cross * x * y,
					ShBand2Cross * y * z,
					ShBand2Zonal * (3.0 * z * z - 1.0),
					ShBand2Cross * x * z,
					ShBand2Sectoral * (x * x - y * y),
				};
			}

			// The Hanning window of EnvironmentData.h for SH band `band`: (1 + cos(pi l / w)) / 2.
			double HanningWindow(uint32_t band)
			{
				return 0.5 * (1.0 + std::cos(Pi * static_cast<double>(band) / static_cast<double>(EnvironmentData::IrradianceShWindow)));
			}

			// The integral over [0, x] x [0, y] of a cube face at distance 1 as a solid angle (the area element 1 / (1 + u² + v²)^1.5).
			double FaceAreaElement(double x, double y)
			{
				return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0));
			}

			bool IsValidCube(const ReferenceCube& cube)
			{
				const size_t texels = static_cast<size_t>(cube.Size) * cube.Size;
				return cube.Size > 0 && std::ranges::all_of(cube.Faces, [texels](const std::vector<glm::dvec3>& face)
				{
					return face.size() == texels;
				});
			}

			// Bilinear sample of one face of `cube` at the face coordinates (s, t) in [0, 1], clamped at the face's edges.
			glm::dvec3 SampleFaceBilinear(const ReferenceCube& cube, uint32_t face, double s, double t)
			{
				const double maxIndex = static_cast<double>(cube.Size) - 1.0;
				const double x = std::clamp(s * cube.Size - 0.5, 0.0, maxIndex);
				const double y = std::clamp(t * cube.Size - 0.5, 0.0, maxIndex);
				const uint32_t column0 = static_cast<uint32_t>(std::floor(x));
				const uint32_t row0 = static_cast<uint32_t>(std::floor(y));
				const uint32_t column1 = std::min(column0 + 1, cube.Size - 1);
				const uint32_t row1 = std::min(row0 + 1, cube.Size - 1);
				const double fx = x - column0;
				const double fy = y - row0;
				const std::vector<glm::dvec3>& texels = cube.Faces[face];
				const auto at = [&texels, &cube](uint32_t column, uint32_t row)
				{
					return texels[static_cast<size_t>(row) * cube.Size + column];
				};
				const glm::dvec3 top = glm::mix(at(column0, row0), at(column1, row0), fx);
				const glm::dvec3 bottom = glm::mix(at(column0, row1), at(column1, row1), fx);
				return glm::mix(top, bottom, fy);
			}

			// The single-scatter directional albedo of the specular lobe for a scalar f0: the integral of D V F NdotL over the
			// light directions, written over half vectors (dL = 4 VdotH dH) with tan(theta_h) = alpha tan(psi), so the midpoint
			// rule in psi resolves the GGX peak of any roughness; D, V and F are evaluated explicitly.
			double IntegrateSingleScatterAlbedo(double nDotV, double alpha, double f0)
			{
				const glm::dvec3 view(std::sqrt(std::max(0.0, 1.0 - nDotV * nDotV)), 0.0, nDotV);
				const double polarStep = 0.5 * Pi / AlbedoPolarSteps;
				const double azimuthStep = TwoPi / AlbedoAzimuthSteps;
				double albedo = 0.0;
				for (uint32_t polar = 0; polar < AlbedoPolarSteps; ++polar)
				{
					const double psi = (polar + 0.5) * polarStep;
					const double tanPsi = std::tan(psi);
					const double theta = std::atan(alpha * tanPsi);
					// d(theta) / d(psi) of tan(theta) = alpha tan(psi).
					const double thetaPerPsi = alpha * (1.0 + tanPsi * tanPsi) / (1.0 + alpha * alpha * tanPsi * tanPsi);
					const double cosTheta = std::cos(theta);
					const double sinTheta = std::sin(theta);
					const double distribution = DistributionGgx(cosTheta, alpha);
					for (uint32_t azimuth = 0; azimuth < AlbedoAzimuthSteps; ++azimuth)
					{
						const double phi = (azimuth + 0.5) * azimuthStep;
						const glm::dvec3 half(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
						const double vDotH = glm::dot(view, half);
						if (vDotH <= 0.0)
							continue;
						const double nDotL = 2.0 * vDotH * half.z - view.z;
						if (nDotL <= 0.0)
							continue;
						const double fresnel = FresnelSchlick(glm::dvec3(f0), vDotH).x;
						const double visibility = VisibilitySmithGgxCorrelated(nDotV, nDotL, alpha);
						albedo += distribution * visibility * fresnel * nDotL * 4.0 * vDotH * sinTheta * thetaPerPsi;
					}
				}
				return albedo * polarStep * azimuthStep;
			}

			// The nearest-even integer of a non-negative finite `value`.
			double RoundHalfToEven(double value)
			{
				const double whole = std::floor(value);
				const double fraction = value - whole;
				if (fraction > 0.5 || (fraction == 0.5 && std::fmod(whole, 2.0) != 0.0))
					return whole + 1.0;
				return whole;
			}

		}

		glm::dvec2 Hammersley(uint32_t index, uint32_t count)
		{
			ENGINE_CORE_ASSERT(index < count, "Hammersley point {} of a {}-point sequence", index, count);
			return glm::dvec2(static_cast<double>(index) / static_cast<double>(count), RadicalInverseBase2(index));
		}

		double DistributionGgx(double nDotH, double alpha)
		{
			ENGINE_CORE_ASSERT(alpha > 0.0, "GGX needs a positive alpha, not {}", alpha);
			const double alphaSquared = alpha * alpha;
			const double denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
			return alphaSquared / (Pi * denominator * denominator);
		}

		double VisibilitySmithGgxCorrelated(double nDotV, double nDotL, double alpha)
		{
			const double alphaSquared = alpha * alpha;
			const double lambdaV = nDotL * std::sqrt(nDotV * nDotV * (1.0 - alphaSquared) + alphaSquared);
			const double lambdaL = nDotV * std::sqrt(nDotL * nDotL * (1.0 - alphaSquared) + alphaSquared);
			return 0.5 / (lambdaV + lambdaL);
		}

		glm::dvec3 FresnelSchlick(const glm::dvec3& f0, double vDotH)
		{
			const double complement = 1.0 - std::clamp(vDotH, 0.0, 1.0);
			const double complementSquared = complement * complement;
			return f0 + (glm::dvec3(1.0) - f0) * (complementSquared * complementSquared * complement);
		}

		glm::dvec3 ImportanceSampleGgx(const glm::dvec2& xi, double alpha)
		{
			const double phi = TwoPi * xi.x;
			const double cosThetaSquared = (1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y);
			const double cosTheta = std::sqrt(cosThetaSquared);
			const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosThetaSquared));
			return glm::dvec3(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
		}

		glm::dvec2 ComputeDfg(double nDotV, double perceptualRoughness, uint32_t sampleCount)
		{
			ENGINE_CORE_ASSERT(sampleCount > 0, "The DFG estimate needs samples");
			const double alpha = perceptualRoughness * perceptualRoughness;
			const glm::dvec3 view(std::sqrt(std::max(0.0, 1.0 - nDotV * nDotV)), 0.0, nDotV);
			glm::dvec2 sum(0.0);
			for (uint32_t index = 0; index < sampleCount; ++index)
			{
				const glm::dvec3 half = ImportanceSampleGgx(Hammersley(index, sampleCount), alpha);
				const double vDotH = glm::dot(view, half);
				const double nDotL = 2.0 * vDotH * half.z - view.z;
				const double nDotH = half.z;
				if (nDotL <= 0.0 || vDotH <= 0.0 || nDotH <= 0.0)
					continue;
				const double gv = VisibilitySmithGgxCorrelated(nDotV, nDotL, alpha) * 4.0 * nDotL * vDotH / nDotH;
				const double complement = 1.0 - vDotH;
				const double fc = complement * complement * complement * complement * complement;
				sum += glm::dvec2(fc * gv, gv);
			}
			return sum / static_cast<double>(sampleCount);
		}

		std::vector<glm::dvec2> ComputeDfgLut(uint32_t size, uint32_t sampleCount)
		{
			std::vector<glm::dvec2> texels;
			texels.reserve(static_cast<size_t>(size) * size);
			for (uint32_t row = 0; row < size; ++row)
			{
				const double roughness = (row + 0.5) / size;
				for (uint32_t column = 0; column < size; ++column)
					texels.push_back(ComputeDfg((column + 0.5) / size, roughness, sampleCount));
			}
			return texels;
		}

		double ComputeCompensatedSpecularAlbedo(double nDotV, double perceptualRoughness, uint32_t sampleCount)
		{
			// Shading clamps the roughness (§8.5) before it reads the LUT.
			constexpr double F0 = 1.0;
			const double roughness = std::clamp(perceptualRoughness, MinPerceptualRoughness, 1.0);
			const double alpha = roughness * roughness;
			const double singleScatter = IntegrateSingleScatterAlbedo(nDotV, alpha, F0);
			const glm::dvec2 dfg = ComputeDfg(nDotV, roughness, sampleCount);
			return singleScatter * (1.0 + F0 * (1.0 / dfg.y - 1.0));
		}

		glm::dvec3 CubeTexelDirection(uint32_t face, uint32_t column, uint32_t row, uint32_t size)
		{
			ENGINE_CORE_ASSERT(face < 6 && column < size && row < size, "Texel ({}, {}) of face {} of a {}-texel cube", column, row, face, size);
			// (s, t) in [-1, 1] per Vulkan's cube-map face selection: s = sc / |ma|, t = tc / |ma|.
			const double s = 2.0 * (column + 0.5) / size - 1.0;
			const double t = 2.0 * (row + 0.5) / size - 1.0;
			glm::dvec3 direction(0.0);
			switch (face)
			{
				case 0:  direction = glm::dvec3(1.0, -t, -s); break;
				case 1:  direction = glm::dvec3(-1.0, -t, s); break;
				case 2:  direction = glm::dvec3(s, 1.0, t); break;
				case 3:  direction = glm::dvec3(s, -1.0, -t); break;
				case 4:  direction = glm::dvec3(s, -t, 1.0); break;
				default: direction = glm::dvec3(-s, -t, -1.0); break;
			}
			return glm::normalize(direction);
		}

		double CubeTexelSolidAngle(uint32_t column, uint32_t row, uint32_t size)
		{
			ENGINE_CORE_ASSERT(column < size && row < size, "Texel ({}, {}) of a {}-texel face", column, row, size);
			const double x0 = 2.0 * column / size - 1.0;
			const double x1 = 2.0 * (column + 1.0) / size - 1.0;
			const double y0 = 2.0 * row / size - 1.0;
			const double y1 = 2.0 * (row + 1.0) / size - 1.0;
			return FaceAreaElement(x0, y0) - FaceAreaElement(x0, y1) - FaceAreaElement(x1, y0) + FaceAreaElement(x1, y1);
		}

		glm::dvec2 DirectionToEquirectUv(const glm::dvec3& direction)
		{
			// direction = (sin(theta) sin(phi), cos(theta), -sin(theta) cos(phi)), theta = v pi, phi = (u - 0.5) 2 pi.
			const double theta = std::acos(std::clamp(direction.y, -1.0, 1.0));
			const double phi = std::atan2(direction.x, -direction.z);
			double u = phi / TwoPi + 0.5;
			if (u >= 1.0)
				u -= 1.0;
			if (u < 0.0)
				u += 1.0;
			return glm::dvec2(u, theta / Pi);
		}

		glm::dvec3 EquirectUvToDirection(const glm::dvec2& uv)
		{
			const double theta = uv.y * Pi;
			const double phi = (uv.x - 0.5) * TwoPi;
			return glm::dvec3(std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi));
		}

		ReferenceCube EquirectToCube(std::span<const float> rgb, uint32_t width, uint32_t height, uint32_t size)
		{
			ENGINE_CORE_ASSERT(height > 0 && width == 2 * height && rgb.size() == static_cast<size_t>(width) * height * 3 && size > 0,
				"A {}x{} equirectangular image of {} floats into a {}-texel cube", width, height, rgb.size(), size);
			const auto texel = [rgb, width](uint32_t column, uint32_t row)
			{
				const size_t offset = (static_cast<size_t>(row) * width + column) * 3;
				return glm::dvec3(rgb[offset], rgb[offset + 1], rgb[offset + 2]);
			};
			ReferenceCube cube;
			cube.Size = size;
			for (uint32_t face = 0; face < 6; ++face)
			{
				std::vector<glm::dvec3>& texels = cube.Faces[face];
				texels.reserve(static_cast<size_t>(size) * size);
				for (uint32_t row = 0; row < size; ++row)
				{
					for (uint32_t column = 0; column < size; ++column)
					{
						const glm::dvec2 uv = DirectionToEquirectUv(CubeTexelDirection(face, column, row, size));
						const double x = uv.x * width - 0.5;
						const double y = uv.y * height - 0.5;
						const double xFloor = std::floor(x);
						const double yFloor = std::floor(y);
						const double fx = x - xFloor;
						const double fy = y - yFloor;
						// Wrapping in u (columns), clamped in v (rows).
						const int64_t column0 = static_cast<int64_t>(xFloor);
						const uint32_t left = static_cast<uint32_t>((column0 % width + width) % width);
						const uint32_t right = (left + 1) % width;
						const int64_t lastRow = static_cast<int64_t>(height) - 1;
						const uint32_t top = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(yFloor), 0, lastRow));
						const uint32_t bottom = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(yFloor) + 1, 0, lastRow));
						const glm::dvec3 upper = glm::mix(texel(left, top), texel(right, top), fx);
						const glm::dvec3 lower = glm::mix(texel(left, bottom), texel(right, bottom), fx);
						texels.push_back(glm::mix(upper, lower, fy));
					}
				}
			}
			return cube;
		}

		ReferenceCube PrefilterSpecular(const ReferenceCube& source, double perceptualRoughness, uint32_t size)
		{
			ENGINE_CORE_ASSERT(IsValidCube(source) && size > 0, "Prefiltering a malformed {}-texel cube into {} texels", source.Size, size);
			ReferenceCube filtered;
			filtered.Size = size;
			if (perceptualRoughness <= 0.0)
			{
				for (uint32_t face = 0; face < 6; ++face)
				{
					std::vector<glm::dvec3>& texels = filtered.Faces[face];
					texels.reserve(static_cast<size_t>(size) * size);
					for (uint32_t row = 0; row < size; ++row)
					{
						for (uint32_t column = 0; column < size; ++column)
							texels.push_back(SampleFaceBilinear(source, face, (column + 0.5) / size, (row + 0.5) / size));
					}
				}
				return filtered;
			}

			// Every source texel's direction, solid angle and radiance, once.
			struct SourceTexel
			{
				glm::dvec3 Direction{};
				double SolidAngle = 0.0;
				glm::dvec3 Radiance{};
			};
			std::vector<SourceTexel> sourceTexels;
			sourceTexels.reserve(static_cast<size_t>(source.Size) * source.Size * 6);
			for (uint32_t face = 0; face < 6; ++face)
			{
				for (uint32_t row = 0; row < source.Size; ++row)
				{
					for (uint32_t column = 0; column < source.Size; ++column)
					{
						sourceTexels.push_back({ .Direction = CubeTexelDirection(face, column, row, source.Size),
							.SolidAngle = CubeTexelSolidAngle(column, row, source.Size),
							.Radiance = source.Faces[face][static_cast<size_t>(row) * source.Size + column] });
					}
				}
			}

			const double alpha = perceptualRoughness * perceptualRoughness;
			for (uint32_t face = 0; face < 6; ++face)
			{
				std::vector<glm::dvec3>& texels = filtered.Faces[face];
				texels.reserve(static_cast<size_t>(size) * size);
				for (uint32_t row = 0; row < size; ++row)
				{
					for (uint32_t column = 0; column < size; ++column)
					{
						// N = V = R.
						const glm::dvec3 normal = CubeTexelDirection(face, column, row, size);
						glm::dvec3 sum(0.0);
						double totalWeight = 0.0;
						for (const SourceTexel& texel : sourceTexels)
						{
							const double nDotL = glm::dot(normal, texel.Direction);
							if (nDotL <= 0.0)
								continue;
							const double nDotH = glm::dot(normal, glm::normalize(normal + texel.Direction));
							const double weight = DistributionGgx(nDotH, alpha) * nDotL * texel.SolidAngle;
							sum += weight * texel.Radiance;
							totalWeight += weight;
						}
						texels.push_back(totalWeight > 0.0 ? sum / totalWeight : glm::dvec3(0.0));
					}
				}
			}
			return filtered;
		}

		std::array<glm::dvec3, 9> ProjectIrradianceSH9(const ReferenceCube& source)
		{
			ENGINE_CORE_ASSERT(IsValidCube(source), "Projecting a malformed {}-texel cube", source.Size);
			std::array<glm::dvec3, 9> coefficients{};
			for (uint32_t face = 0; face < 6; ++face)
			{
				for (uint32_t row = 0; row < source.Size; ++row)
				{
					for (uint32_t column = 0; column < source.Size; ++column)
					{
						const glm::dvec3 radiance = source.Faces[face][static_cast<size_t>(row) * source.Size + column];
						const double solidAngle = CubeTexelSolidAngle(column, row, source.Size);
						const std::array<double, 9> basis = EvaluateShBasis(CubeTexelDirection(face, column, row, source.Size));
						for (size_t index = 0; index < coefficients.size(); ++index)
							coefficients[index] += radiance * (basis[index] * solidAngle);
					}
				}
			}
			for (size_t index = 0; index < coefficients.size(); ++index)
			{
				const uint32_t band = ShBands[index];
				coefficients[index] *= CosineLobeOverPi[band] * HanningWindow(band);
			}
			return coefficients;
		}

		glm::dvec3 EvaluateIrradianceSH9(std::span<const glm::dvec3, 9> coefficients, const glm::dvec3& normal)
		{
			const std::array<double, 9> basis = EvaluateShBasis(normal);
			glm::dvec3 value(0.0);
			for (size_t index = 0; index < coefficients.size(); ++index)
				value += coefficients[index] * basis[index];
			return value;
		}

		glm::dvec3 TonemapAgx(const glm::dvec3& color)
		{
			const glm::dvec3 inset = Multiply(AgxInset, glm::max(color, glm::dvec3(0.0)));
			glm::dvec3 sigmoid(0.0);
			for (glm::length_t channel = 0; channel < 3; ++channel)
			{
				const double encoded = (std::log2(std::max(inset[channel], 1e-10)) - AgxMinEv) / (AgxMaxEv - AgxMinEv);
				const double x = std::clamp(encoded, 0.0, 1.0);
				const double x2 = x * x;
				const double x4 = x2 * x2;
				sigmoid[channel] = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
			}
			const glm::dvec3 outset = Multiply(AgxOutset, sigmoid);
			glm::dvec3 display(0.0);
			for (glm::length_t channel = 0; channel < 3; ++channel)
				display[channel] = std::clamp(std::pow(std::max(outset[channel], 0.0), 2.2), 0.0, 1.0);
			return display;
		}

		glm::dvec3 TonemapAces(const glm::dvec3& color)
		{
			const glm::dvec3 v = Multiply(AcesInput, glm::max(color, glm::dvec3(0.0)));
			const glm::dvec3 numerator = v * (v + 0.0245786) - 0.000090537;
			const glm::dvec3 denominator = v * (0.983729 * v + 0.4329510) + 0.238081;
			return glm::clamp(Multiply(AcesOutput, numerator / denominator), 0.0, 1.0);
		}

		glm::dvec3 TonemapPbrNeutral(const glm::dvec3& color)
		{
			glm::dvec3 c = glm::max(color, glm::dvec3(0.0));
			const double x = std::min({ c.r, c.g, c.b });
			const double offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
			c -= offset;
			const double peak = std::max({ c.r, c.g, c.b });
			if (peak < NeutralStartCompression)
				return glm::clamp(c, 0.0, 1.0);
			constexpr double D = 1.0 - NeutralStartCompression;
			const double newPeak = 1.0 - D * D / (peak + D - NeutralStartCompression);
			c *= newPeak / peak;
			const double g = 1.0 - 1.0 / (NeutralDesaturation * (peak - newPeak) + 1.0);
			return glm::clamp(glm::mix(c, glm::dvec3(newPeak), g), 0.0, 1.0);
		}

		glm::dvec3 TonemapLinear(const glm::dvec3& color)
		{
			return glm::clamp(color, 0.0, 1.0);
		}

		glm::dvec3 ApplyTonemapper(RenderTonemapper tonemapper, const glm::dvec3& color)
		{
			switch (tonemapper)
			{
				case RenderTonemapper::AgX:        return TonemapAgx(color);
				case RenderTonemapper::Aces:       return TonemapAces(color);
				case RenderTonemapper::PbrNeutral: return TonemapPbrNeutral(color);
				case RenderTonemapper::Linear:     return TonemapLinear(color);
			}
			ENGINE_CORE_ASSERT(false, "Unknown RenderTonemapper {}", std::to_underlying(tonemapper));
			return TonemapLinear(color);
		}

		double LinearToSrgb(double value)
		{
			const double clamped = std::clamp(value, 0.0, 1.0);
			return clamped <= 0.0031308 ? 12.92 * clamped : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
		}

		double SrgbToLinear(double value)
		{
			const double clamped = std::clamp(value, 0.0, 1.0);
			return clamped <= 0.04045 ? clamped / 12.92 : std::pow((clamped + 0.055) / 1.055, 2.4);
		}

		std::vector<glm::dvec3> MakeTonemapSamplePoints()
		{
			constexpr std::array<glm::dvec3, 4> Colors = { glm::dvec3(1.0, 1.0, 1.0), glm::dvec3(1.0, 0.25, 0.05), glm::dvec3(0.05, 1.0, 0.25),
				glm::dvec3(0.25, 0.05, 1.0) };
			constexpr uint32_t StepsPerColor = 256;
			std::vector<glm::dvec3> points;
			points.reserve(Colors.size() * StepsPerColor);
			for (const glm::dvec3& color : Colors)
			{
				for (uint32_t step = 0; step < StepsPerColor; ++step)
				{
					const double exponent = -12.0 + 24.0 * step / (StepsPerColor - 1);
					points.push_back(std::exp2(exponent) * color);
				}
			}
			return points;
		}

		double HalfToDouble(uint16_t half)
		{
			const double sign = (half & 0x8000U) != 0 ? -1.0 : 1.0;
			const uint32_t exponent = (half >> 10U) & 0x1FU;
			const uint32_t mantissa = half & 0x3FFU;
			if (exponent == 0)
				return sign * std::ldexp(static_cast<double>(mantissa), -24);
			if (exponent == 0x1F)
				return mantissa == 0 ? sign * std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
			return sign * std::ldexp(static_cast<double>(mantissa + 1024U), static_cast<int>(exponent) - 25);
		}

		uint16_t DoubleToHalf(double value)
		{
			const uint16_t sign = std::signbit(value) ? 0x8000U : 0U;
			if (std::isnan(value))
				return static_cast<uint16_t>(sign | 0x7E00U);
			const double magnitude = std::abs(value);
			// 65520 lies halfway between the largest finite half (65504) and 2^16; ties go to the even neighbour, infinity.
			if (magnitude >= 65520.0)
				return static_cast<uint16_t>(sign | 0x7C00U);
			// floor(log2(magnitude)), exact for every non-zero finite double.
			const int unbiased = magnitude == 0.0 ? 0 : std::ilogb(magnitude);
			if (magnitude == 0.0 || unbiased < -14)
			{
				// Zero or subnormal: a multiple of 2^-24; rounding up to 1024 of them yields the smallest normal's bits.
				const double units = RoundHalfToEven(std::ldexp(magnitude, 24));
				return static_cast<uint16_t>(sign | static_cast<uint16_t>(units));
			}
			// Normal: 11 significant bits, the leading one implicit.
			double significand = RoundHalfToEven(std::ldexp(magnitude, 10 - unbiased));
			int biased = unbiased + 15;
			if (significand >= 2048.0)
			{
				significand /= 2.0;
				++biased;
			}
			if (biased >= 0x1F)
				return static_cast<uint16_t>(sign | 0x7C00U);
			return static_cast<uint16_t>(sign | (static_cast<uint32_t>(biased) << 10U) | (static_cast<uint32_t>(significand) - 1024U));
		}

	}

}
