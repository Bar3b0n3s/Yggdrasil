#include "TestsPCH.h"

#include "Support/TempDirectory.h"

#include "Engine/Core/FileSystem.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("TempDirectory: creates an empty unique directory and removes it with its content" * doctest::skip(true))
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

				const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(first.GetPath());
				REQUIRE(entries.has_value());
				CHECK(entries->empty());

				REQUIRE(FileSystem::CreateDirectories(first / "Nested/Deeper").has_value());
				REQUIRE(FileSystem::WriteFileAtomic(first / "Nested/Deeper/File.txt", AsBytes("content")).has_value());
				CHECK((first / "Nested/Deeper/File.txt") == first.GetPath() / "Nested" / "Deeper" / "File.txt");
			}
			CHECK_FALSE(FileSystem::Exists(firstPath));
		}
	}

}
