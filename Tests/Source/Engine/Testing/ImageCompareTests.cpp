#include "TestsPCH.h"

#include "Engine/Testing/ImageCompare.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/Image.h"
#include "Support/ChildProcess.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

// The golden comparison (Architecture §15.4), and its PNG entry point (a ChildTargets case) for
// Tests/Automation/test_game_view.py's compare_png (Docs/Decisions/0012-m7-decisions.md decision 9): it compares two PNG
// files with the golden thresholds through CompareImages, so the Python suite uses the C++ comparison.

namespace Engine {

	// The members of the comparison child's --child-argument: a JSON object {"actual": <path>, "expected": <path>}.
	static constexpr std::string_view CompareActualMember = "actual";
	static constexpr std::string_view CompareExpectedMember = "expected";
	static constexpr const char* ComparePngsTargetName = "ImageCompare: two PNG files match with the golden thresholds (child target)";

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
		TEST_CASE("ImageCompare: thresholds of the golden comparison")
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

		TEST_CASE("ImageCompare: the difference image is black where equal, scaled where close and red beyond the maximum")
		{
			const Image expected = MakeUniform(3, 1, 100);
			Image actual = expected;
			actual.Pixels[4 + 1] = std::byte{ 103 }; // pixel 1: green off by 3
			actual.Pixels[8 + 2] = std::byte{ 200 }; // pixel 2: blue off by 100
			const Result<ImageCompareResult> result = CompareImages(actual, expected);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			const std::vector<std::byte>& difference = result->Difference.Pixels;
			REQUIRE(difference.size() == 12);
			const auto pixel = [&difference](size_t index)
			{
				return std::array<uint8_t, 4>{ std::to_integer<uint8_t>(difference[index * 4 + 0]), std::to_integer<uint8_t>(difference[index * 4 + 1]),
					std::to_integer<uint8_t>(difference[index * 4 + 2]), std::to_integer<uint8_t>(difference[index * 4 + 3]) };
			};
			CHECK(pixel(0) == std::array<uint8_t, 4>{ 0, 0, 0, 255 });
			const std::array<uint8_t, 4> close = pixel(1);
			CHECK(close[0] == 0);
			CHECK(close[1] > 0); // the green difference, scaled up
			CHECK(close[2] == 0);
			CHECK(pixel(2) == std::array<uint8_t, 4>{ 255, 0, 0, 255 });
			CHECK(result->DifferingPixels == 2);
			CHECK(result->LargestDifference == 100);
			CHECK_FALSE(result->Passed);
			CHECK(result->Summary.starts_with("failed: 2 of 3 pixels"));
		}

		TEST_CASE("ImageCompare: BGRA8 and RGBA8 images of the same colours are equal")
		{
			Image rgba = MakeUniform(2, 2, 0);
			Image bgra = rgba;
			bgra.Format = nvrhi::Format::BGRA8_UNORM;
			for (size_t offset = 0; offset < rgba.Pixels.size(); offset += 4)
			{
				rgba.Pixels[offset + 0] = std::byte{ 10 };  // red
				rgba.Pixels[offset + 2] = std::byte{ 200 }; // blue
				bgra.Pixels[offset + 0] = std::byte{ 200 }; // blue first in BGRA
				bgra.Pixels[offset + 2] = std::byte{ 10 };
			}
			const Result<ImageCompareResult> result = CompareImages(bgra, rgba);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->Passed);
			CHECK(result->LargestDifference == 0);
		}

		TEST_CASE("ImageCompare: images of different sizes never pass")
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

		TEST_CASE("ImageCompare: smoke statistics describe the luminance and the colours")
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

		// The comparison entry point of the Python suite: `Tests --no-skip --test-case=<this name>
		// --child-argument={"actual": <path>, "expected": <path>} --child-process` passes (exit code 0) when the PNGs match
		// with the golden thresholds of §15.4 (Testing/ImageCompare) and fails naming the differences otherwise.
		TEST_CASE(ComparePngsTargetName * doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			const Result<Json> argument = JsonReader::Parse(Test::GetTestOptions().ChildArgument);
			REQUIRE_MESSAGE(argument.has_value(), "--child-argument must be {\"actual\": <path>, \"expected\": <path>}: " << argument.error().ToString());
			const Result<std::string> actualPath = JsonReader(*argument).ReadMember<std::string>(CompareActualMember);
			const Result<std::string> expectedPath = JsonReader(*argument).ReadMember<std::string>(CompareExpectedMember);
			REQUIRE_MESSAGE(actualPath.has_value(), actualPath.error().ToString());
			REQUIRE_MESSAGE(expectedPath.has_value(), expectedPath.error().ToString());

			const Result<Image> actual = ReadPng(FileSystem::PathFromUtf8(*actualPath));
			REQUIRE_MESSAGE(actual.has_value(), actual.error().ToString());
			const Result<Image> expected = ReadPng(FileSystem::PathFromUtf8(*expectedPath));
			REQUIRE_MESSAGE(expected.has_value(), expected.error().ToString());
			const Result<ImageCompareResult> compared = CompareImages(*actual, *expected);
			REQUIRE_MESSAGE(compared.has_value(), compared.error().ToString());
			MESSAGE("PNG comparison of '" << *actualPath << "' with '" << *expectedPath << "': " << compared->Summary);
			CHECK_MESSAGE(compared->Passed, compared->Summary);
		}

		TEST_CASE("ImageCompare: the PNG comparison entry point passes equal images and fails different ones")
		{
			Test::TempDirectory directory("ImageComparePngs");
			const auto writeImage = [&directory](std::string_view name, std::byte value) -> std::filesystem::path
			{
				Result<Image> image = CreateImage(16, 8, nvrhi::Format::RGBA8_UNORM);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				std::ranges::fill(image->Pixels, value);
				const std::filesystem::path path = directory / std::string(name);
				REQUIRE(WritePng(path, *image).has_value());
				return path;
			};
			const std::filesystem::path base = writeImage("Base.png", std::byte{ 100 });
			const std::filesystem::path same = writeImage("Same.png", std::byte{ 101 }); // within the 2/255 tolerance
			const std::filesystem::path other = writeImage("Other.png", std::byte{ 180 });

			const auto compare = [&base](const std::filesystem::path& actual) -> Test::ChildProcessResult
			{
				const Json argument = Json{ { CompareActualMember, Test::PathToUtf8(actual) }, { CompareExpectedMember, Test::PathToUtf8(base) } };
				const std::vector<std::string> arguments = {
					"--no-skip",
					std::string("--test-case=") + ComparePngsTargetName,
					"--child-argument=" + argument.dump(),
				};
				const Result<Test::ChildProcessResult> result =
					Test::RunChildProcess(Test::GetTestOptions().ExecutablePath, arguments, std::chrono::seconds(60));
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				return *result;
			};
			const Test::ChildProcessResult matched = compare(same);
			INFO("child output: ", matched.StandardOutput, matched.StandardError);
			CHECK(matched.ExitCode == 0);
			const Test::ChildProcessResult differed = compare(other);
			INFO("child output: ", differed.StandardOutput, differed.StandardError);
			CHECK(differed.ExitCode != 0);
			CHECK((differed.StandardOutput + differed.StandardError).contains("differ"));
		}
	}

}
