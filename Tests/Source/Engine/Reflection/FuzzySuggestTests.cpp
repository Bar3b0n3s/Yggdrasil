#include "TestsPCH.h"

#include "Engine/Reflection/FuzzySuggest.h"

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("FuzzySuggest: Levenshtein distance ignores ASCII case")
		{
			CHECK(LevenshteinDistance("", "") == 0);
			CHECK(LevenshteinDistance("Mass", "mass") == 0);
			CHECK(LevenshteinDistance("Mas", "Mass") == 1);
			CHECK(LevenshteinDistance("kitten", "sitting") == 3);
			CHECK(LevenshteinDistance("", "abc") == 3);
			CHECK(LevenshteinDistance("Friction", "Fricton") == 1);
		}

		TEST_CASE("FuzzySuggest: suggestions are close names ordered by distance then text")
		{
			const std::vector<std::string> candidates = { "Mass", "Max", "Layer", "Friction", "MaxLinearVelocity" };
			CHECK(GetMaxSuggestionDistance("Mas") == 2);
			CHECK(GetMaxSuggestionDistance("MaxLinearVelocty") == 5);

			CHECK(FuzzySuggest("Mas", candidates) == std::vector<std::string>{ "Mass", "Max" });
			CHECK(FuzzySuggest("Fricton", candidates) == std::vector<std::string>{ "Friction" });
			CHECK(FuzzySuggest("Mass", candidates) == std::vector<std::string>{ "Max" }); // an exact name is never suggested
			CHECK(FuzzySuggest("Zzzzzz", candidates).empty());
			CHECK(FuzzySuggest("Mas", candidates, 1) == std::vector<std::string>{ "Mass" });
			CHECK(FuzzySuggest("Mas", candidates, 0).empty());
		}

		TEST_CASE("FuzzySuggest: duplicates are dropped and both candidate spellings give the same result")
		{
			const std::vector<std::string_view> views = { "Max", "Mass", "Max", "mass" };
			const std::vector<std::string> strings = { "Max", "Mass", "Max", "mass" };
			const std::vector<std::string> expected = { "Mass", "mass", "Max" }; // distance 0 first (in byte order), then distance 2
			CHECK(FuzzySuggest("MASS", views) == expected);
			CHECK(FuzzySuggest("MASS", strings) == expected);
			CHECK(GetMaxSuggestionDistance("") == 2);
			CHECK(LevenshteinDistance("abc", "") == 3);
			CHECK(LevenshteinDistance("flaw", "lawn") == 2);
		}

		TEST_CASE("FuzzySuggest: the hint lists suggestions in natural language")
		{
			CHECK(MakeDidYouMeanHint({}).empty());
			const std::vector<std::string> one = { "Mass" };
			CHECK(MakeDidYouMeanHint(one) == "did you mean 'Mass'?");
			const std::vector<std::string> two = { "Mass", "Max" };
			CHECK(MakeDidYouMeanHint(two) == "did you mean 'Mass' or 'Max'?");
			const std::vector<std::string> three = { "A", "B", "C" };
			CHECK(MakeDidYouMeanHint(three) == "did you mean 'A', 'B' or 'C'?");
		}
	}

}
