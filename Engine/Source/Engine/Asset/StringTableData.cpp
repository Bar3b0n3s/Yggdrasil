#include "EnginePCH.h"
#include "Engine/Asset/StringTableData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <set>
#include <utility>

namespace Engine {

	namespace Utils {

		// The oldest readable .stringtable version (there has been no other yet).
		static constexpr uint32_t MinimumStringTableVersion = 1;
		// The fewest payload bytes a string takes (its uint32 length) and a presence byte; DeserializeStringTablePayload checks
		// every count against these before it allocates.
		static constexpr uint64_t MinStringPayloadBytes = 4;
		static constexpr uint64_t PresencePayloadBytes = 1;
		// The fallback string a generated entry gets when it has none (GenerateStringTable).
		static constexpr std::string_view GeneratedFallbackValue = "Text";

		// An Error at <context's pointer><relative> (`relative` empty or an already escaped pointer suffix).
		static void AddTableIssue(ValidationContext& context, std::string_view relative, std::string message, std::string hint = {})
		{
			ValidationIssue issue;
			issue.Severity = DiagnosticSeverity::Error;
			issue.JsonPointer = context.GetPointer() + std::string(relative);
			issue.Message = std::move(message);
			issue.Hint = std::move(hint);
			context.AddIssue(std::move(issue));
		}

		[[nodiscard]] static bool IsAsciiLetter(char character)
		{
			return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
		}

		[[nodiscard]] static bool IsAsciiDigit(char character)
		{
			return character >= '0' && character <= '9';
		}

