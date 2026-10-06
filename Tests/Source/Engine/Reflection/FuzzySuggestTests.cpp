#include "TestsPCH.h"

#include "Engine/Reflection/FuzzySuggest.h"

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("FuzzySuggest: Levenshtein distance ignores ASCII case" * doctest::skip(true))
		{
			CHECK(LevenshteinDistance("", "") == 0);
			CHECK(LevenshteinDistance("Mass", "mass") == 0);
			CHECK(LevenshteinDistance("Mas", "Mass") == 1);
			CHECK(LevenshteinDistance("kitten", "sitting") == 3);
			CHECK(LevenshteinDistance("", "abc") == 3);
			CHECK(LevenshteinDistance("Friction", "Fricton") == 1);
		}

		TEST_CASE("FuzzySuggest: suggestions are close names ordered by distance then text" * doctest::skip(true))
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

		TEST_CASE("FuzzySuggest: the hint lists suggestions in natural language" * doctest::skip(true))
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
