#include "TestsPCH.h"
#include "Support/GoldenImage.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Support/TestData.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

// Every check logs one line that starts with "Golden image '<name>' on device class '<class>'" and goes on with ": matched"
// (Info), ": goldens missing", ": wrote the candidate" or " does not match" (Warn). Scripts/Test.py reads these lines from
// the golden run's output, whatever reporter doctest used, to report the device class, the images without goldens, the
// candidates written and the mismatches: keep the texts in step with its GOLDEN_PATTERN.

namespace Engine {

	namespace Test {

		namespace Utils {

			// The smoke mode's bounds (§15.4): an image that rendered at all has two colours and is neither black nor white.
			constexpr uint64_t SmokeMinimumDistinctColors = 2;
			constexpr double SmokeMinimumMeanLuminance = 0.01;
			constexpr double SmokeMaximumMeanLuminance = 0.99;

			// Whether `text` can name a file in a golden directory: letters, digits, '.', '-' and '_', not starting with '.'.
			[[nodiscard]] static bool IsSafeFileComponent(std::string_view text)
			{
				return !text.empty() && text.front() != '.' && std::ranges::all_of(text, [](char character)
				{
					return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
						|| character == '.' || character == '-' || character == '_';
				});
			}

			// `path` for messages: relative to the repository root when it lies below it, with '/' separators on every host.
			[[nodiscard]] static std::string DisplayPath(const std::filesystem::path& path)
			{
				const std::filesystem::path relative = path.lexically_relative(GetRepositoryRoot());
				const bool belowRoot = !relative.empty() && *relative.begin() != "..";
				const std::u8string text = (belowRoot ? relative : path).generic_u8string();
				return std::string(reinterpret_cast<const char*>(text.data()), text.size());
			}

			// The outputs of a failed comparison of image `name` in `directory`.
			[[nodiscard]] static std::filesystem::path GetOutputPath(const std::filesystem::path& directory, std::string_view name,
				std::string_view kind)
			{
				return directory / PathFromUtf8(std::format("{}-{}.png", name, kind));
			}

			// Removes the outputs an earlier failed comparison of `name` left behind, so the output directory only ever shows
			// the images of current failures. A file that cannot be removed is reported in the log and otherwise ignored.
			static void RemoveStaleOutputs(const std::filesystem::path& directory, std::string_view name)
			{
				for (const std::string_view kind : { "actual", "expected", "diff" })
				{
					const std::filesystem::path path = GetOutputPath(directory, name, kind);
					if (!FileSystem::Exists(path))
						continue;
					const Status removed = FileSystem::Remove(path);
					if (!removed.has_value())
						ENGINE_CORE_WARN("Cannot remove the stale golden output '{}': {}", DisplayPath(path), removed.error());
				}
			}

			// The smoke checks of an image on a device class without goldens: empty when they pass, otherwise why not.
			[[nodiscard]] static std::string FindSmokeProblems(const Image& image, ImageStatistics& statistics)
			{
				if (!image.IsValid() || image.Pixels.empty())
					return std::format("the image is not a valid non-empty image ({}x{}, {} bytes)", image.Width, image.Height, image.Pixels.size());
				Result<ImageStatistics> computed = ComputeImageStatistics(image);
				if (!computed.has_value())
					return computed.error().ToString();
				statistics = *computed;

				std::string problems;
				if (statistics.DistinctColors < SmokeMinimumDistinctColors)
					problems += std::format("{} distinct colour(s), fewer than {}", statistics.DistinctColors, SmokeMinimumDistinctColors);
				if (statistics.MeanLuminance < SmokeMinimumMeanLuminance || statistics.MeanLuminance > SmokeMaximumMeanLuminance)
				{
					if (!problems.empty())
						problems += "; ";
					problems += std::format("mean luminance {:.4f} outside [{}, {}]", statistics.MeanLuminance, SmokeMinimumMeanLuminance,
						SmokeMaximumMeanLuminance);
				}
				return problems;
			}

			[[nodiscard]] static GoldenCheckResult MakeErrorResult(std::string message)
			{
				return GoldenCheckResult{ .Outcome = GoldenOutcome::Error, .Message = std::move(message) };
			}

		}

		std::filesystem::path GetGoldenDirectory(std::string_view deviceClass)
		{
			return GetRepositoryRoot() / "Tests" / "Golden" / PathFromUtf8(deviceClass);
		}

		std::filesystem::path GetGoldenOutputDirectory()
		{
			return GetRepositoryRoot() / "bin" / "TestResults" / "Golden";
		}

		GoldenSettings GetRepositoryGoldenSettings()
		{
			return GoldenSettings{
				.GoldenRoot = GetRepositoryRoot() / "Tests" / "Golden",
				.OutputDirectory = GetGoldenOutputDirectory(),
				.UpdateGolden = GetTestOptions().UpdateGolden,
			};
		}

		GoldenCheckResult CheckGoldenImage(std::string_view name, const Image& actual, std::string_view deviceClass,
			const ImageCompareThresholds& thresholds)
		{
			return CheckGoldenImage(name, actual, deviceClass, thresholds, GetRepositoryGoldenSettings());
		}

