#include "TestsPCH.h"

#include "Engine/Testing/ImageCompare.h"

namespace Engine {

	static Image MakeUniform(uint32_t width, uint32_t height, uint8_t value)
	{
		return Image{
			.Width = width,
			.Height = height,
			.Format = nvrhi::Format::RGBA8_UNORM,
			.Pixels = std::vector<std::byte>(static_cast<size_t>(width) * height * 4, static_cast<std::byte>(value)),
		};
	}

	// Sets the red channel of `count` pixels of `image`, from the first one on, to `value`.
	static void SetRed(Image& image, size_t count, uint8_t value)
	{
		for (size_t pixel = 0; pixel < count; ++pixel)
			image.Pixels[pixel * 4] = static_cast<std::byte>(value);
	}

	TEST_SUITE("Testing")
	{
		TEST_CASE("ImageCompare: thresholds of the golden comparison" * doctest::skip(true))
		{
			// 100 x 100 pixels: 0.1 % is 10 pixels (§15.4: fail above 0.1 % beyond 2/255, or any pixel beyond 24/255).
			const Image expected = MakeUniform(100, 100, 100);
			struct Row
			{
				std::string Description{};
				size_t Pixels = 0;
				uint8_t Value = 100;
				bool Passes = true;
			};
			const std::vector<Row> rows = {
				{ "identical", 0, 100, true },
				{ "every pixel off by 2", 10000, 102, true },
				{ "10 pixels off by 3", 10, 103, true },
				{ "11 pixels off by 3", 11, 103, false },
				{ "one pixel off by 24", 1, 124, true },
				{ "one pixel off by 25", 1, 125, false },
			};
			for (const Row& row : rows)
			{
				CAPTURE(row.Description);
				Image actual = expected;
				SetRed(actual, row.Pixels, row.Value);
				const Result<ImageCompareResult> result = CompareImages(actual, expected);
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				CHECK(result->Passed == row.Passes);
				CHECK(result->PixelCount == 10000);
				CHECK(result->Difference.Width == 100);
				CHECK(result->Difference.IsValid());
				CHECK_FALSE(result->Summary.empty());
			}

			// Tunable per test.
			Image actual = expected;
			SetRed(actual, 50, 110);
			const Result<ImageCompareResult> loose = CompareImages(actual, expected,
				{ .MaxDifferingFraction = 0.01, .PixelTolerance = 2, .MaxChannelDifference = 24 });
			REQUIRE(loose.has_value());
			CHECK(loose->Passed);
			CHECK(loose->DifferingPixels == 50);
			CHECK(loose->LargestDifference == 10);
		}

		TEST_CASE("ImageCompare: images of different sizes never pass" * doctest::skip(true))
		{
			const Result<ImageCompareResult> result = CompareImages(MakeUniform(10, 10, 0), MakeUniform(10, 11, 0));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_FALSE(result->Passed);
			CHECK(result->Summary.contains("10x10"));
			CHECK(result->Summary.contains("10x11"));

			const Image invalid{ .Width = 2, .Height = 2, .Format = nvrhi::Format::RGBA8_UNORM };
			const Result<ImageCompareResult> rejected = CompareImages(invalid, MakeUniform(2, 2, 0));
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("ImageCompare: smoke statistics describe the luminance and the colours" * doctest::skip(true))
		{
			const Result<ImageStatistics> black = ComputeImageStatistics(MakeUniform(4, 4, 0));
			REQUIRE_MESSAGE(black.has_value(), black.error().ToString());
			CHECK(black->MeanLuminance == doctest::Approx(0.0));
			CHECK(black->DistinctColors == 1);

			Image halves = MakeUniform(4, 4, 255);
			for (size_t pixel = 0; pixel < 8; ++pixel)
			{
				for (size_t channel = 0; channel < 3; ++channel)
					halves.Pixels[pixel * 4 + channel] = std::byte{ 0 };
			}
			const Result<ImageStatistics> statistics = ComputeImageStatistics(halves);
			REQUIRE(statistics.has_value());
			CHECK(statistics->MeanLuminance == doctest::Approx(0.5));
			CHECK(statistics->MinLuminance == doctest::Approx(0.0));
			CHECK(statistics->MaxLuminance == doctest::Approx(1.0));
			CHECK(statistics->DistinctColors == 2);
		}
	}

}
