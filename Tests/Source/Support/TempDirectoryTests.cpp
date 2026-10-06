#include "TestsPCH.h"

#include "Support/TempDirectory.h"

#include "Support/DeathTest.h"

#include <fstream>
#include <system_error>

// These tests inspect the directory with std::filesystem rather than Engine::FileSystem, so the support code every
// FileSystem test relies on is verified independently of FileSystem.

namespace Engine {

	ENGINE_DEATH_TEST("Support/TempDirectoryRejectsBadLabel")
	{
		Test::TempDirectory directory("not a label");
	}

	ENGINE_DEATH_TEST("Support/TempDirectoryRejectsEscape")
	{
		Test::TempDirectory directory("Escape");
		// The assert ends the process before the destructor could remove the directory; operator/ never touches it.
		std::error_code error;
		std::filesystem::remove(directory.GetPath(), error);
		static_cast<void>(directory / "Nested/../../Outside.txt");
	}

	TEST_SUITE("Support")
	{
		TEST_CASE("TempDirectory: creates an empty unique directory and removes it with its content")
		{
			std::filesystem::path firstPath;
			{
				Test::TempDirectory first("Unique");
				Test::TempDirectory second("Unique");
				firstPath = first.GetPath();

				CHECK(first.GetPath().is_absolute());
				CHECK(first.GetPath() != second.GetPath());
				CHECK(first.GetPath().parent_path().filename() == "EngineTests");
				CHECK(first.GetPath().filename().generic_string().starts_with("Unique-"));
				CHECK(first.GetPath().filename().generic_string().size() == std::string_view("Unique-").size() + 16);

				std::error_code error;
				CHECK(std::filesystem::is_directory(first.GetPath(), error));
				CHECK(std::filesystem::is_empty(first.GetPath(), error));
				CHECK_FALSE(error);

				REQUIRE(std::filesystem::create_directories(first / "Nested/Deeper", error));
				REQUIRE_FALSE(error);
				{
					std::ofstream file(first / "Nested/Deeper/File.txt", std::ios::binary);
					file << "content";
					REQUIRE(file.good());
				}
				CHECK((first / "Nested/Deeper/File.txt") == first.GetPath() / "Nested" / "Deeper" / "File.txt");
				CHECK(std::filesystem::is_regular_file(first.GetPath() / "Nested" / "Deeper" / "File.txt", error));
			}
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(firstPath, error));
			CHECK_FALSE(error);
		}

		TEST_CASE("TempDirectory: the operator / result uses the native separators")
		{
			Test::TempDirectory directory("Separators");
			const std::filesystem::path joined = directory / "A/B/C.txt";
			CHECK(joined.parent_path().parent_path().parent_path() == directory.GetPath());
			CHECK(joined.filename() == "C.txt");
			CHECK(joined.native().find(std::filesystem::path("A/B").make_preferred().native()) != std::filesystem::path::string_type::npos);
		}

		TEST_CASE("TempDirectory: a label outside letters, digits, '-' and '_' is a programmer error")
		{
			ENGINE_CHECK_DEATH("Support/TempDirectoryRejectsBadLabel", "TempDirectory label 'not a label'");
		}

		TEST_CASE("TempDirectory: a path that leaves the directory is a programmer error")
		{
			ENGINE_CHECK_DEATH("Support/TempDirectoryRejectsEscape", "'Nested/../../Outside.txt' is absolute or contains '..'");
		}
	}

}
