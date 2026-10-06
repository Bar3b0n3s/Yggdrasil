#include "TestsPCH.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

// Architecture §6 "Load -> save is byte-identical ... for every authored file under Projects/ and Tests/Data" at the JSON
// level (ADR 0003 decision 27): every authored file is canonical JsonWriter output. Line-oriented logs (.jsonl) and
// project Automation/ folders are exempt. Fixtures that are deliberately not canonical JSON are listed and justified
// here. Projects/ joins the walk when it first exists (M14).

namespace Engine {

	// Authored-format extensions (§6), as in Scripts/ModuleRules.json "Json".
	static const std::vector<std::string> AuthoredExtensions = { ".eproj", ".scene", ".prefab", ".material", ".sfx", ".meta", ".replay" };

	TEST_SUITE("Core")
	{
		TEST_CASE("AuthoredFiles: every authored file under Tests/Data is canonical JSON")
		{
			const Result<std::vector<std::string>> files = Test::ListTestDataFiles("", AuthoredExtensions);
			REQUIRE(files.has_value());
			size_t checked = 0;
			for (const std::string& file : *files)
			{
				// Lint fixtures are seeded defects of their own kind, never engine input.
				if (file.starts_with("Lint/") || file.find("/Automation/") != std::string::npos)
					continue;
				INFO(file);
				const Result<std::string> text = Test::ReadTestDataText(file);
				REQUIRE(text.has_value());
				const Result<Json> parsed = JsonReader::Parse(*text);
				REQUIRE(parsed.has_value());
				const Result<std::string> written = JsonWriter::Write(*parsed);
				REQUIRE(written.has_value());
				CHECK(*written == *text);
				++checked;
			}
			CHECK(checked >= 15);
		}
	}

}
