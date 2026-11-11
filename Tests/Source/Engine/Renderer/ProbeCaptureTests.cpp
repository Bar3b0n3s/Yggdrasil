#include "TestsPCH.h"

#include "Engine/Renderer/ProbeCapture.h"

#include "Support/RenderReference.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

// Probe captures' CPU steps (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A): the furnace
// and colour-bleed oracles of an irradiance probe's SH9 projection, against the test support's double-precision reference
// (Test::ProjectIrradianceSH9), and the reflection probe's deterministic HDR image. Skipped until stream Q-A implements them;
// the GPU halves (the capture, the bake's byte-identical files and the shading) are streams Q-A's and Q-B's GPU cases.

namespace Engine {

	namespace {

		// A capture of `size` texels a face whose texel in direction d has radiance `radiance(d)`.
		template<typename Radiance>
		ProbeCubeCapture MakeCapture(uint32_t size, Radiance radiance)
		{
			ProbeCubeCapture capture;
			capture.FaceSize = size;
			for (uint32_t face = 0; face < 6; ++face)
			{
				for (uint32_t row = 0; row < size; ++row)
				{
					for (uint32_t column = 0; column < size; ++column)
						capture.Faces[face].push_back(glm::vec4(radiance(Test::CubeTexelDirection(face, column, row, size)), 1.0f));
				}
			}
			return capture;
		}

		Test::ReferenceCube ToReference(const ProbeCubeCapture& capture)
		{
			Test::ReferenceCube cube;
			cube.Size = capture.FaceSize;
			for (uint32_t face = 0; face < 6; ++face)
			{
				for (const glm::vec4& texel : capture.Faces[face])
					cube.Faces[face].push_back(glm::dvec3(texel));
			}
			return cube;
		}

	}

	TEST_SUITE("Renderer")
	{
		// The furnace: a probe inside uniform radiance L sees irradiance pi L from every side, so its coefficients evaluate to L
		// in every direction (EnvironmentData's convention), and a white Lambertian surface lit by it reflects L.
		TEST_CASE("ProbeCapture: a uniform cube projects to its radiance in every direction (the furnace)" * doctest::skip(true))
		{
			const glm::vec3 radiance(0.75f, 1.5f, 3.0f);
			const ProbeCubeCapture capture = MakeCapture(16, [&radiance](const glm::dvec3& /*direction*/)
			{
				return glm::vec3(radiance);
			});
			const std::array<glm::vec3, 9> sh = ProjectCubeToIrradianceSH9(capture);
			std::array<glm::dvec3, 9> coefficients{};
			for (size_t k = 0; k < 9; ++k)
				coefficients[k] = glm::dvec3(sh[k]);
			for (const glm::dvec3 normal : { glm::dvec3(1, 0, 0), glm::dvec3(0, -1, 0), glm::dvec3(0, 0, 1), glm::normalize(glm::dvec3(1, 1, -1)) })
			{
				CAPTURE(normal.x);
				const glm::dvec3 value = Test::EvaluateIrradianceSH9(coefficients, normal);
				CHECK(value.x == doctest::Approx(radiance.x).epsilon(1e-4));
				CHECK(value.y == doctest::Approx(radiance.y).epsilon(1e-4));
				CHECK(value.z == doctest::Approx(radiance.z).epsilon(1e-4));
			}
		}

		// Colour bleed: a probe beside a red wall (the +X hemisphere red, the rest black) sees red light from +X and none from
		// -X: its coefficients equal the reference projection, and evaluate to red facing the wall and to black facing away
		// within the SH9 window's ringing.
		TEST_CASE("ProbeCapture: a cube lit red on one side projects to the reference irradiance of the red hemisphere (colour bleed)" * doctest::skip(true))
		{
			const ProbeCubeCapture capture = MakeCapture(32, [](const glm::dvec3& direction)
			{
				return direction.x > 0.0 ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f);
			});
			const std::array<glm::vec3, 9> sh = ProjectCubeToIrradianceSH9(capture);
			const std::array<glm::dvec3, 9> reference = Test::ProjectIrradianceSH9(ToReference(capture));
			for (size_t k = 0; k < 9; ++k)
			{
				CAPTURE(k);
				CHECK(sh[k].x == doctest::Approx(reference[k].x).epsilon(1e-5));
				CHECK(sh[k].y == doctest::Approx(reference[k].y).epsilon(1e-5));
				CHECK(sh[k].z == doctest::Approx(reference[k].z).epsilon(1e-5));
			}
			std::array<glm::dvec3, 9> coefficients{};
			for (size_t k = 0; k < 9; ++k)
				coefficients[k] = glm::dvec3(sh[k]);
			CHECK(Test::EvaluateIrradianceSH9(coefficients, glm::dvec3(1, 0, 0)).x > 0.8);
			CHECK(Test::EvaluateIrradianceSH9(coefficients, glm::dvec3(-1, 0, 0)).x < 0.1);
			CHECK(Test::EvaluateIrradianceSH9(coefficients, glm::dvec3(1, 0, 0)).y == doctest::Approx(0.0));
		}

		TEST_CASE("ProbeCapture: a reflection probe's HDR image is the cube resampled, written byte-identically every time" * doctest::skip(true))
		{
			const ProbeCubeCapture capture = MakeCapture(8, [](const glm::dvec3& direction)
			{
				return glm::vec3(glm::abs(direction));
			});
			const ProbeEquirectImage image = ResampleCubeToEquirect(capture);
			CHECK(image.Width == 32);
			CHECK(image.Height == 16);
			REQUIRE(image.Texels.size() == 32u * 16u);
			const Buffer first = WriteRadianceHdr(image);
			const Buffer second = WriteRadianceHdr(ResampleCubeToEquirect(capture));
			REQUIRE(first.size() > 32u * 16u * 4u);
			CHECK(first == second);
			const std::string_view header(reinterpret_cast<const char*>(first.data()), 10);
			CHECK(header == "#?RADIANCE");
		}
	}

}