		[[nodiscard]] static char ToAsciiLower(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// Whether `a` and `b` are equal ignoring ASCII case (byte-wise otherwise).
		[[nodiscard]] static bool EqualsIgnoringAsciiCase(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && std::ranges::equal(a, b, [](char left, char right)
			{
				return ToAsciiLower(left) == ToAsciiLower(right);
			});
		}

		// The control character `text` holds, other than those `allowed` lists, as its byte offset: a C0 control, DEL, or a
		// C1 control (U+0080 to U+009F, encoded as C2 80 to C2 9F), the rule of sprite sheet names too. `text` is valid UTF-8.
		[[nodiscard]] static std::optional<size_t> FindControlCharacter(std::string_view text, std::string_view allowed)
		{
			for (size_t index = 0; index < text.size(); ++index)
			{
				const auto byte = static_cast<unsigned char>(text[index]);
				if ((byte < 0x20 || byte == 0x7f) && allowed.find(text[index]) == std::string_view::npos)
					return index;
				if (byte == 0xc2 && index + 1 < text.size())
				{
					const auto next = static_cast<unsigned char>(text[index + 1]);
					if (next >= 0x80 && next <= 0x9f)
						return index;
				}
			}
			return std::nullopt;
		}

		// A key's problem (1 to MaxStringTableKeyBytes bytes of valid UTF-8 without control characters), or none.
		[[nodiscard]] static std::optional<std::string> FindKeyProblem(std::string_view key)
		{
			if (key.empty())
				return std::string("is empty");
			if (key.size() > MaxStringTableKeyBytes)
				return std::format("is {} bytes long, more than the {} a key may have", key.size(), MaxStringTableKeyBytes);
			if (!IsValidUtf8(key))
				return std::string("is not valid UTF-8");
			if (const std::optional<size_t> control = FindControlCharacter(key, {}))
				return std::format("holds a control character at byte {}", *control);
			return std::nullopt;
		}

		// A value's problem (valid UTF-8 of at most MaxStringTableValueBytes bytes; line feeds and tabs are its only control
		// characters), or none.
		[[nodiscard]] static std::optional<std::string> FindValueProblem(std::string_view value)
		{
			if (value.size() > MaxStringTableValueBytes)
				return std::format("is {} bytes long, more than the {} a string may have", value.size(), MaxStringTableValueBytes);
			if (!IsValidUtf8(value))
				return std::string("is not valid UTF-8");
			if (const std::optional<size_t> control = FindControlCharacter(value, "\n\t"))
				return std::format("holds a control character at byte {} (line feeds and tabs are the only ones a string may hold)", *control);
			return std::nullopt;
		}

		// Every rule of ValidateStringTable, located below the context's pointer, in document order: the locales, then each
		// entry's Key and its Values by locale (up to the count limits). A duplicate is reported at its later occurrence.
		static void ValidateTable(const StringTableData& table, ValidationContext& context)
		{
			if (table.Locales.empty())
				AddTableIssue(context, "/Locales", "the table has no locale", "list at least one locale, such as [\"en\"]; the first is the fallback");
			else if (table.Locales.size() > MaxStringTableLocales)
				AddTableIssue(context, "/Locales", std::format("the table has {} locales, more than the {} a table may have", table.Locales.size(), MaxStringTableLocales));
			const size_t locales = std::min<size_t>(table.Locales.size(), MaxStringTableLocales);
			for (size_t index = 0; index < locales; ++index)
			{
				const std::string& locale = table.Locales[index];
				const std::string pointer = JsonReader::AppendPointer("/Locales", index);
				if (!IsLocaleTag(locale))
				{
					AddTableIssue(context, pointer, std::format("locale {} ('{}') is not a locale tag", index, locale),
						std::format("a locale is 1 to {} ASCII letters, digits and '-', starting with a letter, such as en, fr or pt-BR", MaxStringTableLocaleBytes));
					continue;
				}
				const auto earlier = std::ranges::find_if(table.Locales.begin(), table.Locales.begin() + static_cast<std::ptrdiff_t>(index), [&locale](const std::string& other)
				{
					return EqualsIgnoringAsciiCase(other, locale);
				});
				if (earlier != table.Locales.begin() + static_cast<std::ptrdiff_t>(index))
				{
					AddTableIssue(context, pointer, std::format("locale {} ('{}') repeats the locale '{}' (locales are compared ignoring ASCII case)", index, locale, *earlier),
						"each locale appears once: remove the repeated one");
				}
			}

			if (table.Entries.size() > MaxStringTableEntries)
				AddTableIssue(context, "/Entries", std::format("the table has {} entries, more than the {} a table may have", table.Entries.size(), MaxStringTableEntries));
			const std::string_view fallback = table.Locales.empty() ? std::string_view() : std::string_view(table.Locales.front());
			std::set<std::string_view> keys;
			const size_t entries = std::min<size_t>(table.Entries.size(), MaxStringTableEntries);
			for (size_t index = 0; index < entries; ++index)
			{
				const StringTableEntry& entry = table.Entries[index];
				const std::string base = JsonReader::AppendPointer("/Entries", index);
				if (const std::optional<std::string> problem = FindKeyProblem(entry.Key))
				{
					AddTableIssue(context, base + "/Key", std::format("entry {}'s Key {}", index, *problem),
						std::format("a key is 1 to {} bytes of text without control characters, such as Menu.Start", MaxStringTableKeyBytes));
				}
				else if (!keys.insert(entry.Key).second)
					AddTableIssue(context, base + "/Key", std::format("entry {}'s Key '{}' is the key of an earlier entry", index, entry.Key), "keys are unique: rename or remove the entry");

				const std::string values = base + "/Values";
				if (!fallback.empty() && !entry.Values.contains(std::string(fallback)))
				{
					AddTableIssue(context, values, std::format("entry {} has no string for the fallback locale '{}'", index, fallback),
						"every entry has a string for the table's first locale, which every other locale falls back to");
				}
				for (const auto& [locale, value] : entry.Values)
				{
					const std::string pointer = JsonReader::AppendPointer(values, locale);
					if (std::ranges::find(table.Locales, locale) == table.Locales.end())
					{
						AddTableIssue(context, pointer, std::format("entry {} has a string for '{}', which is not one of the table's Locales", index, locale),
							"add the locale to Locales, or remove the string (a Values key is spelled exactly as in Locales)");
					}
					if (const std::optional<std::string> problem = FindValueProblem(value))
						AddTableIssue(context, pointer, std::format("entry {}'s string for '{}' {}", index, locale, *problem));
				}
			}
		}

		// The "StringTable" Validate hook: ValidateStringTable on the whole table.
		static void ValidateStringTableHook(const StringTableData& table, ValidationContext& context)
		{
			ValidateTable(table, context);
		}

		// `text` reduced to a locale tag: its ASCII letters, digits and '-' only, starting with a letter ('x' is prepended
		// otherwise), at most MaxStringTableLocaleBytes bytes.
		[[nodiscard]] static std::string MakeLocaleTag(std::string_view text)
		{
			std::string tag;
			for (const char character : text)
			{
				if (IsAsciiLetter(character) || IsAsciiDigit(character) || character == '-')
					tag += character;
			}
			if (tag.empty() || !IsAsciiLetter(tag.front()))
				tag.insert(tag.begin(), 'x');
			tag.resize(std::min(tag.size(), MaxStringTableLocaleBytes));
			return tag;
		}

		// The "StringTable" Generate hook: random locales become unique tags (at least one), random keys valid and unique
		// (random text is printable ASCII), and every entry keeps only strings of the table's locales, with one for the
		// fallback.
		static void GenerateStringTable(StringTableData& table, Random& /*random*/)
		{
			std::vector<std::string> locales;
			for (const std::string& candidate : table.Locales)
			{
				std::string tag = MakeLocaleTag(candidate);
				const bool taken = std::ranges::any_of(locales, [&tag](const std::string& other)
				{
					return EqualsIgnoringAsciiCase(other, tag);
				});
				if (!taken && locales.size() < MaxStringTableLocales)
					locales.push_back(std::move(tag));
			}
			if (locales.empty())
				locales.emplace_back("en");
			table.Locales = std::move(locales);

			table.Entries.resize(std::min<size_t>(table.Entries.size(), MaxStringTableEntries));
			std::set<std::string> keys;
			for (StringTableEntry& entry : table.Entries)
			{
				if (FindKeyProblem(entry.Key).has_value())
					entry.Key = "Key";
				while (keys.contains(entry.Key))
					entry.Key += '#';
				keys.insert(entry.Key);
				std::erase_if(entry.Values, [&table](const auto& value)
				{
					return std::ranges::find(table.Locales, value.first) == table.Locales.end() || FindValueProblem(value.second).has_value();
				});
				entry.Values.try_emplace(table.Locales.front(), GeneratedFallbackValue);
			}
		}

		// The registered "StringTable" struct. A registry without it is a programmer error (asserted); the error return keeps
		// builds without asserts safe.
		[[nodiscard]] static Result<const StructInfo*> FindStringTableType(const TypeRegistry& registry)
		{
			ENGINE_CORE_ASSERT(registry.IsFrozen(), "String tables need a frozen type registry");
			const StructInfo* type = registry.FindStruct<StringTableData>();
			ENGINE_CORE_ASSERT(type != nullptr, "String tables need a registry on which RegisterStringTableTypes ran");
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no StringTable struct (RegisterStringTableTypes was not called)");
			return type;
		}

		[[nodiscard]] static Result<StringTableData> ReadStringTableDocument(const Json& document, const TypeRegistry& registry, StringTableLoadReport& report,
			bool strictUnknowns)
		{
			ENGINE_TRY_ASSIGN(const StructInfo* type, FindStringTableType(registry));
			const JsonReader root(document);
			ENGINE_TRY_ASSIGN(report.FileVersion, root.ReadFormatHeader(StringTableFormatName, MinimumStringTableVersion, StringTableFormatVersion));

			// Everything below the header is the StringTable object; the header check above guarantees an object.
			Json fields = document;
			fields.erase("Format");
			fields.erase("Version");

			StringTableData table;
			ReadContext context;
			context.Strict = strictUnknowns;
			context.Diagnostics = &report.Diagnostics;
			ENGINE_TRY(type->FromJson(&table, JsonReader(fields), context));
			ENGINE_TRY(ValidateStringTable(table));
			return table;
		}

		// A payload count `count` (`what`: "entries") that must be at most `limit` and leave room for `minimumBytes` bytes per
		// item in what follows. Errors: Parse.
		[[nodiscard]] static Status CheckPayloadCount(const BinaryReader& reader, uint64_t count, uint64_t limit, uint64_t minimumBytes, std::string_view what)
		{
			if (count > limit)
				return MakeError(ErrorCode::Parse, "string table payload: {} {} at offset {}, more than the {} a table may have", count, what, reader.GetPosition(), limit);
			if (count * minimumBytes > reader.GetRemaining())
			{
				return MakeError(ErrorCode::Parse, "string table payload: {} {} need at least {} bytes, but {} follow at offset {}", count, what, count * minimumBytes,
					reader.GetRemaining(), reader.GetPosition());
			}
			return {};
		}

		// The entries of `table` sorted by Key, byte-wise: the cooked payload's order.
		[[nodiscard]] static std::vector<const StringTableEntry*> SortEntriesByKey(const StringTableData& table)
		{
			std::vector<const StringTableEntry*> sorted;
			sorted.reserve(table.Entries.size());
			for (const StringTableEntry& entry : table.Entries)
				sorted.push_back(&entry);
			std::ranges::sort(sorted, [](const StringTableEntry* left, const StringTableEntry* right)
			{
				return std::string_view(left->Key) < std::string_view(right->Key);
			});
			return sorted;
		}

	}

