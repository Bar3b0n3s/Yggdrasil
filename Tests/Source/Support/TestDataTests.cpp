#include "TestsPCH.h"

#include "Support/TestData.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("TestData: locates Tests/Data from any working directory")
		{
			const std::filesystem::path data = Test::GetTestDataPath();
			CHECK(data.is_absolute());
			CHECK(data.filename() == "Data");
			std::error_code error;
			CHECK(std::filesystem::is_directory(data / "Scenes", error));
			CHECK(std::filesystem::is_regular_file(Test::GetRepositoryRoot() / "AGENTS.md", error));
			CHECK(Test::GetTestDataPath("Scenes/Invalid") == (data / "Scenes" / "Invalid").lexically_normal());
		}

		TEST_CASE("TestData: lists fixtures sorted with forward slashes")
		{
			const Result<std::vector<std::string>> files = Test::ListTestDataFiles("Scenes", { ".scene" });
			REQUIRE(files.has_value());
			REQUIRE_FALSE(files->empty());
			CHECK(std::is_sorted(files->begin(), files->end()));
			for (const std::string& file : *files)
			{
				CHECK(file.starts_with("Scenes/"));
				CHECK(file.ends_with(".scene"));
				CHECK(file.find('\\') == std::string::npos);
			}

			const Result<std::string> text = Test::ReadTestDataText(files->front());
			REQUIRE(text.has_value());
			CHECK(text->starts_with("{\n"));
			CHECK_FALSE(Test::ReadTestDataText("Scenes/DoesNotExist.scene").has_value());
		}
	}

}
