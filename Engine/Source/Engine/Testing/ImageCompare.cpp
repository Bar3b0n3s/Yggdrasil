#include "EnginePCH.h"
#include "Engine/Testing/ImageCompare.h"

namespace Engine {

	namespace Utils {

		constexpr size_t ChannelCount = 4;

		// The difference image scales channel differences by this factor, so a difference at the default tolerance (2/255)
		// is already visible and one of 16/255 or more saturates.
		constexpr uint32_t DifferenceScale = 16;

		// The cap of ImageStatistics::DistinctColors.
		constexpr uint64_t MaxDistinctColors = 65536;

		// Rec. 709 luma weights.
		constexpr double LumaRed = 0.2126;
		constexpr double LumaGreen = 0.7152;
		constexpr double LumaBlue = 0.0722;

		[[nodiscard]] static uint8_t AbsoluteDifference(std::byte a, std::byte b)
		{
			const int difference = std::to_integer<int>(a) - std::to_integer<int>(b);
			return static_cast<uint8_t>(difference < 0 ? -difference : difference);
		}

		[[nodiscard]] static std::byte ScaleDifference(uint8_t difference)
		{
			return static_cast<std::byte>(std::min<uint32_t>(static_cast<uint32_t>(difference) * DifferenceScale, 255));
		}

		// The one-line summary of a comparison of two images of equal size.
		[[nodiscard]] static std::string Summarize(const ImageCompareResult& result, const ImageCompareThresholds& thresholds)
		{
			return std::format("{}: {} of {} pixels ({:.2f} %) differ by more than {}/255; largest difference {}/255",
				result.Passed ? "passed" : "failed", result.DifferingPixels, result.PixelCount, result.DifferingFraction * 100.0,
				thresholds.PixelTolerance, result.LargestDifference);
		}

	}

	Result<ImageCompareResult> CompareImages(const Image& actual, const Image& expected, const ImageCompareThresholds& thresholds)
	{
		Result<Image> actualRgba = ConvertToRgba8(actual);
		if (!actualRgba.has_value())
			return std::unexpected(std::move(actualRgba).error().WithContext("while comparing the actual image"));
		Result<Image> expectedRgba = ConvertToRgba8(expected);
		if (!expectedRgba.has_value())
			return std::unexpected(std::move(expectedRgba).error().WithContext("while comparing the expected image"));

		ImageCompareResult result;
		if (actualRgba->Width != expectedRgba->Width || actualRgba->Height != expectedRgba->Height)
		{
			result.Summary = std::format("failed: the actual image is {}x{}, the expected image {}x{}", actualRgba->Width,
				actualRgba->Height, expectedRgba->Width, expectedRgba->Height);
			return result;
		}

		ENGINE_TRY_ASSIGN(result.Difference, CreateImage(actualRgba->Width, actualRgba->Height, nvrhi::Format::RGBA8_UNORM));
		result.PixelCount = static_cast<uint64_t>(actualRgba->Width) * actualRgba->Height;
		const std::vector<std::byte>& actualPixels = actualRgba->Pixels;
		const std::vector<std::byte>& expectedPixels = expectedRgba->Pixels;
		std::vector<std::byte>& differencePixels = result.Difference.Pixels;
		for (size_t offset = 0; offset < actualPixels.size(); offset += Utils::ChannelCount)
		{
			std::array<uint8_t, Utils::ChannelCount> differences{};
			uint8_t largest = 0;
			for (size_t channel = 0; channel < Utils::ChannelCount; ++channel)
			{
				differences[channel] = Utils::AbsoluteDifference(actualPixels[offset + channel], expectedPixels[offset + channel]);
				largest = std::max(largest, differences[channel]);
			}
			result.LargestDifference = std::max(result.LargestDifference, largest);
			if (largest > thresholds.PixelTolerance)
				++result.DifferingPixels;

			// Black where equal; red where the pixel exceeds MaxChannelDifference; otherwise the scaled colour differences,
			// with an alpha difference shown as grey on all three.
			const std::byte alpha = Utils::ScaleDifference(differences[3]);
			if (largest > thresholds.MaxChannelDifference)
			{
				differencePixels[offset + 0] = std::byte{ 255 };
				differencePixels[offset + 1] = std::byte{ 0 };
				differencePixels[offset + 2] = std::byte{ 0 };
			}
			else
			{
				for (size_t channel = 0; channel < 3; ++channel)
					differencePixels[offset + channel] = std::max(Utils::ScaleDifference(differences[channel]), alpha);
			}
			differencePixels[offset + 3] = std::byte{ 255 };
		}

		result.DifferingFraction = static_cast<double>(result.DifferingPixels) / static_cast<double>(result.PixelCount);
		result.Passed = result.DifferingFraction <= thresholds.MaxDifferingFraction && result.LargestDifference <= thresholds.MaxChannelDifference;
		result.Summary = Utils::Summarize(result, thresholds);
		return result;
	}

	Result<ImageStatistics> ComputeImageStatistics(const Image& image)
	{
		ENGINE_TRY_ASSIGN(const Image rgba, ConvertToRgba8(image));
		ImageStatistics statistics;
		const size_t pixelCount = rgba.Pixels.size() / Utils::ChannelCount;
		if (pixelCount == 0)
			return statistics;

		std::vector<uint32_t> colors;
		colors.reserve(pixelCount);
		double luminanceSum = 0.0;
		statistics.MinLuminance = 1.0;
		statistics.MaxLuminance = 0.0;
		for (size_t offset = 0; offset < rgba.Pixels.size(); offset += Utils::ChannelCount)
		{
			const uint32_t red = std::to_integer<uint32_t>(rgba.Pixels[offset + 0]);
			const uint32_t green = std::to_integer<uint32_t>(rgba.Pixels[offset + 1]);
			const uint32_t blue = std::to_integer<uint32_t>(rgba.Pixels[offset + 2]);
			const uint32_t alpha = std::to_integer<uint32_t>(rgba.Pixels[offset + 3]);
			const double luminance = (Utils::LumaRed * red + Utils::LumaGreen * green + Utils::LumaBlue * blue) / 255.0;
			luminanceSum += luminance;
			statistics.MinLuminance = std::min(statistics.MinLuminance, luminance);
			statistics.MaxLuminance = std::max(statistics.MaxLuminance, luminance);
			colors.push_back(red | (green << 8) | (blue << 16) | (alpha << 24));
		}
		statistics.MeanLuminance = luminanceSum / static_cast<double>(pixelCount);

		std::ranges::sort(colors);
		const auto duplicates = std::ranges::unique(colors);
		const auto distinct = static_cast<uint64_t>(std::distance(colors.begin(), duplicates.begin()));
		statistics.DistinctColors = std::min(distinct, Utils::MaxDistinctColors);
		return statistics;
	}

}
