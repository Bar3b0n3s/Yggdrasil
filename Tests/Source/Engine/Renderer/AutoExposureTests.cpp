#include "TestsPCH.h"

#include "Engine/Renderer/AutoExposure.h"

#include <cstdint>
#include <numeric>
#include <vector>

// Histogram auto-exposure's CPU statement (M26, C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decision 15, stream X-A):
// the bins the GPU histogram must equal and the temporal rule of the adapted exposure, skipped until stream X-A implements
// them. The GPU half of the acceptance, "ExposurePass: the luminance histogram equals the CPU bins", is stream X-A's GPU case.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("AutoExposure: luminance falls in its bin, black in the first and the brightest in the last" * doctest::skip(true))
		{
			CHECK(GetLuminanceHistogramBin(glm::vec3(0.0f)) == 0);
			CHECK(GetLuminanceHistogramBin(glm::vec3(1.0e9f)) == ExposureHistogramBins - 1);
			// Luminance 1 (log2 0) is 12 EV above the range's start: with 64 bins over 32 EV, two bins an EV, bin 24.
			CHECK(GetLuminanceHistogramBin(glm::vec3(1.0f)) == 24);
			const std::vector<glm::vec4> pixels = { glm::vec4(0.0f), glm::vec4(1.0f), glm::vec4(1.0f), glm::vec4(4.0f, 4.0f, 4.0f, 0.0f) };
			const ExposureHistogram histogram = ComputeLuminanceHistogram(pixels);
			CHECK(std::accumulate(histogram.begin(), histogram.end(), uint32_t{ 0 }) == 4);
			CHECK(histogram[0] == 1);
			CHECK(histogram[24] == 2);
			CHECK(histogram[28] == 1);
		}

		TEST_CASE("AutoExposure: the target maps the metered luminance to middle grey, and adaptation converges at once after a reset" * doctest::skip(true))
		{
			PostProcessSettings post;
			post.ExposureMode = RenderExposureMode::Auto;
			// Every pixel at luminance 0.18 needs no exposure.
			ExposureHistogram grey{};
			grey[GetLuminanceHistogramBin(glm::vec3(0.18f))] = 100;
			const float neutral = ComputeExposureTarget(grey, post);
			CHECK(neutral == doctest::Approx(0.0f).epsilon(0.5));
			// A brighter scene needs less exposure, clamped to the range.
			ExposureHistogram bright{};
			bright[ExposureHistogramBins - 1] = 100;
			CHECK(ComputeExposureTarget(bright, post) == post.AutoExposureMinEV);
			CHECK(ComputeExposureTarget(ExposureHistogram{}, post) == 0.0f);
			// After a reset the adapted exposure is the target at once; otherwise it moves at its speeds.
			CHECK(AdaptExposure(0.0f, 4.0f, 0.016f, true, post) == 4.0f);
			CHECK(AdaptExposure(0.0f, 4.0f, 0.5f, false, post) == doctest::Approx(post.AutoExposureSpeedUp * 0.5f));
			CHECK(AdaptExposure(0.0f, -4.0f, 0.5f, false, post) == doctest::Approx(-post.AutoExposureSpeedDown * 0.5f));
			CHECK(AdaptExposure(3.9f, 4.0f, 1.0f, false, post) == 4.0f);
			CHECK(AdaptExposure(1.0f, 4.0f, 0.0f, false, post) == 1.0f);
		}
	}

}
