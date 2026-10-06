#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	// One enumerator of a reflected enum: its PascalCase name (the only spelling files contain, §6), its integer value
	// and its mandatory description.
	struct EnumEntry
	{
		std::string Name;
		int64_t Value = 0;
		std::string Description;
	};

	// A reflected enum (Architecture §5.4): the name <-> value table of a registered C++ enum (TypeRegistry::Enum<E>), or
	// of a script-declared Field.Enum({...}) schema (M13, created by its schema owner). Enums serialize by name.
	//
	// Entries keep registration order, which is the order of the JSON Schema "enum" list, the inspector combo box and the
	// generated documentation. Names are unique and non-empty, values unique, descriptions non-empty (asserted when added).
	// Registration happens on one thread before TypeRegistry::Freeze; afterwards every const member is thread-safe.
	class EnumInfo
	{
	public:
		EnumInfo(std::string name, std::string description);

		[[nodiscard]] const std::string& GetName() const { return m_Name; }
		[[nodiscard]] const std::string& GetDescription() const { return m_Description; }
		[[nodiscard]] std::span<const EnumEntry> GetEntries() const { return m_Entries; }

		// Appends an entry (registration only). Asserts a non-empty unique name, a unique value and a non-empty
		// description: a violation is a programmer error in a registration file.
		void AddEntry(EnumEntry entry);

		// The entry named exactly `name` (case-sensitive: authored files, §6), or nullptr.
		[[nodiscard]] const EnumEntry* FindByName(std::string_view name) const;

		// The entry whose name equals `name` ignoring ASCII case (automation and script input, §6, §13.4), or nullptr.
		// Responses then echo the canonical spelling, entry->Name.
		[[nodiscard]] const EnumEntry* FindByNameIgnoreCase(std::string_view name) const;

		// The entry with `value`, or nullptr.
		[[nodiscard]] const EnumEntry* FindByValue(int64_t value) const;

		// Up to `maxResults` entry names close to `name`, best first (FuzzySuggest), for "did you mean" hints of an unknown
		// enum name. Empty when nothing is close.
		[[nodiscard]] std::vector<std::string> SuggestNames(std::string_view name, size_t maxResults = 3) const;
	private:
		std::string m_Name;
		std::string m_Description;
		std::vector<EnumEntry> m_Entries;
	};

}
