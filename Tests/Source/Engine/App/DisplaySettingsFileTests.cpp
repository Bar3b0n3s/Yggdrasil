#include "TestsPCH.h"
#include "Engine/App/DisplaySettingsFile.h"

#include "Engine/App/Application.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Paths.h"
#include "Support/Fuzz.h"
#include "Support/TempDirectory.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <span>
#include <string>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 3): user://Display.json, the display settings a game chose, which
// the Runtime applies before its window first shows.

namespace Engine {

	TEST_SUITE("App")
	{
		TEST_CASE("DisplaySettingsFile: the settings round-trip through Display.json's text")
		{
			const DisplaySettings settings{ .WindowMode = DisplayWindowMode::Exclusive, .Width = 2560, .Height = 1440, .RefreshRate = 144, .VSync = false };
			const Result<std::string> text = SerializeDisplaySettingsFile(settings);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(*text == "{\n\t\"Format\": \"DisplaySettings\",\n\t\"Version\": 1,\n\t\"WindowMode\": \"Exclusive\",\n\t\"Width\": 2560,\n\t\"Height\": 1440,\n\t\"RefreshRate\": 144,\n\t\"VSync\": false\n}\n");
			const Result<DisplaySettings> read = ParseDisplaySettingsFile(*text);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(*read == settings);
			// Unknown members are ignored.
			const Result<DisplaySettings> future = ParseDisplaySettingsFile(
				R"({"Format":"DisplaySettings","Version":1,"WindowMode":"Borderless","Width":1920,"Height":1080,"RefreshRate":60,"VSync":true,"Hdr":true})");
			REQUIRE_MESSAGE(future.has_value(), future.error().ToString());
			CHECK(future->WindowMode == DisplayWindowMode::Borderless);
			// The writer refuses what the reader would.
			CHECK_FALSE(SerializeDisplaySettingsFile(DisplaySettings{ .Width = 0, .Height = 720 }).has_value());
			CHECK_FALSE(SerializeDisplaySettingsFile(DisplaySettings{ .Width = 1280, .Height = 720, .RefreshRate = 5000 }).has_value());
		}

		TEST_CASE("DisplaySettingsFile: another format, a newer version, a missing member or one out of range is refused at its member")
		{
			const auto refused = [](std::string_view text, ErrorCode code, std::string_view pointer)
			{
				CAPTURE(std::string(text));
				const Result<DisplaySettings> read = ParseDisplaySettingsFile(text);
				REQUIRE_FALSE(read.has_value());
				CHECK(read.error().GetCode() == code);
				if (!pointer.empty())
					CHECK(read.error().GetLocation().JsonPointer.value_or("") == pointer);
			};
			refused("not json", ErrorCode::Parse, "");
			refused(R"({"Format":"EditorPreferences","Version":1})", ErrorCode::Validation, "/Format");
			refused(R"({"Format":"DisplaySettings","Version":2,"WindowMode":"Windowed","Width":1,"Height":1,"RefreshRate":0,"VSync":true})", ErrorCode::UnsupportedVersion, "");
			refused(R"({"Format":"DisplaySettings","Version":1,"Width":1,"Height":1,"RefreshRate":0,"VSync":true})", ErrorCode::Validation, "");
			refused(R"({"Format":"DisplaySettings","Version":1,"WindowMode":"Fullscreen","Width":1,"Height":1,"RefreshRate":0,"VSync":true})", ErrorCode::Validation, "/WindowMode");
			refused(R"({"Format":"DisplaySettings","Version":1,"WindowMode":"Windowed","Width":0,"Height":1,"RefreshRate":0,"VSync":true})", ErrorCode::Validation, "/Width");
			refused(R"({"Format":"DisplaySettings","Version":1,"WindowMode":"Windowed","Width":1,"Height":20000,"RefreshRate":0,"VSync":true})", ErrorCode::Validation, "/Height");
			refused(R"({"Format":"DisplaySettings","Version":1,"WindowMode":"Windowed","Width":1,"Height":1,"RefreshRate":1001,"VSync":true})", ErrorCode::Validation, "/RefreshRate");
			refused(R"({"Format":"DisplaySettings","Version":1,"WindowMode":"Windowed","Width":1,"Height":1,"RefreshRate":0,"VSync":"yes"})", ErrorCode::Validation, "/VSync");
		}

		TEST_CASE("DisplaySettingsFile: the Runtime's startup read takes the game's Display.json, and a missing or invalid one gives its problem")
		{
			const Test::TempDirectory temp;
			const StartupDisplaySettings missing = ReadStartupDisplaySettings("Game", temp.GetPath());
			CHECK_FALSE(missing.Settings.has_value());
			CHECK(missing.Problem.empty());
			const Result<UserDataPaths> paths = Paths::GetUserDataPaths("Game", temp.GetPath());
			REQUIRE(paths.has_value());
			std::error_code error;
			std::filesystem::create_directories(paths->Root, error);
			REQUIRE_FALSE(error);
			const DisplaySettings settings{ .WindowMode = DisplayWindowMode::Borderless, .Width = 1920, .Height = 1080, .RefreshRate = 60, .VSync = true };
			const Result<std::string> text = SerializeDisplaySettingsFile(settings);
			REQUIRE(text.has_value());
			REQUIRE(FileSystem::WriteFileAtomic(paths->Root / "Display.json", std::as_bytes(std::span(text->data(), text->size()))).has_value());
			const StartupDisplaySettings read = ReadStartupDisplaySettings("Game", temp.GetPath());
			REQUIRE(read.Settings.has_value());
			CHECK(*read.Settings == settings);
			CHECK(read.Problem.empty());

			// The factory runs before logging exists: the problem comes back for the Runtime to log, and nothing is logged here.
			const std::string truncated = text->substr(0, text->size() / 2);
			REQUIRE(FileSystem::WriteFileAtomic(paths->Root / "Display.json", std::as_bytes(std::span(truncated.data(), truncated.size()))).has_value());
			const StartupDisplaySettings invalid = ReadStartupDisplaySettings("Game", temp.GetPath());
			CHECK_FALSE(invalid.Settings.has_value());
			CHECK(invalid.Problem.find("user://Display.json") != std::string::npos);
		}

		TEST_CASE("DisplaySettingsFile: the startup settings make the Runtime's first window in the chosen mode")
		{
			ApplicationSpecification specification;
			specification.WindowSettings = WindowSpecification{ .Width = 1280, .Height = 720, .Fullscreen = true };
			specification.Graphics.VSync = true;
			ApplyStartupDisplaySettings(specification, DisplaySettings{ .WindowMode = DisplayWindowMode::Exclusive, .Width = 1920, .Height = 1080, .RefreshRate = 144, .VSync = false });
			CHECK(specification.WindowSettings.DisplayMode == DisplayWindowMode::Exclusive);
			CHECK(specification.WindowSettings.Width == 1920);
			CHECK(specification.WindowSettings.Height == 1080);
			CHECK(specification.WindowSettings.RefreshRate == 144);
			CHECK_FALSE(specification.WindowSettings.Fullscreen);
			CHECK_FALSE(specification.Graphics.VSync);
			// Borderless covers the monitor: the window keeps the project's size for when it is windowed again.
			ApplyStartupDisplaySettings(specification, DisplaySettings{ .WindowMode = DisplayWindowMode::Borderless, .Width = 2560, .Height = 1440, .RefreshRate = 60, .VSync = true });
			CHECK(specification.WindowSettings.DisplayMode == DisplayWindowMode::Borderless);
			CHECK(specification.WindowSettings.Width == 1920);
			CHECK(specification.WindowSettings.RefreshRate == 60);
			CHECK(specification.Graphics.VSync);
			// Windowed takes the size, at no refresh rate of its own.
			ApplyStartupDisplaySettings(specification, DisplaySettings{ .WindowMode = DisplayWindowMode::Windowed, .Width = 800, .Height = 600, .RefreshRate = 60 });
			CHECK(specification.WindowSettings.DisplayMode == DisplayWindowMode::Windowed);
			CHECK(specification.WindowSettings.Width == 800);
			CHECK(specification.WindowSettings.Height == 600);
			CHECK(specification.WindowSettings.RefreshRate == 0);
		}

		TEST_CASE("DisplaySettingsFile: the writer needs a user:// mount and writes Display.json there")
		{
			VirtualFileSystem vfs;
			const DisplaySettings settings{ .Width = 1280, .Height = 720 };
			CHECK(WriteDisplaySettingsFile(vfs, settings).error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()));
			REQUIRE(WriteDisplaySettingsFile(vfs, settings).has_value());
			const auto path = VfsPath::Parse(DisplaySettingsPath);
			REQUIRE(path.has_value());
			const auto text = vfs.ReadText(*path);
			REQUIRE(text.has_value());
			CHECK(ParseDisplaySettingsFile(*text).value_or(DisplaySettings{}) == settings);
		}

