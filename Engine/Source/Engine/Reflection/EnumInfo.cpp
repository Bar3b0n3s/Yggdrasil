#include "EnginePCH.h"
#include "Engine/Reflection/EnumInfo.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/FuzzySuggest.h"

#include <algorithm>

namespace Engine {

	namespace Utils {

		static bool EqualsIgnoringAsciiCase(std::string_view lhs, std::string_view rhs)
		{
			const auto fold = [](char character)
			{
				return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
			};
			return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [&fold](char left, char right)
			{
				return fold(left) == fold(right);
			});
		}

	}

	EnumInfo::EnumInfo(std::string name, std::string description)
		: m_Name(std::move(name)), m_Description(std::move(description))
	{
	}

	void EnumInfo::AddEntry(EnumEntry entry)
	{
		ENGINE_CORE_ASSERT(!entry.Name.empty(), "An entry of enum '{}' needs a name", m_Name);
		ENGINE_CORE_ASSERT(!entry.Description.empty(), "Entry '{}' of enum '{}' needs a description", entry.Name, m_Name);
		ENGINE_CORE_ASSERT(FindByName(entry.Name) == nullptr, "Enum '{}' already has an entry named '{}'", m_Name, entry.Name);
		ENGINE_CORE_ASSERT(FindByValue(entry.Value) == nullptr, "Enum '{}' already has an entry with value {}", m_Name, entry.Value);
		m_Entries.push_back(std::move(entry));
	}

	const EnumEntry* EnumInfo::FindByName(std::string_view name) const
	{
		for (const EnumEntry& entry : m_Entries)
		{
			if (entry.Name == name)
				return &entry;
		}
		return nullptr;
	}

	const EnumEntry* EnumInfo::FindByNameIgnoreCase(std::string_view name) const
	{
		for (const EnumEntry& entry : m_Entries)
		{
			if (Utils::EqualsIgnoringAsciiCase(entry.Name, name))
				return &entry;
		}
		return nullptr;
	}

	const EnumEntry* EnumInfo::FindByValue(int64_t value) const
	{
		for (const EnumEntry& entry : m_Entries)
		{
			if (entry.Value == value)
				return &entry;
		}
		return nullptr;
	}

	std::vector<std::string> EnumInfo::SuggestNames(std::string_view name, size_t maxResults) const
	{
		std::vector<std::string_view> names;
		names.reserve(m_Entries.size());
		for (const EnumEntry& entry : m_Entries)
			names.push_back(entry.Name);
		return FuzzySuggest(name, names, maxResults);
	}

}
