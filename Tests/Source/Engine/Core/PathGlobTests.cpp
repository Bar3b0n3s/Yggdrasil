#include "TestsPCH.h"

#include "Engine/Core/PathGlob.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <string>

// The one path-glob matcher of Export.Exclude and Testing.Replays (Docs/Decisions/0024-m15-contract.md decision 7, stream
// B), which replaces the exporter's and the Runtime's copies.

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("PathGlob: '*' and '?' stay in a segment, '**' crosses separators and '**/' also matches no directory")
		{
			CHECK(MatchesPathGlob("Assets/Tests/**", "Assets/Tests/Level.scene"));
			CHECK(MatchesPathGlob("Assets/Tests/**", "Assets/Tests/Deep/Er/Module.luau"));
			CHECK_FALSE(MatchesPathGlob("Assets/Tests/**", "Assets/TestsOther/Level.scene"));
			CHECK(MatchesPathGlob("Assets/Tests/Replays/*.replay", "Assets/Tests/Replays/Input.replay"));
			CHECK_FALSE(MatchesPathGlob("Assets/Tests/Replays/*.replay", "Assets/Tests/Replays/Old/Input.replay"));
			CHECK_FALSE(MatchesPathGlob("Assets/Tests/Replays/*.replay", "Assets/Tests/Replays/Input.replay.meta"));
			CHECK(MatchesPathGlob("Assets/**/Old.png", "Assets/Old.png"));
			CHECK(MatchesPathGlob("Assets/**/Old.png", "Assets/A/B/Old.png"));
			CHECK(MatchesPathGlob("Assets/Level?.scene", "Assets/Level1.scene"));
			CHECK_FALSE(MatchesPathGlob("Assets/Level?.scene", "Assets/Level10.scene"));
			CHECK_FALSE(MatchesPathGlob("Assets/?", "Assets//"));
			CHECK_FALSE(MatchesPathGlob("Assets/level1.scene", "Assets/Level1.scene")); // case-sensitive (§4.10)
			CHECK(MatchesPathGlob("Assets/Exact.scene", "Assets/Exact.scene"));
			CHECK_FALSE(MatchesPathGlob("", "Assets/Exact.scene"));
			CHECK(MatchesPathGlob("**", "Assets/Any/Thing.png"));
			// No exponential backtracking: a long path against many stars resolves at once.
			const std::string path = std::string(200, 'a') + "/b";
			CHECK_FALSE(MatchesPathGlob("*a*a*a*a*a*a*a*a*a*a*a*a*c", path));
			CHECK(MatchesPathGlob("a*a*a*a*a*a*a*a*a*a*a*a/b", path));
		}

		TEST_CASE("PathGlob: '?' and '*' step over whole UTF-8 code points and an ill-formed byte counts as one")
		{
			// U+00F1 (2 bytes), U+20AC (3) and U+1F3B2 (4) are one character each, as Test.py's glob_regex counts them; the
			// shared cases (Tests/Data/PathGlob/Cases.json) hold the same rule. Literal non-ASCII matches itself.
			CHECK(MatchesPathGlob("Ni?o", "Ni\xc3\xb1o"));
			CHECK_FALSE(MatchesPathGlob("Ni??o", "Ni\xc3\xb1o"));
			CHECK(MatchesPathGlob("?", "\xe2\x82\xac"));
			CHECK_FALSE(MatchesPathGlob("???", "\xe2\x82\xac"));
			CHECK(MatchesPathGlob("?", "\xf0\x9f\x8e\xb2"));
			CHECK_FALSE(MatchesPathGlob("*??", "\xe2\x82\xac"));
			CHECK_FALSE(MatchesPathGlob("**??", "\xc3\xa9"));
			CHECK(MatchesPathGlob("\xc3\xa9*", "\xc3\xa9t\xc3\xa9"));
			// A pattern byte that splits a code point never lets a wildcard continue inside it.
			CHECK_FALSE(MatchesPathGlob("\xc3*", "\xc3\xa9"));
			CHECK_FALSE(MatchesPathGlob("\xc3?", "\xc3\xa9"));
			// Bytes of ill-formed sequences (no VfsPath holds them) are one character each.
			CHECK(MatchesPathGlob("A?", "A\xff"));
			CHECK(MatchesPathGlob("A?", "A\x80"));
			CHECK_FALSE(MatchesPathGlob("A?", "A\xc3("));
			CHECK(MatchesPathGlob("A??", "A\xc3("));
			CHECK(MatchesPathGlob("A*", "A\xe2\x82"));
			CHECK(MatchesPathGlob("A??", "A\xe2\x82"));
		}

		TEST_CASE("PathGlob: the shared cases that Test.py's glob also runs")
		{
			// Tests/Data/PathGlob/Cases.json, which Tests/Automation/test_coverage_gates.py runs against Scripts/Test.py's
			// glob_regex, so the C++ and Python matchers cannot drift apart.
			const Result<std::string> text = Test::ReadTestDataText("PathGlob/Cases.json");
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const Result<Json> document = JsonReader::Parse(*text);
			REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
			const JsonReader root(*document);
			const Result<JsonReader> cases = root.GetMember("Cases");
			REQUIRE_MESSAGE(cases.has_value(), cases.error().ToString());
			const Result<size_t> count = cases->GetArraySize();
			REQUIRE_MESSAGE(count.has_value(), count.error().ToString());
			REQUIRE(*count > 0);
			for (size_t index = 0; index < *count; ++index)
			{
				const Result<JsonReader> entry = cases->GetElement(index);
				REQUIRE(entry.has_value());
				const Result<std::string> pattern = entry->ReadMember<std::string>("Pattern");
				const Result<std::string> path = entry->ReadMember<std::string>("Path");
				const Result<bool> matches = entry->ReadMember<bool>("Matches");
				REQUIRE(pattern.has_value());
				REQUIRE(path.has_value());
				REQUIRE(matches.has_value());
				CAPTURE(*pattern);
				CAPTURE(*path);
				CHECK(MatchesPathGlob(*pattern, *path) == *matches);
			}
		}
	}

}
