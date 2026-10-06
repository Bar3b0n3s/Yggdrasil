#include "EnginePCH.h"
#include "Engine/Reflection/EnumInfo.h"

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements the enum tables.

namespace Engine {

	EnumInfo::EnumInfo(std::string name, std::string description)
		: m_Name(std::move(name)), m_Description(std::move(description))
	{
	}

	void EnumInfo::AddEntry(EnumEntry /*entry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	const EnumEntry* EnumInfo::FindByName(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const EnumEntry* EnumInfo::FindByNameIgnoreCase(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const EnumEntry* EnumInfo::FindByValue(int64_t /*value*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<std::string> EnumInfo::SuggestNames(std::string_view /*name*/, size_t /*maxResults*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
