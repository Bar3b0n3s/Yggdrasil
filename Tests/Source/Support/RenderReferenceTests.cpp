#include "TestsPCH.h"
#include "Support/RenderReference.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// The CPU references checked against analytic values (Docs/Decisions/0013-m8-decisions.md decision 14), so the GPU
// oracle tests compare against something known to be right.

namespace Engine {

	// A cube of `size` whose texels hold `radiance` evaluated at their directions.
	template<typename Function>
	static Test::ReferenceCube MakeCube(uint32_t size, const Function& radiance)
	{
		Test::ReferenceCube cube;
		cube.Size = size;
		for (uint32_t face = 0; face < 6; ++face)
		{
			for (uint32_t row = 0; row < size; ++row)
			{
				for (uint32_t column = 0; column < size; ++column)
					cube.Faces[face].push_back(radiance(Test::CubeTexelDirection(face, column, row, size)));
			}
		}
		return cube;
	}

	// The largest per-channel difference between two colours.
	static double MaxDifference(const glm::dvec3& left, const glm::dvec3& right)
	{
		const glm::dvec3 difference = glm::abs(left - right);
		return std::max({ difference.x, difference.y, difference.z });
	}

	TEST_SUITE("Support")
	{
		TEST_CASE("RenderReference: the Hammersley sequence is the radical inverse in base 2")
		{
			CHECK(Test::Hammersley(0, 4) == glm::dvec2(0.0, 0.0));
			CHECK(Test::Hammersley(1, 4) == glm::dvec2(0.25, 0.5));
			CHECK(Test::Hammersley(2, 4) == glm::dvec2(0.5, 0.25));
			CHECK(Test::Hammersley(3, 4) == glm::dvec2(0.75, 0.75));
			CHECK(Test::Hammersley(5, 8).y == 0.625); // 101 mirrored is 0.101 in binary
		}

		TEST_CASE("RenderReference: GGX integrates to 1 over the hemisphere's projected area")
		{
			// The integral of D(h) NdotH over the hemisphere is 1 for every alpha (midpoint rule in theta).
			for (const double alpha : { 0.05, 0.25, 0.6, 1.0 })
			{
				CAPTURE(alpha);
				constexpr uint32_t Steps = 20000;
				double integral = 0.0;
				for (uint32_t step = 0; step < Steps; ++step)
				{
					const double theta = (static_cast<double>(step) + 0.5) / Steps * glm::half_pi<double>();
					integral += Test::DistributionGgx(std::cos(theta), alpha) * std::cos(theta) * std::sin(theta) * glm::half_pi<double>() / Steps;
				}
				CHECK(integral * glm::two_pi<double>() == doctest::Approx(1.0).epsilon(1e-3));
			}
			// alpha = 1 is the uniform distribution 1 / pi.
			CHECK(Test::DistributionGgx(0.3, 1.0) == doctest::Approx(1.0 / glm::pi<double>()));
		}

		TEST_CASE("RenderReference: Smith visibility and Schlick's Fresnel match their closed forms")
		{
			// At alpha = 1 the height-correlated visibility is 0.5 / (NdotL + NdotV).
			CHECK(Test::VisibilitySmithGgxCorrelated(0.5, 0.5, 1.0) == doctest::Approx(0.5));
			CHECK(Test::VisibilitySmithGgxCorrelated(0.25, 1.0, 1.0) == doctest::Approx(0.4));
			// At alpha -> 0 it tends to 1 / (4 NdotL NdotV), the visibility of a mirror.
			CHECK(Test::VisibilitySmithGgxCorrelated(0.5, 0.8, 1e-6) == doctest::Approx(1.0 / (4.0 * 0.5 * 0.8)).epsilon(1e-6));
			const glm::dvec3 f0(0.04, 0.5, 1.0);
			CHECK(Test::FresnelSchlick(f0, 1.0) == f0);
			CHECK(Test::FresnelSchlick(f0, 0.0) == glm::dvec3(1.0));
			CHECK(Test::FresnelSchlick(glm::dvec3(0.0), 0.5).x == doctest::Approx(0.03125));
		}

		TEST_CASE("RenderReference: GGX importance samples are unit half vectors with the GGX polar angle")
		{
			// phi = 2 pi xi.x; at alpha = 1, cos(theta) = sqrt(1 - xi.y).
			const glm::dvec3 sample = Test::ImportanceSampleGgx(glm::dvec2(0.25, 0.75), 1.0);
			CHECK(sample.x == doctest::Approx(0.0));
			CHECK(sample.y == doctest::Approx(std::sqrt(0.75)));
			CHECK(sample.z == doctest::Approx(0.5));
			CHECK(Test::ImportanceSampleGgx(glm::dvec2(0.7, 0.0), 0.3) == glm::dvec3(0.0, 0.0, 1.0));
			// The fraction of samples whose half vector lies within theta0 of N is the CDF of D(h) NdotH,
			// tan²(theta0) / (alpha² + tan²(theta0)).
			constexpr uint32_t Count = 4096;
			constexpr double Alpha = 0.4;
			const double cosThreshold = std::cos(0.5);
			uint32_t inside = 0;
			for (uint32_t index = 0; index < Count; ++index)
			{
				const glm::dvec3 half = Test::ImportanceSampleGgx(Test::Hammersley(index, Count), Alpha);
				CHECK(glm::length(half) == doctest::Approx(1.0));
				if (half.z > cosThreshold)
					++inside;
			}
			const double tanSquared = std::tan(0.5) * std::tan(0.5);
			CHECK(static_cast<double>(inside) / Count == doctest::Approx(tanSquared / (Alpha * Alpha + tanSquared)).epsilon(2e-3));
		}

		TEST_CASE("RenderReference: the compensated specular albedo of a white f0 is 1")
		{
			for (const double roughness : { Test::MinPerceptualRoughness, 0.25, 0.5, 0.75, 1.0 })
			{
				for (const double nDotV : { 0.1, 0.5, 1.0 })
				{
					CAPTURE(roughness);
					CAPTURE(nDotV);
					CHECK(Test::ComputeCompensatedSpecularAlbedo(nDotV, roughness, 4096) == doctest::Approx(1.0).epsilon(0.01));
				}
			}
		}

		TEST_CASE("RenderReference: DFG of a smooth surface seen head-on is (0, 1)")
		{
			// Roughness near 0 and NdotV = 1: Fc = 0 and Gv = 1.
			const glm::dvec2 dfg = Test::ComputeDfg(1.0, 0.0, 1024);
			CHECK(dfg.x == doctest::Approx(0.0).epsilon(1e-3));
			CHECK(dfg.y == doctest::Approx(1.0).epsilon(1e-2));
			CHECK(Test::ComputeDfgLut(8, 64).size() == 64);
		}

		TEST_CASE("RenderReference: the DFG LUT holds NdotV along rows and roughness down the columns")
		{
			constexpr uint32_t Size = 4;
			const std::vector<glm::dvec2> lut = Test::ComputeDfgLut(Size, 128);
			REQUIRE(lut.size() == Size * Size);
			for (uint32_t row = 0; row < Size; ++row)
			{
				for (uint32_t column = 0; column < Size; ++column)
				{
					CAPTURE(row);
					CAPTURE(column);
					const glm::dvec2 texel = lut[static_cast<size_t>(row) * Size + column];
					CHECK(texel == Test::ComputeDfg((column + 0.5) / Size, (row + 0.5) / Size, 128));
					// Both terms are albedos, 0 <= DFG1 <= DFG2 <= 1, up to the estimator's noise for DFG2 (a sample's Gv may
					// exceed 1 at low roughness).
					CHECK(texel.x >= 0.0);
					CHECK(texel.x <= texel.y);
					CHECK(texel.y <= 1.001);
				}
			}
			// The single-scatter albedo falls with roughness (energy lost to multiple scattering).
			CHECK(lut[3 * Size + 2].y < lut[0 * Size + 2].y);
		}

		TEST_CASE("RenderReference: cube texel directions follow Vulkan's face selection")
		{
			// The centre of each face points along its major axis.
			constexpr std::array<glm::dvec3, 6> Axes = { glm::dvec3(1, 0, 0), glm::dvec3(-1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, -1, 0),
				glm::dvec3(0, 0, 1), glm::dvec3(0, 0, -1) };
			for (uint32_t face = 0; face < 6; ++face)
			{
				CAPTURE(face);
				const glm::dvec3 centre = glm::normalize(Test::CubeTexelDirection(face, 0, 0, 2) + Test::CubeTexelDirection(face, 1, 1, 2));
				CHECK(glm::length(centre - Axes[face]) < 1e-9);
			}
			// +X: s grows towards -Z, t towards -Y (row 0 is the top).
			CHECK(Test::CubeTexelDirection(0, 0, 0, 2).z > 0.0);
			CHECK(Test::CubeTexelDirection(0, 0, 0, 2).y > 0.0);
			// +Y: s grows towards +X, t towards +Z; -Z: s grows towards -X.
			CHECK(Test::CubeTexelDirection(2, 1, 1, 2).x > 0.0);
			CHECK(Test::CubeTexelDirection(2, 1, 1, 2).z > 0.0);
			CHECK(Test::CubeTexelDirection(5, 1, 0, 2).x < 0.0);
			double total = 0.0;
			for (uint32_t row = 0; row < 16; ++row)
			{
				for (uint32_t column = 0; column < 16; ++column)
					total += Test::CubeTexelSolidAngle(column, row, 16);
			}
			CHECK(total * 6.0 == doctest::Approx(4.0 * glm::pi<double>()).epsilon(1e-9));
			// A centre texel subtends more than a corner texel.
			CHECK(Test::CubeTexelSolidAngle(7, 7, 16) > Test::CubeTexelSolidAngle(0, 0, 16));
		}

		TEST_CASE("RenderReference: the equirectangular mapping puts -Z at the centre and round-trips")
		{
			CHECK(Test::DirectionToEquirectUv(glm::dvec3(0.0, 0.0, -1.0)).x == doctest::Approx(0.5));
			CHECK(Test::DirectionToEquirectUv(glm::dvec3(1.0, 0.0, 0.0)).x == doctest::Approx(0.75));
			CHECK(Test::DirectionToEquirectUv(glm::dvec3(-1.0, 0.0, 0.0)).x == doctest::Approx(0.25));
			CHECK(Test::DirectionToEquirectUv(glm::dvec3(0.0, 1.0, 0.0)).y == doctest::Approx(0.0));
			CHECK(Test::DirectionToEquirectUv(glm::dvec3(0.0, -1.0, 0.0)).y == doctest::Approx(1.0));
			// +Z is the u = 0 / 1 seam; u stays below 1.
			const glm::dvec2 behind = Test::DirectionToEquirectUv(glm::dvec3(0.0, 0.0, 1.0));
			CHECK(behind.x >= 0.0);
			CHECK(behind.x < 1.0);
			const glm::dvec3 direction = glm::normalize(glm::dvec3(0.3, -0.4, 0.5));
			CHECK(glm::length(Test::EquirectUvToDirection(Test::DirectionToEquirectUv(direction)) - direction) < 1e-12);
			CHECK(glm::length(Test::EquirectUvToDirection(glm::dvec2(0.5, 0.5)) - glm::dvec3(0.0, 0.0, -1.0)) < 1e-12);
		}

		TEST_CASE("RenderReference: an equirectangular image resamples into the cube along each texel's direction")
		{
			// A smooth image, 1 + 0.5 d: every cube texel holds it at its own direction, within the bilinear error.
			constexpr uint32_t Width = 256;
			constexpr uint32_t Height = 128;
			std::vector<float> rgb;
			rgb.reserve(static_cast<size_t>(Width) * Height * 3);
			for (uint32_t row = 0; row < Height; ++row)
			{
				for (uint32_t column = 0; column < Width; ++column)
				{
					const glm::dvec3 direction = Test::EquirectUvToDirection(glm::dvec2((column + 0.5) / Width, (row + 0.5) / Height));
					for (glm::length_t channel = 0; channel < 3; ++channel)
						rgb.push_back(static_cast<float>(1.0 + 0.5 * direction[channel]));
				}
			}
			const Test::ReferenceCube cube = Test::EquirectToCube(rgb, Width, Height, 16);
			REQUIRE(cube.Size == 16);
			double worst = 0.0;
			for (uint32_t face = 0; face < 6; ++face)
			{
				REQUIRE(cube.Faces[face].size() == 256);
				for (uint32_t row = 0; row < 16; ++row)
				{
					for (uint32_t column = 0; column < 16; ++column)
					{
						const glm::dvec3 expected = glm::dvec3(1.0) + 0.5 * Test::CubeTexelDirection(face, column, row, 16);
						worst = std::max(worst, MaxDifference(cube.Faces[face][row * 16 + column], expected));
					}
				}
			}
			CHECK(worst < 0.01);
		}

		TEST_CASE("RenderReference: a constant environment prefilters and projects to the constant")
		{
			Test::ReferenceCube constant;
			constant.Size = 8;
			for (std::vector<glm::dvec3>& face : constant.Faces)
				face.assign(64, glm::dvec3(0.5, 1.0, 2.0));
			for (const double roughness : { 0.0, 0.5, 1.0 })
			{
				const Test::ReferenceCube filtered = Test::PrefilterSpecular(constant, roughness, 4);
				REQUIRE(filtered.Size == 4);
				CHECK(glm::length(filtered.Faces[2][5] - glm::dvec3(0.5, 1.0, 2.0)) < 1e-6);
			}
			const std::array<glm::dvec3, 9> sh = Test::ProjectIrradianceSH9(constant);
			for (const glm::dvec3& normal : { glm::dvec3(0, 1, 0), glm::dvec3(1, 0, 0), glm::normalize(glm::dvec3(-1, -2, 3)) })
				CHECK(glm::length(Test::EvaluateIrradianceSH9(sh, normal) - glm::dvec3(0.5, 1.0, 2.0)) < 1e-3);
		}

		TEST_CASE("RenderReference: roughness 0 resamples the source bilinearly within each face")
		{
			// Equal sizes copy; a source of twice the size averages 2x2 blocks (the bake's mip-0 rule).
			const auto gradient = [](const glm::dvec3& direction)
			{
				return glm::dvec3(1.0) + 0.5 * direction;
			};
			const Test::ReferenceCube source = MakeCube(8, gradient);
			const Test::ReferenceCube copy = Test::PrefilterSpecular(source, 0.0, 8);
			CHECK(copy.Faces == source.Faces);
			const Test::ReferenceCube half = Test::PrefilterSpecular(source, 0.0, 4);
			for (uint32_t face = 0; face < 6; ++face)
			{
				for (uint32_t row = 0; row < 4; ++row)
				{
					for (uint32_t column = 0; column < 4; ++column)
					{
						const std::vector<glm::dvec3>& texels = source.Faces[face];
						const size_t topLeft = static_cast<size_t>(2 * row) * 8 + 2 * column;
						const glm::dvec3 box = (texels[topLeft] + texels[topLeft + 1] + texels[topLeft + 8] + texels[topLeft + 9]) * 0.25;
						CHECK(MaxDifference(half.Faces[face][row * 4 + column], box) < 1e-12);
					}
				}
			}
		}

		TEST_CASE("RenderReference: the GGX prefilter blurs a sharp source more at higher roughness")
		{
			// One bright direction (the +Y face) in a dark environment: the -Y face sees none of it at any roughness, and the
			// value at the centre of +Y falls as the lobe widens while the side faces gain.
			const Test::ReferenceCube source = MakeCube(8, [](const glm::dvec3& direction)
			{
				return glm::dvec3(direction.y > 0.9 ? 10.0 : 0.0);
			});
			const Test::ReferenceCube smooth = Test::PrefilterSpecular(source, 0.3, 4);
			const Test::ReferenceCube rough = Test::PrefilterSpecular(source, 1.0, 4);
			const size_t centre = 1 * 4 + 1;
			CHECK(smooth.Faces[2][centre].x > rough.Faces[2][centre].x);
			CHECK(rough.Faces[0][centre].x > smooth.Faces[0][centre].x);
			CHECK(rough.Faces[3][centre].x == 0.0);
		}

		TEST_CASE("RenderReference: SH9 irradiance of linear and quadratic environments matches the analytic convolution")
		{
			// L = a + b y: E(n) / pi = a + (2/3) b n.y, with band 1 windowed by (1 + cos(pi / 4)) / 2.
			const double window1 = 0.5 * (1.0 + std::cos(glm::pi<double>() / 4.0));
			const std::array<glm::dvec3, 9> linear = Test::ProjectIrradianceSH9(MakeCube(32, [](const glm::dvec3& direction)
			{
				return glm::dvec3(1.0 + 0.5 * direction.y);
			}));
			for (const glm::dvec3& normal : { glm::dvec3(0, 1, 0), glm::dvec3(0, -1, 0), glm::normalize(glm::dvec3(1, 1, -2)) })
			{
				const double expected = 1.0 + 2.0 / 3.0 * 0.5 * window1 * normal.y;
				CHECK(Test::EvaluateIrradianceSH9(linear, normal).x == doctest::Approx(expected).epsilon(1e-3));
			}
			// L = x y (pure band 2): E(n) / pi = (1/4) x y, windowed by 0.5.
			const std::array<glm::dvec3, 9> quadratic = Test::ProjectIrradianceSH9(MakeCube(32, [](const glm::dvec3& direction)
			{
				return glm::dvec3(direction.x * direction.y);
			}));
			const glm::dvec3 diagonal = glm::normalize(glm::dvec3(1, 1, 0));
			CHECK(Test::EvaluateIrradianceSH9(quadratic, diagonal).x == doctest::Approx(0.125 * 0.5).epsilon(1e-3));
			CHECK(std::abs(Test::EvaluateIrradianceSH9(quadratic, glm::dvec3(0, 0, 1)).x) < 1e-6);
		}

		TEST_CASE("RenderReference: tonemappers map black to black and stay in [0, 1]")
		{
			const std::vector<glm::dvec3> points = Test::MakeTonemapSamplePoints();
			REQUIRE(points.size() == 1024);
			for (const RenderTonemapper tonemapper : { RenderTonemapper::AgX, RenderTonemapper::Aces, RenderTonemapper::PbrNeutral, RenderTonemapper::Linear })
			{
				CAPTURE(static_cast<int>(tonemapper));
				CHECK(glm::length(Test::ApplyTonemapper(tonemapper, glm::dvec3(0.0))) < 0.02);
				for (const glm::dvec3& point : points)
				{
					const glm::dvec3 mapped = Test::ApplyTonemapper(tonemapper, point);
					CHECK((mapped.x >= 0.0 && mapped.x <= 1.0 && mapped.y >= 0.0 && mapped.y <= 1.0 && mapped.z >= 0.0 && mapped.z <= 1.0));
				}
			}
			CHECK(Test::TonemapLinear(glm::dvec3(2.0, 0.5, -1.0)) == glm::dvec3(1.0, 0.5, 0.0));
			// PBR Neutral leaves values below its compression start (0.8 - 0.04) unchanged apart from the toe offset.
			CHECK(Test::TonemapPbrNeutral(glm::dvec3(0.5)).x == doctest::Approx(0.5).epsilon(0.05));
		}

		TEST_CASE("RenderReference: tonemappers match hand-evaluated values of TonemapPass.h's formulas")
		{
			// Grey 0.18 and 1, and a saturated orange, evaluated independently from the header's text.
			CHECK(MaxDifference(Test::TonemapAgx(glm::dvec3(0.18)), glm::dvec3(0.214467, 0.214533, 0.214537)) < 1e-5);
			CHECK(MaxDifference(Test::TonemapAgx(glm::dvec3(1.0)), glm::dvec3(0.589977, 0.590207, 0.590221)) < 1e-5);
			CHECK(MaxDifference(Test::TonemapAgx(glm::dvec3(1.0, 0.25, 0.05)), glm::dvec3(0.648106, 0.293509, 0.110673)) < 1e-5);
			CHECK(MaxDifference(Test::TonemapAces(glm::dvec3(1.0)), glm::dvec3(0.619115, 0.619115, 0.619109)) < 1e-5);
			CHECK(MaxDifference(Test::TonemapAces(glm::dvec3(0.18)), glm::dvec3(0.105591, 0.105591, 0.105590)) < 1e-5);
			CHECK(MaxDifference(Test::TonemapAces(glm::dvec3(1.0, 0.25, 0.05)), glm::dvec3(0.667225, 0.189719, 0.031106)) < 1e-5);
			// PBR Neutral at 1: offset 0.04, peak 0.96, new peak 1 - 0.0576 / 0.44.
			CHECK(MaxDifference(Test::TonemapPbrNeutral(glm::dvec3(1.0)), glm::dvec3(1.0 - 0.0576 / 0.44)) < 1e-12);
			// Below 0.08 the toe offset is x - 6.25 x², so a grey of 0.04 becomes 0.01.
			CHECK(Test::TonemapPbrNeutral(glm::dvec3(0.04)).x == doctest::Approx(0.01));
			// Each curve rises monotonically along the grey ramp.
			for (const RenderTonemapper tonemapper : { RenderTonemapper::AgX, RenderTonemapper::Aces, RenderTonemapper::PbrNeutral, RenderTonemapper::Linear })
			{
				CAPTURE(static_cast<int>(tonemapper));
				double previous = -1.0;
				bool monotonic = true;
				for (const glm::dvec3& point : Test::MakeTonemapSamplePoints())
				{
					if (point.y != point.x)
						break;
					const double value = Test::ApplyTonemapper(tonemapper, point).x;
					monotonic = monotonic && value >= previous;
					previous = value;
				}
				CHECK(monotonic);
			}
		}

		TEST_CASE("RenderReference: the tonemapper sample points span 2^-12 to 2^12 for four colours")
		{
			const std::vector<glm::dvec3> points = Test::MakeTonemapSamplePoints();
			REQUIRE(points.size() == 1024);
			CHECK(points[0] == glm::dvec3(std::exp2(-12.0)));
			CHECK(points[255] == glm::dvec3(std::exp2(12.0)));
			CHECK(points[256] == glm::dvec3(1.0, 0.25, 0.05) * std::exp2(-12.0));
			CHECK(points[512 + 255] == glm::dvec3(0.05, 1.0, 0.25) * std::exp2(12.0));
			CHECK(points[1023] == glm::dvec3(0.25, 0.05, 1.0) * std::exp2(12.0));
		}

		TEST_CASE("RenderReference: the sRGB OETF and binary16 conversions are exact at their reference points")
		{
			CHECK(Test::LinearToSrgb(0.0) == 0.0);
			CHECK(Test::LinearToSrgb(1.0) == doctest::Approx(1.0));
			CHECK(Test::LinearToSrgb(0.0031308) == doctest::Approx(0.0404500).epsilon(1e-5));
			CHECK(Test::SrgbToLinear(Test::LinearToSrgb(0.18)) == doctest::Approx(0.18));
			CHECK(Test::HalfToDouble(0x3C00) == 1.0);
			CHECK(Test::HalfToDouble(0xC000) == -2.0);
			CHECK(Test::DoubleToHalf(1.0) == 0x3C00);
			CHECK(Test::DoubleToHalf(65504.0) == 0x7BFF);
		}

		TEST_CASE("RenderReference: binary16 conversions round to nearest even and round-trip every half")
		{
			for (uint32_t bits = 0; bits <= 0xFFFFU; ++bits)
			{
				const uint16_t half = static_cast<uint16_t>(bits);
				const double value = Test::HalfToDouble(half);
				if (std::isnan(value))
				{
					CHECK((half & 0x7C00U) == 0x7C00U);
					continue;
				}
				CAPTURE(bits);
				CHECK(Test::DoubleToHalf(value) == half);
			}
			// Ties go to the even significand.
			CHECK(Test::DoubleToHalf(1.0 + std::ldexp(1.0, -11)) == 0x3C00);
			CHECK(Test::DoubleToHalf(1.0 + 3.0 * std::ldexp(1.0, -11)) == 0x3C02);
			CHECK(Test::DoubleToHalf(1.0 + 1.5 * std::ldexp(1.0, -11)) == 0x3C01);
			// Subnormals, the smallest normal, and underflow (2^-25 is the tie between 0 and 2^-24).
			CHECK(Test::DoubleToHalf(std::ldexp(1.0, -24)) == 0x0001);
			CHECK(Test::DoubleToHalf(std::ldexp(1.0, -25)) == 0x0000);
			CHECK(Test::DoubleToHalf(std::ldexp(1.0, -25) * 1.5) == 0x0001);
			CHECK(Test::DoubleToHalf(std::ldexp(1.0, -14)) == 0x0400);
			CHECK(Test::DoubleToHalf(std::ldexp(1023.75, -24)) == 0x0400);
			// Overflow, infinities, NaN and signed zero.
			CHECK(Test::DoubleToHalf(65519.0) == 0x7BFF);
			CHECK(Test::DoubleToHalf(65520.0) == 0x7C00);
			CHECK(Test::DoubleToHalf(-std::numeric_limits<double>::infinity()) == 0xFC00);
			CHECK((Test::DoubleToHalf(std::numeric_limits<double>::quiet_NaN()) & 0x7FFFU) == 0x7E00);
			CHECK(Test::DoubleToHalf(-0.0) == 0x8000);
			CHECK(Test::HalfToDouble(0x7C00) == std::numeric_limits<double>::infinity());
			CHECK(std::isnan(Test::HalfToDouble(0x7E00)));
		}
	}

}