	void RegisterStringTableTypes(TypeRegistry& registry)
	{
		// The rules across fields (tags, unique locales and keys, each string's locale and text, the fallback's string, the
		// counts) are ValidateStringTable's, run as the table's Validate hook.
		registry.Struct<StringTableEntry>("StringTableEntry", "One key and its strings by locale.")
			.Field("Key", &StringTableEntry::Key,
				std::format("The entry's identity, which Loc.Get looks up: 1 to {} bytes of text without control characters, unique in the table.", MaxStringTableKeyBytes))
			.Field("Values", &StringTableEntry::Values,
				std::format("The entry's strings by locale (each key one of the table's Locales, written sorted): at most {} bytes of UTF-8 each, line feeds "
							"and tabs their only control characters; the fallback locale's string is required, the others optional.",
					MaxStringTableValueBytes));

		registry.Struct<StringTableData>("StringTable", "A string table: locales and keyed strings (.stringtable).")
			.Field("Locales", &StringTableData::Locales,
				std::format("1 to {} locale tags (1 to {} ASCII letters, digits and '-', starting with a letter, such as en, fr or pt-BR), unique ignoring "
							"ASCII case; the first is the fallback locale.",
					MaxStringTableLocales, MaxStringTableLocaleBytes))
			.Field("Entries", &StringTableData::Entries, std::format("At most {} entries, in authored order (sorted by Key once cooked).", MaxStringTableEntries))
			.Validate(&Utils::ValidateStringTableHook)
			.Generate(&Utils::GenerateStringTable);
	}

