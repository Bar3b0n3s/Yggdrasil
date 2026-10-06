#include "EnginePCH.h"
#include "Engine/Core/UUID.h"

// M1 contract stub (Roadmap rule 3): stream B implements the text form and prefix matching.

namespace Engine {

	std::string UUID::ToString() const
	{
		return {};
	}

	bool UUID::MatchesPrefix(std::string_view /*prefix*/) const
	{
		return false;
	}

	std::optional<UUID> UUID::FromString(std::string_view /*text*/)
	{
		return std::nullopt;
	}

	bool UUID::IsValidPrefix(std::string_view /*prefix*/)
	{
		return false;
	}

}
