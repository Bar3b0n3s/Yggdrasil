#include "EnginePCH.h"
#include "Engine/Reflection/FuzzySuggest.h"

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements the edit distance and the suggestions.

namespace Engine {

	size_t LevenshteinDistance(std::string_view /*a*/, std::string_view /*b*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	size_t GetMaxSuggestionDistance(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::vector<std::string> FuzzySuggest(std::string_view /*name*/, std::span<const std::string_view> /*candidates*/, size_t /*maxResults*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<std::string> FuzzySuggest(std::string_view /*name*/, std::span<const std::string> /*candidates*/, size_t /*maxResults*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string MakeDidYouMeanHint(std::span<const std::string> /*suggestions*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
