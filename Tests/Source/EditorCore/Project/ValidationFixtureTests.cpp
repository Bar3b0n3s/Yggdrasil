#include "TestsPCH.h"

#include "EditorCore/Project/ProjectValidator.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		// Tests/Data/Validation/<Name>/Expected.json of every fixture (ADR 0022 decision 11), sorted.
		std::vector<std::string> ListValidationFixtures()
		{
			const auto files = Test::ListTestDataFiles("Validation", { ".json" });
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			std::vector<std::string> fixtures;
			for (const std::string& file : *files)
			{
				const std::string_view path(file);
				if (path.ends_with("/Expected.json") && std::ranges::count(path, '/') == 2)
					fixtures.push_back(file);
			}
			return fixtures;
		}

		std::vector<std::string> ReadExpectedCodes(const std::string& file)
		{
			const auto text = Test::ReadTestDataText(file);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const auto document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			const JsonReader reader(*document);
			REQUIRE(reader.ReadFormatHeader("ValidationFixture", 1, 1).has_value());
			const auto codes = reader.GetMember("Codes");
			REQUIRE(codes.has_value());
			const auto size = codes->GetArraySize();
			REQUIRE(size.has_value());
			std::vector<std::string> result;
			for (size_t index = 0; index < *size; ++index)
			{
				const auto code = codes->GetElement(index);
				REQUIRE(code.has_value());
				const auto value = code->ReadString();
				REQUIRE(value.has_value());
				result.push_back(*value);
			}
			return result;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Architecture §15.6 gate 6. Each directory is named after the code it exists for (or "Clean", the baseline that
		// reports nothing) and its Expected.json lists every code the fixture reports.
		TEST_CASE("Validation: every project.validate code is triggered by a fixture in Tests/Data/Validation" * doctest::skip(true))
		{
			const std::vector<std::string> fixtures = ListValidationFixtures();
			REQUIRE_FALSE(fixtures.empty());
			std::set<std::string> triggered;
			bool clean = false;
			for (const std::string& fixture : fixtures)
			{
				CAPTURE(fixture);
				const std::string name = fixture.substr(std::string_view("Validation/").size(),
					fixture.size() - std::string_view("Validation/").size() - std::string_view("/Expected.json").size());
				const std::vector<std::string> codes = ReadExpectedCodes(fixture);
				if (name == "Clean")
				{
					clean = true;
					CHECK(codes.empty());
					continue;
				}
				CHECK(std::ranges::find(ProjectValidator::GetCodes(), name) != ProjectValidator::GetCodes().end());
				CHECK(std::ranges::find(codes, name) != codes.end());
				triggered.insert(codes.begin(), codes.end());
			}
			CHECK(clean);
			for (const std::string_view code : ProjectValidator::GetCodes())
			{
				CAPTURE(code);
				CHECK(triggered.contains(std::string(code)));
			}
		}

		TEST_CASE("Validation: each fixture reports exactly its expected codes" * doctest::skip(true))
		{
			const std::vector<std::string> fixtures = ListValidationFixtures();
			REQUIRE_FALSE(fixtures.empty());
			FAIL("contract skeleton: copy each fixture's Project/ into a temporary directory, open it, run ProjectValidator::Validate "
				 "(ValidationScope::Project) and compare the sorted set of reported codes with Expected.json's Codes");
		}

		// Tests/Data/CoverageGaps/Gate6/Expected.json: {"Format": "CoverageGap", "Version": 1, "Gate": "6", "Exclude": [fixture
		// directories], "Missing": [codes]}: without the excluded fixtures, gate 6 fails and names exactly Missing.
		TEST_CASE("Validation: the gate 6 seeded-gap fixture names exactly its untriggered code" * doctest::skip(true))
		{
			const auto expected = Test::ReadTestDataText("CoverageGaps/Gate6/Expected.json");
			REQUIRE_MESSAGE(expected.has_value(), expected.error().ToString());
			FAIL("contract skeleton: evaluate gate 6 over the fixtures minus Exclude and compare the untriggered codes with Missing");
		}
	}

}