	Status ValidateStringTable(const StringTableData& table)
	{
		ValidationContext context;
		Utils::ValidateTable(table, context);
		return context.ToStatus("the string table");
	}

	bool IsLocaleTag(std::string_view locale)
	{
		if (locale.empty() || locale.size() > MaxStringTableLocaleBytes || !Utils::IsAsciiLetter(locale.front()))
			return false;
		return std::ranges::all_of(locale, [](char character)
		{
			return Utils::IsAsciiLetter(character) || Utils::IsAsciiDigit(character) || character == '-';
		});
	}

	uint32_t ResolveStringTableLocale(const StringTableData& table, std::string_view locale)
	{
		ENGINE_CORE_ASSERT(!table.Locales.empty(), "A string table has at least one locale");
		if (locale.empty())
			return 0;
		const auto findLocale = [&table](std::string_view wanted) -> std::optional<uint32_t>
		{
			for (size_t index = 0; index < table.Locales.size(); ++index)
				if (Utils::EqualsIgnoringAsciiCase(table.Locales[index], wanted))
					return static_cast<uint32_t>(index);
			return std::nullopt;
		};
		if (const std::optional<uint32_t> exact = findLocale(locale))
			return *exact;
		if (const std::optional<uint32_t> language = findLocale(locale.substr(0, locale.find('-'))))
			return *language;
		return 0;
	}

	const StringTableEntry* FindStringTableEntry(const StringTableData& table, std::string_view key)
	{
		const auto found = std::ranges::lower_bound(table.Entries, key, std::ranges::less{}, [](const StringTableEntry& entry)
		{
			return std::string_view(entry.Key);
		});
		if (found == table.Entries.end() || found->Key != key)
			return nullptr;
		return &*found;
	}

	const std::string* FindLocalizedString(const StringTableData& table, std::string_view key, std::string_view locale)
	{
		const StringTableEntry* entry = FindStringTableEntry(table, key);
		if (entry == nullptr || table.Locales.empty())
			return nullptr;
		const uint32_t index = ResolveStringTableLocale(table, locale);
		if (const auto value = entry->Values.find(table.Locales[index]); value != entry->Values.end())
			return &value->second;
		if (const auto fallback = entry->Values.find(table.Locales.front()); fallback != entry->Values.end())
			return &fallback->second;
		return nullptr;
	}

	std::vector<AssetHandle> GetStringTableDependencies(const StringTableData& /*table*/)
	{
		return {};
	}

	Result<Json> StringTableToJson(const StringTableData& table, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const StructInfo* type, Utils::FindStringTableType(registry));
		ENGINE_TRY_ASSIGN(Json fields, type->ToJson(&table));

		// The header first, then the fields in registry order (§6).
		Json document = Json::object();
		document["Format"] = std::string(StringTableFormatName);
		document["Version"] = StringTableFormatVersion;
		for (auto field = fields.begin(); field != fields.end(); ++field)
			document[field.key()] = std::move(*field);
		return document;
	}

	Result<std::string> StringTableToText(const StringTableData& table, const TypeRegistry& registry, JsonStyle style)
	{
		ENGINE_TRY_ASSIGN(const Json document, StringTableToJson(table, registry));
		return JsonWriter::Write(document, style);
	}

