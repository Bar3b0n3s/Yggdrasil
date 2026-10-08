#include "TestsPCH.h"
#include "Support/RenderReference.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

// The CPU references checked against analytic values (stream E, Docs/Decisions/0013-m8-decisions.md decision 14), so the GPU
// oracle tests compare against something known to be right.

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("RenderReference: the Hammersley sequence is the radical inverse in base 2" * doctest::skip(true))
		{
			CHECK(Test::Hammersley(0, 4) == glm::dvec2(0.0, 0.0));
			CHECK(Test::Hammersley(1, 4) == glm::dvec2(0.25, 0.5));
			CHECK(Test::Hammersley(2, 4) == glm::dvec2(0.5, 0.25));
			CHECK(Test::Hammersley(3, 4) == glm::dvec2(0.75, 0.75));
		}

		TEST_CASE("RenderReference: GGX integrates to 1 over the hemisphere's projected area" * doctest::skip(true))
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
		}

		TEST_CASE("RenderReference: the compensated specular albedo of a white f0 is 1" * doctest::skip(true))
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

		TEST_CASE("RenderReference: DFG of a smooth surface seen head-on is (0, 1)" * doctest::skip(true))
		{
			// Roughness near 0 and NdotV = 1: Fc = 0 and Gv = 1.
			const glm::dvec2 dfg = Test::ComputeDfg(1.0, 0.0, 1024);
			CHECK(dfg.x == doctest::Approx(0.0).epsilon(1e-3));
			CHECK(dfg.y == doctest::Approx(1.0).epsilon(1e-2));
			CHECK(Test::ComputeDfgLut(8, 64).size() == 64);
		}

		TEST_CASE("RenderReference: cube texel directions follow Vulkan's face selection" * doctest::skip(true))
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
			double total = 0.0;
			for (uint32_t row = 0; row < 16; ++row)
			{
				for (uint32_t column = 0; column < 16; ++column)
					total += Test::CubeTexelSolidAngle(column, row, 16);
			}
			CHECK(total * 6.0 == doctest::Approx(4.0 * glm::pi<double>()).epsilon(1e-9));
		}

		TEST_CASE("RenderReference: the equirectangular mapping puts -Z at the centre and round-trips" * doctest::skip(true))
		{
			CHECK(Test::DirectionToEquirectUv(glm::dvec3(0.0, 0.0, -1.0)).x == doctest::Approx(0.5));
			CHECK(Test::DirectionToEquirectUv(glm::dvec3(1.0, 0.0, 0.0)).x == doctest::Approx(0.75));
			CHECK(Test::DirectionToEquirectUv(glm::dvec3(0.0, 1.0, 0.0)).y == doctest::Approx(0.0));
			const glm::dvec3 direction = glm::normalize(glm::dvec3(0.3, -0.4, 0.5));
			CHECK(glm::length(Test::EquirectUvToDirection(Test::DirectionToEquirectUv(direction)) - direction) < 1e-12);
		}

		TEST_CASE("RenderReference: a constant environment prefilters and projects to the constant" * doctest::skip(true))
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

		TEST_CASE("RenderReference: tonemappers map black to black and stay in [0, 1]" * doctest::skip(true))
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

		TEST_CASE("RenderReference: the sRGB OETF and binary16 conversions are exact at their reference points" * doctest::skip(true))
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
	}

}
