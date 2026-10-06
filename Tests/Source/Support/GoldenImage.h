#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Testing/ImageCompare.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Golden images (Architecture §15.4). Goldens live in Tests/Golden/<DeviceClass>/<Name>.png, where the device class is
// GetDeviceClass(vendor, driver version, driver ID) of the device that rendered (GraphicsDeviceInfo::DeviceClass, e.g. "nvidia-58x"); there
// is no cross-device comparison and no software-rasterizer golden. Golden test cases carry
// doctest::test_suite(Test::GoldenSuite) (HeadlessGpuFixture.h) and run in the golden stage (Release, §15.8).

namespace Engine {

	namespace Test {

		// <repo>/Tests/Golden/<deviceClass>.
		[[nodiscard]] std::filesystem::path GetGoldenDirectory(std::string_view deviceClass);
		// <repo>/bin/TestResults/Golden: where a failed comparison writes <Name>-actual.png, <Name>-expected.png and
		// <Name>-diff.png (ImageCompareResult::Difference).
		[[nodiscard]] std::filesystem::path GetGoldenOutputDirectory();

		enum class GoldenOutcome : uint8_t
		{
			Matched,     // the comparison passed
			Mismatched,  // the comparison failed; actual, expected and diff were written
			Updated,     // --update-golden: the image was written as the golden candidate (review before commit)
			SmokePassed, // no golden for the device class: the smoke checks passed ("goldens missing" warning)
			SmokeFailed, // no golden for the device class, and the image failed the smoke checks
			Error        // the golden could not be read or decoded, or an output could not be written
		};

		struct GoldenCheckResult
		{
			GoldenOutcome Outcome = GoldenOutcome::Error;
			// One line for the test output: the comparison summary, the written paths, or the smoke statistics.
			std::string Message{};
		};

		// The golden check of image `name` (a file stem such as "Triangle") rendered on `deviceClass`:
		//   - with --update-golden (TestOptions::UpdateGolden): writes `actual` as GetGoldenDirectory(deviceClass)/<name>.png
		//     (Updated);
		//   - when that golden exists: CompareImages with `thresholds` (§15.4 defaults); on a mismatch writes the three PNGs
		//     into GetGoldenOutputDirectory() (Mismatched);
		//   - otherwise the smoke mode: the image is valid and non-empty, has at least 2 distinct colours and a mean
		//     luminance in [0.01, 0.99] (SmokePassed or SmokeFailed), never a pass of the comparison.
		[[nodiscard]] GoldenCheckResult CheckGoldenImage(std::string_view name, const Image& actual, std::string_view deviceClass,
			const ImageCompareThresholds& thresholds = {});

		// "Matched", "Mismatched", ...
		[[nodiscard]] std::string_view GoldenOutcomeToString(GoldenOutcome outcome);

	}

}

// The standard expectation of a golden image: CheckGoldenImage passes when the outcome is Matched, Updated or SmokePassed
// (the latter two also WARN with the message, so a run without goldens or one that rewrote them is visible) and fails
// with the message otherwise.
#define ENGINE_CHECK_GOLDEN(name, image, deviceClass) \
	do \
	{ \
		const ::Engine::Test::GoldenCheckResult engineGoldenResult = ::Engine::Test::CheckGoldenImage(name, image, deviceClass); \
		const bool engineGoldenPassed = engineGoldenResult.Outcome == ::Engine::Test::GoldenOutcome::Matched \
			|| engineGoldenResult.Outcome == ::Engine::Test::GoldenOutcome::Updated \
			|| engineGoldenResult.Outcome == ::Engine::Test::GoldenOutcome::SmokePassed; \
		if (engineGoldenResult.Outcome != ::Engine::Test::GoldenOutcome::Matched) \
			WARN_MESSAGE(engineGoldenPassed, engineGoldenResult.Message); \
		CHECK_MESSAGE(engineGoldenPassed, engineGoldenResult.Message); \
	} while (false)