		TEST_CASE("DisplaySettings: 1,000 seeded mutations of Display.json never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			const Result<std::string> written =
				SerializeDisplaySettingsFile(DisplaySettings{ .WindowMode = DisplayWindowMode::Exclusive, .Width = 1920, .Height = 1080, .RefreshRate = 165, .VSync = false });
			REQUIRE(written.has_value());
			const std::array<std::string, 2> seeds = { *written,
				R"({"Format":"DisplaySettings","Version":1,"WindowMode":"Borderless","Width":16384,"Height":1,"RefreshRate":0,"VSync":true,"Future":{"Nested":[1e300,null,"x"]}})" };
			const Test::FuzzRun run = Test::RunFuzzCase(0xD15B, 1000, [&seeds](Random& random, uint64_t /*index*/)
			{
				std::string mutated = seeds[static_cast<size_t>(random.RangeInt(0, 1))];
				Test::MutateText(mutated, random);
				const Result<DisplaySettings> read = ParseDisplaySettingsFile(mutated);
				if (read.has_value())
				{
					// Whatever the reader accepts, the writer writes back and the reader reads the same.
					const Result<std::string> again = SerializeDisplaySettingsFile(*read);
					REQUIRE(again.has_value());
					CHECK(ParseDisplaySettingsFile(*again).value_or(DisplaySettings{}) == *read);
					return true;
				}
				const ErrorCode code = read.error().GetCode();
				CHECK((code == ErrorCode::Parse || code == ErrorCode::Validation || code == ErrorCode::UnsupportedVersion));
				return false;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}
	}

}
