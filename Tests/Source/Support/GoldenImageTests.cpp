#include "TestsPCH.h"

#include "Support/GoldenImage.h"

#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

// The golden harness (Architecture §15.4) on CPU images and a device class no machine has, so the checks never read or
// write the committed goldens; the comparison paths run on goldens in a temporary directory.

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

	// Settings that read and write only below `directory`.
	static Test::GoldenSettings MakeTemporarySettings(const Test::TempDirectory& directory, bool updateGolden)
	{
		return Test::GoldenSettings{
			.GoldenRoot = directory / "Golden",
			.OutputDirectory = directory / "Output",
			.UpdateGolden = updateGolden,
		};
	}

	static bool FileExists(const std::filesystem::path& path)
	{
		return FileSystem::Exists(path);
	}

	TEST_SUITE("Support")
	{
		TEST_CASE("GoldenImage: the golden and output directories follow the repository layout")
		{
			const std::filesystem::path golden = Test::GetGoldenDirectory("nvidia-58x");
			CHECK(golden.filename() == "nvidia-58x");
			CHECK(golden.parent_path().filename() == "Golden");
			CHECK(golden.parent_path().parent_path().filename() == "Tests");
			const std::filesystem::path output = Test::GetGoldenOutputDirectory();
			CHECK(output.filename() == "Golden");
			CHECK(output.parent_path().filename() == "TestResults");
		}

		TEST_CASE("GoldenImage: without goldens for the device class the smoke checks run")
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
			// The prefix and the event Scripts/Test.py parses (GOLDEN_PATTERN).
			CHECK(smoke.Message.starts_with("Golden image 'Checkerboard' on device class 'vendorffff-0': goldens missing"));

			// A flat black image fails the smoke checks: one colour, and luminance outside [0.01, 0.99].
			Image black = MakeCheckerboard(32);
			std::ranges::fill(black.Pixels, std::byte{ 0 });
			const Test::GoldenCheckResult failed = Test::CheckGoldenImage("Black", black, deviceClass);
			CHECK(failed.Outcome == Test::GoldenOutcome::SmokeFailed);
		}

		TEST_CASE("GoldenImage: a name or device class that is not a plain file name is an Error, and nothing is written")
		{
			// Checked before any file access, so this holds under --update-golden too.
			const Image image = MakeCheckerboard(8);
			const std::array<std::pair<std::string_view, std::string_view>, 6> invalid = { {
				{ "", "vendorffff-0" },
				{ "../Escape", "vendorffff-0" },
				{ "Sub/Name", "vendorffff-0" },
				{ ".Hidden", "vendorffff-0" },
				{ "Checkerboard", "" },
				{ "Checkerboard", "../vendorffff-0" },
			} };
			for (const auto& [name, deviceClass] : invalid)
			{
				CAPTURE(std::string(name));
				CAPTURE(std::string(deviceClass));
				const Test::GoldenCheckResult result = Test::CheckGoldenImage(name, image, deviceClass);
				CHECK(result.Outcome == Test::GoldenOutcome::Error);
				CHECK_FALSE(result.Message.empty());
			}
		}

		TEST_CASE("GoldenImage: update mode writes the candidate, which then matches and clears stale outputs")
		{
			Test::TempDirectory directory("GoldenUpdate");
			const Image image = MakeCheckerboard(16);

			const Test::GoldenCheckResult updated =
				Test::CheckGoldenImage("Board", image, "vendorffff-0", {}, MakeTemporarySettings(directory, true));
			CHECK(updated.Outcome == Test::GoldenOutcome::Updated);
			CHECK(updated.Message.starts_with("Golden image 'Board' on device class 'vendorffff-0': wrote the candidate"));
			const std::filesystem::path golden = directory / "Golden/vendorffff-0/Board.png";
			REQUIRE(FileExists(golden));
			const Result<Image> written = ReadPng(golden);
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			CHECK(written->Pixels == image.Pixels);

			// An earlier failure's outputs are removed by the next match.
			const Test::GoldenSettings compare = MakeTemporarySettings(directory, false);
			REQUIRE(FileSystem::CreateDirectories(compare.OutputDirectory).has_value());
			for (const char* kind : { "actual", "expected", "diff" })
				REQUIRE(WritePng(compare.OutputDirectory / std::format("Board-{}.png", kind), image).has_value());
			const Test::GoldenCheckResult matched = Test::CheckGoldenImage("Board", image, "vendorffff-0", {}, compare);
			CHECK(matched.Outcome == Test::GoldenOutcome::Matched);
			CHECK(matched.Message.starts_with("Golden image 'Board' on device class 'vendorffff-0': matched"));
			for (const char* kind : { "actual", "expected", "diff" })
				CHECK_FALSE(FileExists(compare.OutputDirectory / std::format("Board-{}.png", kind)));
		}

		TEST_CASE("GoldenImage: a changed image is Mismatched and writes the actual, expected and difference images")
		{
			Test::TempDirectory directory("GoldenMismatch");
			const Image image = MakeCheckerboard(16);
			REQUIRE(Test::CheckGoldenImage("Board", image, "vendorffff-0", {}, MakeTemporarySettings(directory, true)).Outcome
				== Test::GoldenOutcome::Updated);
			const Test::GoldenSettings compare = MakeTemporarySettings(directory, false);

			// Every pixel of the top half changed: far beyond the §15.4 thresholds.
			Image changed = image;
			std::fill(changed.Pixels.begin(), changed.Pixels.begin() + static_cast<std::ptrdiff_t>(changed.Pixels.size() / 2), std::byte{ 255 });
			const Test::GoldenCheckResult mismatched = Test::CheckGoldenImage("Board", changed, "vendorffff-0", {}, compare);
			CHECK(mismatched.Outcome == Test::GoldenOutcome::Mismatched);
			CHECK(mismatched.Message.starts_with("Golden image 'Board' on device class 'vendorffff-0' does not match"));
			const Result<Image> actual = ReadPng(compare.OutputDirectory / "Board-actual.png");
			REQUIRE_MESSAGE(actual.has_value(), actual.error().ToString());
			CHECK(actual->Pixels == changed.Pixels);
			const Result<Image> expected = ReadPng(compare.OutputDirectory / "Board-expected.png");
			REQUIRE_MESSAGE(expected.has_value(), expected.error().ToString());
			CHECK(expected->Pixels == image.Pixels);
			CHECK(FileExists(compare.OutputDirectory / "Board-diff.png"));

			// A different size has no difference image, and the stale one of the previous failure is gone.
			const Test::GoldenCheckResult resized = Test::CheckGoldenImage("Board", MakeCheckerboard(8), "vendorffff-0", {}, compare);
			CHECK(resized.Outcome == Test::GoldenOutcome::Mismatched);
			CHECK(resized.Message.starts_with("Golden image 'Board' on device class 'vendorffff-0' does not match"));
			CHECK(FileExists(compare.OutputDirectory / "Board-actual.png"));
			CHECK(FileExists(compare.OutputDirectory / "Board-expected.png"));
			CHECK_FALSE(FileExists(compare.OutputDirectory / "Board-diff.png"));
		}

		TEST_CASE("GoldenImage: a golden that cannot be decoded is an Error")
		{
			Test::TempDirectory directory("GoldenCorrupt");
			const Test::GoldenSettings settings = MakeTemporarySettings(directory, false);
			const std::filesystem::path golden = settings.GoldenRoot / "vendorffff-0" / "Board.png";
			REQUIRE(FileSystem::CreateDirectories(golden.parent_path()).has_value());
			const std::string_view notPng = "not a PNG";
			REQUIRE(FileSystem::WriteFileAtomic(golden, std::as_bytes(std::span(notPng)), { .KeepBackup = false }).has_value());
			const Test::GoldenCheckResult result = Test::CheckGoldenImage("Board", MakeCheckerboard(8), "vendorffff-0", {}, settings);
			CHECK(result.Outcome == Test::GoldenOutcome::Error);
			CHECK(result.Message.starts_with("Golden image 'Board' on device class 'vendorffff-0': cannot read the golden"));
		}

		TEST_CASE("GoldenImage: outcome names are their enumerator names")
		{
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::Matched) == "Matched");
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::Mismatched) == "Mismatched");
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::Updated) == "Updated");
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::SmokePassed) == "SmokePassed");
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::SmokeFailed) == "SmokeFailed");
			CHECK(Test::GoldenOutcomeToString(Test::GoldenOutcome::Error) == "Error");
		}
	}

}
