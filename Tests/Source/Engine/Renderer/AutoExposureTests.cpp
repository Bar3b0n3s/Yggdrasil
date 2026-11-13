#include "TestsPCH.h"

#include "Engine/Renderer/AutoExposure.h"

#include "Shared/ExposureConstants.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

// Histogram auto-exposure's CPU statement (M26, C26.1, R-AEX; Docs/Decisions/0037-m26-contract.md decision 15, stream X-A):
// the bins the GPU histogram must equal and the temporal rule of the adapted exposure. The GPU half of the acceptance,
// "ExposurePass: the luminance histogram equals the CPU bins", is in ExposurePassTests.cpp.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("AutoExposure: luminance falls in its bin, black in the first and the brightest in the last")
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

		TEST_CASE("AutoExposure: the target maps the metered luminance to middle grey, and adaptation converges at once after a reset")
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

		// Shared/ExposureConstants.h's numbers, which the shader reads as the same bits: the edges are 2^(-12 + i / 2) rounded
		// to float, the weights Rec. 709's and middle grey's log2 log2(0.18).
		TEST_CASE("AutoExposure: the shared bin edges, luminance weights and middle grey are the nearest floats of their values")
		{
			for (uint32_t bin = 0; bin < ExposureHistogramBins; ++bin)
			{
				CAPTURE(bin);
				const double exact = std::exp2(static_cast<double>(ExposureHistogramMinLog2) + 0.5 * bin);
				CHECK(std::bit_cast<float>(ExposureHistogramEdgeBits[bin]) == static_cast<float>(exact));
			}
			CHECK(std::bit_cast<float>(ExposureLuminanceWeightBits[0]) == 0.2126f);
			CHECK(std::bit_cast<float>(ExposureLuminanceWeightBits[1]) == 0.7152f);
			CHECK(std::bit_cast<float>(ExposureLuminanceWeightBits[2]) == 0.0722f);
			CHECK(std::bit_cast<float>(ExposureMiddleGreyLog2Bits) == static_cast<float>(std::log2(static_cast<double>(ExposureMiddleGrey))));
		}

		// A bin opens at its lower edge: the first float at the edge counts in it, the float below in the bin before; values
		// below the second edge, negative ones and non-finite ones count in the first bin.
		TEST_CASE("AutoExposure: a luminance on a bin's lower edge falls in that bin, and non-finite or negative luminance in the first")
		{
			// A pixel of radiance (0, 0, b) has the luminance 0.0722 b, so the edge is reached by the b whose product rounds onto
			// it; the test searches the floats around edge / 0.0722 for the first whose luminance is at least the edge.
			const float blueWeight = std::bit_cast<float>(ExposureLuminanceWeightBits[2]);
			for (uint32_t bin = 1; bin < ExposureHistogramBins; ++bin)
			{
				CAPTURE(bin);
				const float edge = std::bit_cast<float>(ExposureHistogramEdgeBits[bin]);
				float blue = edge / blueWeight;
				while (blueWeight * blue >= edge)
					blue = std::nextafter(blue, 0.0f);
				while (blueWeight * blue < edge)
					blue = std::nextafter(blue, std::numeric_limits<float>::infinity());
				CHECK(GetLuminanceHistogramBin(glm::vec3(0.0f, 0.0f, blue)) == bin);
				CHECK(GetLuminanceHistogramBin(glm::vec3(0.0f, 0.0f, std::nextafter(blue, 0.0f))) == bin - 1);
			}
			const float infinity = std::numeric_limits<float>::infinity();
			CHECK(GetLuminanceHistogramBin(glm::vec3(-5.0f, 1.0f, 1.0f)) == 0);
			CHECK(GetLuminanceHistogramBin(glm::vec3(std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f)) == 0);
			CHECK(GetLuminanceHistogramBin(glm::vec3(infinity, 1.0f, 1.0f)) == 0);
			CHECK(GetLuminanceHistogramBin(glm::vec3(1.0f, -infinity, 1.0f)) == 0);
			CHECK(GetLuminanceHistogramBin(glm::vec3(std::exp2(-11.6f))) == 0);
		}

		// The metered window is the pixels between the 50th and the 95th percentile from the darkest, each at its bin's centre,
		// so the darkest half and the brightest twentieth do not move the target; ExposureEV is the tonemap's compensation and
		// never enters the target.
		TEST_CASE("AutoExposure: the target meters the pixels between the 50th and 95th percentiles at their bins' centres")
		{
			PostProcessSettings post;
			const auto centre = [](uint32_t bin)
			{
				return ExposureHistogramMinLog2 + (static_cast<float>(bin) + 0.5f) * 0.5f;
			};
			const float middleGrey = std::bit_cast<float>(ExposureMiddleGreyLog2Bits);
			// Half the pixels very dark: the window [50, 95) holds only bin 24's.
			ExposureHistogram halfDark{};
			halfDark[4] = 50;
			halfDark[24] = 50;
			CHECK(ComputeExposureTarget(halfDark, post) == middleGrey - centre(24));
			// The brightest pixels beyond the 95th percentile: of bin 60's ten, only the five of ranks 90 to 94 count.
			ExposureHistogram highlights{};
			highlights[24] = 90;
			highlights[60] = 10;
			const float mean = (40.0f * centre(24) + 5.0f * centre(60)) / 45.0f;
			CHECK(ComputeExposureTarget(highlights, post) == doctest::Approx(middleGrey - mean).epsilon(1e-6));
			// One pixel meters itself.
			ExposureHistogram single{};
			single[30] = 1;
			CHECK(ComputeExposureTarget(single, post) == middleGrey - centre(30));
			// The compensation is not the target's.
			PostProcessSettings compensated = post;
			compensated.ExposureEV = 3.0f;
			CHECK(ComputeExposureTarget(highlights, compensated) == ComputeExposureTarget(highlights, post));
			// A dark scene's target is clamped to the range's top.
			ExposureHistogram dark{};
			dark[0] = 10;
			CHECK(ComputeExposureTarget(dark, post) == middleGrey - centre(0));
			post.AutoExposureMaxEV = 4.0f;
			CHECK(ComputeExposureTarget(dark, post) == 4.0f);
		}

		// The adaptation never overshoots, moves at SpeedDown towards a darker target, and converges at once from a previous
		// exposure that is not a number; a negative or non-finite delta time or speed moves nothing.
		TEST_CASE("AutoExposure: adaptation stops at the target, and a non-finite previous exposure converges at once")
		{
			PostProcessSettings post;
			post.AutoExposureSpeedUp = 2.0f;
			post.AutoExposureSpeedDown = 0.5f;
			CHECK(AdaptExposure(1.0f, 2.0f, 0.25f, false, post) == 1.5f);
			CHECK(AdaptExposure(1.0f, 2.0f, 10.0f, false, post) == 2.0f);
			CHECK(AdaptExposure(1.0f, -2.0f, 1.0f, false, post) == 0.5f);
			CHECK(AdaptExposure(1.0f, -2.0f, 100.0f, false, post) == -2.0f);
			CHECK(AdaptExposure(std::numeric_limits<float>::quiet_NaN(), -2.0f, 0.1f, false, post) == -2.0f);
			CHECK(AdaptExposure(std::numeric_limits<float>::infinity(), 3.0f, 0.1f, false, post) == 3.0f);
			CHECK(AdaptExposure(1.0f, 2.0f, -1.0f, false, post) == 1.0f);
			CHECK(AdaptExposure(1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN(), false, post) == 1.0f);
			post.AutoExposureSpeedUp = -1.0f;
			CHECK(AdaptExposure(1.0f, 2.0f, 1.0f, false, post) == 1.0f);
		}
	}

}
