#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

#include <cstdint>
#include <string>

// Image comparison for golden images and screenshot checks (Architecture §15.4; M7's
// test_exported_screenshot_matches_editor). Comparison is per channel on RGBA8: a pixel differs when any channel differs
// by more than PixelTolerance. A comparison fails when more than MaxDifferingFraction of the pixels differ, or any channel
// of any pixel differs by more than MaxChannelDifference (§15.4 defaults: 0.1 %, 2/255, 24/255; tunable per test).
// Deterministic and CPU only.

namespace Engine {

	struct ImageCompareThresholds
	{
		double MaxDifferingFraction = 0.001; // fraction of all pixels allowed to differ by more than PixelTolerance
		uint8_t PixelTolerance = 2;          // per-channel difference that does not count as differing
		uint8_t MaxChannelDifference = 24;   // no channel of any pixel may differ by more
	};

	struct ImageCompareResult
	{
		bool Passed = false;
		uint64_t PixelCount = 0;
		uint64_t DifferingPixels = 0; // pixels with a channel difference above PixelTolerance
		double DifferingFraction = 0.0;
		uint8_t LargestDifference = 0; // the largest channel difference of any pixel
		// A visualization of the differences, as large as the inputs, RGBA8: black where equal, the channel differences
		// scaled up (saturating) where they differ, and red where a pixel exceeds MaxChannelDifference. Written as the
		// "diff" PNG of a failed golden test.
		Image Difference{};
		// One line for test output: "passed" or "failed: 1234 of 230400 pixels (0.54 %) differ by more than 2/255; largest
		// difference 37/255".
		std::string Summary{};
	};

	// Compares `actual` with `expected`, both converted to RGBA8 first (ConvertToRgba8). Images of different sizes do not
	// pass: the result has Passed false, no Difference image and a Summary naming both sizes. Errors: InvalidArgument when
	// an image is invalid or its format cannot be converted to RGBA8.
	[[nodiscard]] Result<ImageCompareResult> CompareImages(const Image& actual, const Image& expected,
		const ImageCompareThresholds& thresholds = {});

	// The loose statistics of the smoke mode of golden tests (§15.4): on a device class without goldens a golden test
	// checks that the image rendered at all instead of comparing it.
	struct ImageStatistics
	{
		double MeanLuminance = 0.0; // Rec. 709 luma of the encoded RGB, in [0, 1]
		double MinLuminance = 0.0;
		double MaxLuminance = 0.0;
		uint64_t DistinctColors = 0; // number of distinct RGBA values, capped at 65536
	};

	// The statistics of `image` after ConvertToRgba8. Errors: as ConvertToRgba8.
	[[nodiscard]] Result<ImageStatistics> ComputeImageStatistics(const Image& image);

}
