#include "TestsPCH.h"

#include "Support/GoldenImage.h"

#include "Support/TestOptions.h"

// The golden harness (Architecture §15.4) on CPU images and a device class no machine has, so the checks never read or
// write the committed goldens.

namespace Engine {

	static Image MakeCheckerboard(uint32_t size)
	{
		Image image{ .Width = size, .Height = size, .Format = nvrhi::Format::RGBA8_UNORM };
		image.Pixels.resize(static_cast<size_t>(size) * size * 4);
		for (uint32_t y = 0; y < size; ++y)
		{
			for (uint32_t x = 0; x < size; ++x)
			{
				const std::byte value = ((x / 4 + y / 4) % 2) == 0 ? std::byte{ 40 } : std::byte{ 220 };
				const size_t offset = (static_cast<size_t>(y) * size + x) * 4;
				image.Pixels[offset + 0] = value;
				image.Pixels[offset + 1] = value;
				image.Pixels[offset + 2] = value;
				image.Pixels[offset + 3] = std::byte{ 255 };
			}
		}
		return image;
	}

	TEST_SUITE("Support")
	{
		TEST_CASE("GoldenImage: the golden and output directories follow the repository layout" * doctest::skip(true))
		{
			const std::filesystem::path golden = Test::GetGoldenDirectory("nvidia-58x");
			CHECK(golden.filename() == "nvidia-58x");
			CHECK(golden.parent_path().filename() == "Golden");
			CHECK(golden.parent_path().parent_path().filename() == "Tests");
			const std::filesystem::path output = Test::GetGoldenOutputDirectory();
			CHECK(output.filename() == "Golden");
			CHECK(output.parent_path().filename() == "TestResults");
		}

		TEST_CASE("GoldenImage: without goldens for the device class the smoke checks run" * doctest::skip(true))
		{
			// A device class that has no golden directory.
			const std::string deviceClass = "vendorffff-0";
			if (Test::GetTestOptions().UpdateGolden)
			{
				MESSAGE("skipped under --update-golden, which would write into Tests/Golden");
				return;
			}
			const Test::GoldenCheckResult smoke = Test::CheckGoldenImage("Checkerboard", MakeCheckerboard(32), deviceClass);
			CHECK(smoke.Outcome == Test::GoldenOutcome::SmokePassed);
			CHECK(smoke.Message.contains("goldens missing"));

			// A flat black image fails the smoke checks: one colour, and luminance outside [0.01, 0.99].
			Image black = MakeCheckerboard(32);
			std::ranges::fill(black.Pixels, std::byte{ 0 });
			const Test::GoldenCheckResult failed = Test::CheckGoldenImage("Black", black, deviceClass);
			CHECK(failed.Outcome == Test::GoldenOutcome::SmokeFailed);
		}

		TEST_CASE("GoldenImage: outcome names are their enumerator names")
		{
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::Matched) == "Matched");
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::SmokePassed) == "SmokePassed");
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::Error) == "Error");
		}
	}

}