	Result<StringTableData> StringTableFromJson(const Json& document, const TypeRegistry& registry, StringTableLoadReport& report, bool strictUnknowns)
	{
		report = StringTableLoadReport{};
		return Utils::ReadStringTableDocument(document, registry, report, strictUnknowns);
	}

	Result<StringTableData> StringTableFromText(std::string_view text, const TypeRegistry& registry, StringTableLoadReport& report, bool strictUnknowns)
	{
		report = StringTableLoadReport{};
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		return StringTableFromJson(document, registry, report, strictUnknowns);
	}

	Buffer SerializeStringTablePayload(const StringTableData& table)
	{
		ENGINE_CORE_ASSERT(ValidateStringTable(table).has_value(), "Only a valid string table is cooked");
		BinaryWriter writer;
		writer.WriteU32(static_cast<uint32_t>(table.Locales.size()));
		for (const std::string& locale : table.Locales)
			writer.WriteString(locale);
		writer.WriteU32(static_cast<uint32_t>(table.Entries.size()));
		for (const StringTableEntry* entry : Utils::SortEntriesByKey(table))
		{
			writer.WriteString(entry->Key);
			for (const std::string& locale : table.Locales)
			{
				const auto value = entry->Values.find(locale);
				if (value == entry->Values.end())
				{
					writer.WriteU8(0);
					continue;
				}
				writer.WriteU8(1);
				writer.WriteString(value->second);
			}
		}
		return writer.TakeBuffer();
	}

	Result<StringTableData> DeserializeStringTablePayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		StringTableData table;
		ENGINE_TRY_ASSIGN(const uint32_t localeCount, reader.ReadU32());
		ENGINE_TRY(Utils::CheckPayloadCount(reader, localeCount, MaxStringTableLocales, Utils::MinStringPayloadBytes, "locales"));
		table.Locales.clear();
		table.Locales.reserve(localeCount);
		for (uint32_t index = 0; index < localeCount; ++index)
		{
			ENGINE_TRY_ASSIGN(std::string locale, reader.ReadString());
			table.Locales.push_back(std::move(locale));
		}

		ENGINE_TRY_ASSIGN(const uint32_t entryCount, reader.ReadU32());
		// An entry is at least its key's length and one presence byte per locale.
		const uint64_t minEntryBytes = Utils::MinStringPayloadBytes + static_cast<uint64_t>(localeCount) * Utils::PresencePayloadBytes;
		ENGINE_TRY(Utils::CheckPayloadCount(reader, entryCount, MaxStringTableEntries, minEntryBytes, "entries"));
		table.Entries.reserve(entryCount);
		for (uint32_t index = 0; index < entryCount; ++index)
		{
			StringTableEntry entry;
			const size_t keyOffset = reader.GetPosition();
			ENGINE_TRY_ASSIGN(entry.Key, reader.ReadString());
			if (!table.Entries.empty() && !(std::string_view(table.Entries.back().Key) < std::string_view(entry.Key)))
			{
				return MakeError(ErrorCode::Parse, "string table payload: entry {}'s key at offset {} does not follow entry {}'s in byte-wise order", index, keyOffset,
					index - 1);
			}
			for (const std::string& locale : table.Locales)
			{
				const size_t presenceOffset = reader.GetPosition();
				ENGINE_TRY_ASSIGN(const uint8_t present, reader.ReadU8());
				if (present > 1)
					return MakeError(ErrorCode::Parse, "string table payload: the presence byte at offset {} is {} instead of 0 or 1", presenceOffset, present);
				if (present == 0)
					continue;
				ENGINE_TRY_ASSIGN(std::string value, reader.ReadString());
				entry.Values.insert_or_assign(locale, std::move(value));
			}
			table.Entries.push_back(std::move(entry));
		}
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "string table payload has {} trailing bytes after offset {}", reader.GetRemaining(), reader.GetPosition());
		ENGINE_TRY(ValidateStringTable(table));
		return table;
	}

	Buffer CookStringTable(const StringTableData& table, uint32_t importerVersion)
	{
		const Buffer payload = SerializeStringTablePayload(table);
		return WriteCookedArtifact(AssetType::StringTable, StringTableData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<StringTableData>> LoadCookedStringTable(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::StringTable, StringTableData::FormatVersion));
		ENGINE_TRY_ASSIGN(StringTableData table, DeserializeStringTablePayload(view.Payload));
		return AssetRef<StringTableData>(CreateRef<StringTableData>(std::move(table)));
	}

}
