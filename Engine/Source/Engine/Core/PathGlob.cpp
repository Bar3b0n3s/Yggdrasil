#include "EnginePCH.h"
#include "Engine/Core/PathGlob.h"

#include "Engine/Core/Utf8.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Engine {

	bool MatchesPathGlob(std::string_view pattern, std::string_view path)
	{
		// characters[t]: the bytes of the character that starts at path[t] (a well-formed UTF-8 sequence, or one byte of an
		// ill-formed one), or 0 inside one. Wildcards consume whole characters and match only at their starts, so '?' is one
		// code point and no '*' or "**" ends inside one; literal pattern bytes compare byte for byte, which for UTF-8 text
		// is the same as comparing code points.
		std::vector<uint8_t> characters(path.size(), 0);
		for (size_t t = 0; t < path.size();)
		{
			const size_t length = std::max<size_t>(GetUtf8SequenceLength(path, t), 1);
			characters[t] = static_cast<uint8_t>(length);
			t += length;
		}
		const auto startsCharacter = [&characters](size_t pathIndex)
		{
			return pathIndex == characters.size() || characters[pathIndex] != 0;
		};

		// matches[p * width + t]: whether pattern[p..] matches path[t..]. Rows are filled from the end of the pattern and
		// each row from the end of the path, every cell from cells already filled, so the work is (pattern + 1) x (path + 1)
		// steps whatever the stars, with no recursion and no backtracking.
		const size_t width = path.size() + 1;
		std::vector<uint8_t> matches((pattern.size() + 1) * width, 0);
		matches[pattern.size() * width + path.size()] = 1;
		const auto at = [&matches, width](size_t patternIndex, size_t pathIndex)
		{
			return matches[patternIndex * width + pathIndex] != 0;
		};
		for (size_t p = pattern.size(); p-- > 0;)
		{
			const char current = pattern[p];
			const bool doubleStar = current == '*' && p + 1 < pattern.size() && pattern[p + 1] == '*';
			const size_t afterStars = p + 2;
			// "**/" matches no directory at all, or any characters that end with '/'.
			const bool skipsDirectory = doubleStar && afterStars < pattern.size() && pattern[afterStars] == '/';
			// Whether the rest after "**" matches path[t'..] for some character start t' > t (the "**" consumes path[t..t')).
			bool restMatchesLater = false;
			for (size_t t = width; t-- > 0;)
			{
				bool matched = false;
				if (doubleStar)
				{
					if (startsCharacter(t))
					{
						const bool restMatches = at(afterStars, t) || restMatchesLater;
						restMatchesLater = restMatches;
						matched = restMatches || (skipsDirectory && at(afterStars + 1, t));
					}
				}
				else if (current == '*')
				{
					// '*' matches nothing here, or one more character but '/'.
					matched = startsCharacter(t) && (at(p + 1, t) || (t < path.size() && path[t] != '/' && at(p, t + characters[t])));
				}
				else if (current == '?')
				{
					matched = t < path.size() && characters[t] != 0 && path[t] != '/' && at(p + 1, t + characters[t]);
				}
				else if (t < path.size() && current == path[t])
				{
					matched = at(p + 1, t + 1);
				}
				matches[p * width + t] = matched ? uint8_t{ 1 } : uint8_t{ 0 };
			}
		}
		return at(0, 0);
	}

}