		GoldenCheckResult CheckGoldenImage(std::string_view name, const Image& actual, std::string_view deviceClass,
			const ImageCompareThresholds& thresholds, const GoldenSettings& settings)
		{
			if (!Utils::IsSafeFileComponent(name) || !Utils::IsSafeFileComponent(deviceClass))
			{
				return Utils::MakeErrorResult(std::format(
					"golden image name '{}' and device class '{}' must be plain file names (letters, digits, '.', '-', '_')", name, deviceClass));
			}
			const std::string prefix = std::format("Golden image '{}' on device class '{}'", name, deviceClass);
			const std::filesystem::path directory = settings.GoldenRoot / PathFromUtf8(deviceClass);
			const std::filesystem::path golden = directory / PathFromUtf8(std::format("{}.png", name));
			const std::string goldenText = Utils::DisplayPath(golden);
			const std::filesystem::path& outputDirectory = settings.OutputDirectory;

			if (settings.UpdateGolden)
			{
				Status written = FileSystem::CreateDirectories(directory);
				if (written.has_value())
					written = WritePng(golden, actual);
				if (!written.has_value())
					return Utils::MakeErrorResult(std::format("{}: cannot write the candidate '{}': {}", prefix, goldenText, written.error()));
				std::string message = std::format("{}: wrote the candidate '{}' (--update-golden); review it before committing", prefix, goldenText);
				ENGINE_CORE_WARN("{}", message);
				return GoldenCheckResult{ .Outcome = GoldenOutcome::Updated, .Message = std::move(message) };
			}

			if (FileSystem::Exists(golden))
			{
				const Result<Image> expected = ReadPng(golden);
				if (!expected.has_value())
					return Utils::MakeErrorResult(std::format("{}: cannot read the golden '{}': {}", prefix, goldenText, expected.error()));
				const Result<ImageCompareResult> comparison = CompareImages(actual, *expected, thresholds);
				if (!comparison.has_value())
					return Utils::MakeErrorResult(std::format("{}: cannot compare with the golden '{}': {}", prefix, goldenText, comparison.error()));
				if (comparison->Passed)
				{
					Utils::RemoveStaleOutputs(outputDirectory, name);
					std::string message = std::format("{}: matched '{}' ({})", prefix, goldenText, comparison->Summary);
					ENGINE_CORE_INFO("{}", message);
					return GoldenCheckResult{ .Outcome = GoldenOutcome::Matched, .Message = std::move(message) };
				}

				// The three images of the failure, for review; a size mismatch has no difference image.
				const std::filesystem::path actualPath = Utils::GetOutputPath(outputDirectory, name, "actual");
				const std::filesystem::path expectedPath = Utils::GetOutputPath(outputDirectory, name, "expected");
				const std::filesystem::path differencePath = Utils::GetOutputPath(outputDirectory, name, "diff");
				Utils::RemoveStaleOutputs(outputDirectory, name);
				Status written = FileSystem::CreateDirectories(outputDirectory);
				if (written.has_value())
					written = WritePng(actualPath, actual);
				if (written.has_value())
					written = WritePng(expectedPath, *expected);
				if (written.has_value() && comparison->Difference.IsValid())
					written = WritePng(differencePath, comparison->Difference);
				if (!written.has_value())
				{
					return Utils::MakeErrorResult(std::format("{} does not match its golden ({}), and the outputs cannot be written: {}", prefix,
						comparison->Summary, written.error()));
				}
				const std::string differenceText =
					comparison->Difference.IsValid() ? std::format(" and '{}'", Utils::DisplayPath(differencePath)) : std::string();
				std::string message = std::format("{} does not match the golden '{}': {}; wrote '{}', '{}'{}", prefix, goldenText,
					comparison->Summary, Utils::DisplayPath(actualPath), Utils::DisplayPath(expectedPath), differenceText);
				ENGINE_CORE_WARN("{}", message);
				return GoldenCheckResult{ .Outcome = GoldenOutcome::Mismatched, .Message = std::move(message) };
			}

			// Smoke mode: never a pass of the comparison, and visible as a warning.
			ImageStatistics statistics;
			const std::string problems = Utils::FindSmokeProblems(actual, statistics);
			const std::string summary = problems.empty()
				? std::format("smoke checks passed ({} distinct colours, mean luminance {:.4f})", statistics.DistinctColors, statistics.MeanLuminance)
				: std::format("smoke checks failed: {}", problems);
			std::string message = std::format("{}: goldens missing (no '{}'), {}", prefix, goldenText, summary);
			ENGINE_CORE_WARN("{}", message);
			const GoldenOutcome outcome = problems.empty() ? GoldenOutcome::SmokePassed : GoldenOutcome::SmokeFailed;
			return GoldenCheckResult{ .Outcome = outcome, .Message = std::move(message) };
		}

		std::string_view GoldenOutcomeToString(GoldenOutcome outcome)
		{
			switch (outcome)
			{
				case GoldenOutcome::Matched:     return "Matched";
				case GoldenOutcome::Mismatched:  return "Mismatched";
				case GoldenOutcome::Updated:     return "Updated";
				case GoldenOutcome::SmokePassed: return "SmokePassed";
				case GoldenOutcome::SmokeFailed: return "SmokeFailed";
				case GoldenOutcome::Error:       return "Error";
			}
			return "Unknown";
		}

	}

}
