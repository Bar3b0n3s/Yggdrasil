#include "TestsPCH.h"

#include "Engine/Platform/Paths.h"

#include "Support/TempDirectory.h"

namespace Engine {

	TEST_SUITE("Platform")
	{
		TEST_CASE("Paths: ValidateAppName accepts portable folder names and rejects the rest" * doctest::skip(true))
		{
			const std::array<std::string_view, 6> valid = { "Tetris", "Rolling Ball 3D", "My-Game_2", "Café", "a", "Game.v2" };
			for (const std::string_view name : valid)
			{
				CAPTURE(std::string(name));
				CHECK(Paths::ValidateAppName(name).has_value());
			}

			const std::string tooLong(Paths::MaxAppNameLength + 1, 'a');
			const std::array<std::string_view, 16> invalid = {
				"",
				tooLong,
				"Game/Level",
				"Game\\Level",
				"Game:1",
				"What?",
				"Star*",
				"Quote\"",
				"<Game>",
				"Pipe|",
				".",
				"..",
				" Leading",
				"Trailing.",
				"CON",
				"com1.txt",
			};
			for (const std::string_view name : invalid)
			{
				CAPTURE(std::string(name));
				const Status status = Paths::ValidateAppName(name);
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::Validation);
			}

			CHECK_FALSE(Paths::ValidateAppName(std::string_view("Bad\0Name", 8)).has_value());
			CHECK_FALSE(Paths::ValidateAppName("Tab\tName").has_value());
			CHECK_FALSE(Paths::ValidateAppName("\xff\xfe").has_value()); // not UTF-8
			CHECK(Paths::ValidateAppName(std::string(Paths::MaxAppNameLength, 'a')).has_value());
		}

		TEST_CASE("Paths: GetUserDataPaths lays out the folders under the root and creates nothing" * doctest::skip(true))
		{
			Test::TempDirectory root("UserDataRoot");
			const Result<UserDataPaths> paths = Paths::GetUserDataPaths("Tetris", root.GetPath());
			REQUIRE(paths.has_value());
			CHECK(paths->Root == root.GetPath() / "Tetris");
			CHECK(paths->Logs == root.GetPath() / "Tetris" / "Logs");
			CHECK(paths->Crashes == root.GetPath() / "Tetris" / "Crashes");
			CHECK(paths->GetLogFile("Tetris") == paths->Logs / "Tetris.log");

			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(paths->Root, error));

			REQUIRE(Paths::CreateUserDataDirectories(*paths).has_value());
			CHECK(std::filesystem::is_directory(paths->Logs, error));
			CHECK(std::filesystem::is_directory(paths->Crashes, error));
			// Creating them again is fine.
			CHECK(Paths::CreateUserDataDirectories(*paths).has_value());
		}

		TEST_CASE("Paths: GetUserDataPaths rejects a bad name and a relative root" * doctest::skip(true))
		{
			Test::TempDirectory root("UserDataRoot");
			const Result<UserDataPaths> badName = Paths::GetUserDataPaths("CON", root.GetPath());
			REQUIRE_FALSE(badName.has_value());
			CHECK(badName.error().GetCode() == ErrorCode::Validation);

			const Result<UserDataPaths> relative = Paths::GetUserDataPaths("Tetris", std::filesystem::path("relative") / "root");
			REQUIRE_FALSE(relative.has_value());
			CHECK(relative.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Paths: the OS user-data root is an absolute directory" * doctest::skip(true))
		{
			const Result<std::filesystem::path> root = Paths::GetUserDataRoot();
			REQUIRE_MESSAGE(root.has_value(), root.error().ToString());
			CHECK(root->is_absolute());
			std::error_code error;
			CHECK(std::filesystem::is_directory(*root, error));

			// Without an override, GetUserDataPaths uses it.
			const Result<UserDataPaths> paths = Paths::GetUserDataPaths("Tetris");
			REQUIRE(paths.has_value());
			CHECK(paths->Root == *root / "Tetris");
		}
	}

}
