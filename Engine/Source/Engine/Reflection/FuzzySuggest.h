#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// "Did you mean" suggestions for unknown names (Architecture §5.4 FuzzySuggest): component, field, enum, method and
// setting names in loader warnings, automation errors (ErrorIssue::Suggestions) and script errors. Pure functions,
// deterministic on every platform; thread-safe.

namespace Engine {

	// The Levenshtein edit distance (insertions, deletions and substitutions of single bytes, each costing 1) between `a`
	// and `b`, compared byte-wise after ASCII lowercasing, so "mass" and "Mass" are at distance 0. O(|a| * |b|) time and
	// O(min(|a|, |b|)) memory.
	[[nodiscard]] size_t LevenshteinDistance(std::string_view a, std::string_view b);

	// The largest distance at which FuzzySuggest proposes a candidate for `name`: max(2, |name| / 3).
	[[nodiscard]] size_t GetMaxSuggestionDistance(std::string_view name);

	// Up to `maxResults` candidates whose LevenshteinDistance to `name` is at most GetMaxSuggestionDistance(name), ordered
	// by distance, then by byte-wise candidate text, without duplicates. A candidate equal to `name` (including case) is
	// never suggested. Empty when nothing is close or `maxResults` is 0.
	[[nodiscard]] std::vector<std::string> FuzzySuggest(std::string_view name, std::span<const std::string_view> candidates,
		size_t maxResults = 3);
	[[nodiscard]] std::vector<std::string> FuzzySuggest(std::string_view name, std::span<const std::string> candidates,
		size_t maxResults = 3);

	// The hint text for `suggestions`: "" for none, "did you mean 'Mass'?" for one, "did you mean 'Mass' or 'Max'?" for two,
	// "did you mean 'A', 'B' or 'C'?" for more.
	[[nodiscard]] std::string MakeDidYouMeanHint(std::span<const std::string> suggestions);

}
