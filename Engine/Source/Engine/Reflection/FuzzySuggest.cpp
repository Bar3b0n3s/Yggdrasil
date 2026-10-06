#include "EnginePCH.h"
#include "Engine/Reflection/FuzzySuggest.h"

#include <algorithm>
#include <utility>

namespace Engine {

	namespace Utils {

		static char FoldAsciiCase(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		template<typename Candidate>
		static std::vector<std::string> SuggestFrom(std::string_view name, std::span<const Candidate> candidates, size_t maxResults)
		{
			if (maxResults == 0)
				return {};

			const size_t maxDistance = GetMaxSuggestionDistance(name);
			std::vector<std::pair<size_t, std::string_view>> close;
			for (const Candidate& candidate : candidates)
			{
				const std::string_view text(candidate);
				if (text == name)
					continue;
				const size_t distance = LevenshteinDistance(name, text);
				if (distance <= maxDistance)
					close.emplace_back(distance, text);
			}

			std::sort(close.begin(), close.end());
			const auto sameText = [](const auto& lhs, const auto& rhs)
			{
				return lhs.second == rhs.second;
			};
			close.erase(std::unique(close.begin(), close.end(), sameText), close.end());

			std::vector<std::string> suggestions;
			for (const auto& [distance, text] : close)
			{
				if (suggestions.size() == maxResults)
					break;
				suggestions.emplace_back(text);
			}
			return suggestions;
		}

	}

	size_t LevenshteinDistance(std::string_view a, std::string_view b)
	{
		// One row of the dynamic-programming table, over the shorter string.
		if (a.size() < b.size())
			std::swap(a, b);

		std::vector<size_t> row(b.size() + 1);
		for (size_t column = 0; column <= b.size(); ++column)
			row[column] = column;

		for (size_t line = 1; line <= a.size(); ++line)
		{
			size_t diagonal = row[0];
			row[0] = line;
			const char left = Utils::FoldAsciiCase(a[line - 1]);
			for (size_t column = 1; column <= b.size(); ++column)
			{
				const size_t above = row[column];
				const size_t substitution = diagonal + (left == Utils::FoldAsciiCase(b[column - 1]) ? 0 : 1);
				row[column] = std::min({ above + 1, row[column - 1] + 1, substitution });
				diagonal = above;
			}
		}
		return row[b.size()];
	}

	size_t GetMaxSuggestionDistance(std::string_view name)
	{
		return std::max<size_t>(2, name.size() / 3);
	}

	std::vector<std::string> FuzzySuggest(std::string_view name, std::span<const std::string_view> candidates, size_t maxResults)
	{
		return Utils::SuggestFrom(name, candidates, maxResults);
	}

	std::vector<std::string> FuzzySuggest(std::string_view name, std::span<const std::string> candidates, size_t maxResults)
	{
		return Utils::SuggestFrom(name, candidates, maxResults);
	}

	std::string MakeDidYouMeanHint(std::span<const std::string> suggestions)
	{
		if (suggestions.empty())
			return {};

		std::string hint = "did you mean ";
		for (size_t i = 0; i < suggestions.size(); ++i)
		{
			if (i > 0)
				hint += i + 1 == suggestions.size() ? " or " : ", ";
			hint += '\'';
			hint += suggestions[i];
			hint += '\'';
		}
		hint += '?';
		return hint;
	}

}
