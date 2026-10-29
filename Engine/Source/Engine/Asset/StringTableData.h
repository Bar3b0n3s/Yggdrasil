#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// String tables (M22, C22.1, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decision 10): the .stringtable file
// (AssetType::StringTable, format version 1), the reflected StringTableData struct behind it, its cooked payload and the
// loaded asset. A table is data only: its locales and its entries, each a key with one string per locale. Produced by
// StringTableImporter (.stringtable files) and asset.create; read by scripts through Loc.Get (Scripting's Loc module) in the
// session's locale (the project's Localization.Locale).
//
// Conventions, which every reader follows:
//   - Locales are tags such as "en", "en-US", "fr" or "ja": 1 to MaxStringTableLocaleBytes bytes of ASCII letters, digits
//     and '-', starting with a letter, unique ignoring ASCII case. The first is the table's fallback locale.
//   - An entry's identity is its Key: 1 to MaxStringTableKeyBytes bytes of valid UTF-8 without control characters, unique
//     (byte-wise). Its Values map a locale of Locales to its string (valid UTF-8, at most MaxStringTableValueBytes bytes; line
//     feeds and tabs allowed, no other control character); every entry has a value for the fallback locale, other locales are
//     optional.
//   - A lookup (FindLocalizedString) resolves the requested locale to one of the table's (ResolveStringTableLocale: the
//     table's locale equal to it ignoring ASCII case, else the one equal to its language, the part before its first '-',
//     else the fallback), then takes the entry's value for it, or the fallback's when the entry has none.
// The table references no asset, so its importer's Import.Dependencies is empty (ADR 0028's phase-2 ledger: every new
// importer fills them, here with nothing). Lookups are deterministic: byte-wise comparisons only, no locale database.

namespace Engine {

	class TypeRegistry;

	// The .stringtable document header (§6).
	inline constexpr std::string_view StringTableFormatName = "StringTable";
	inline constexpr uint32_t StringTableFormatVersion = 1;

	// The limits ValidateStringTable enforces, so a table external input controls cannot exhaust memory.
	inline constexpr uint32_t MaxStringTableLocales = 64;
	inline constexpr uint32_t MaxStringTableEntries = 65536;
	inline constexpr size_t MaxStringTableLocaleBytes = 35; // BCP 47's longest common tags fit
	inline constexpr size_t MaxStringTableKeyBytes = 256;
	inline constexpr size_t MaxStringTableValueBytes = 16384;

	// Registry struct "StringTableEntry": one key and its strings by locale.
	struct StringTableEntry
	{
		std::string Key{};
		std::map<std::string, std::string> Values{}; // locale -> string; written sorted by locale, as every Map field

		bool operator==(const StringTableEntry&) const = default;
	};

	// A string table (AssetType::StringTable), registry struct "StringTable". Every field is serialized under its member name,
	// in member order, after "Format": "StringTable" and "Version": 1. Plain data; immutable once loaded
	// (AssetRef<StringTableData>). There is no placeholder table: a lookup in a missing or failed table is the reader's error.
	struct StringTableData : Asset
	{
		static constexpr AssetType StaticType = AssetType::StringTable;
		// The cooked payload layout below (CookedHeader::FormatVersion).
		static constexpr uint16_t FormatVersion = 1;

		StringTableData()
			: Asset(StaticType)
		{
		}

		// 1 to MaxStringTableLocales; the first is the fallback. The default, "en" alone, is a valid empty table (asset.create
		// without values, the registry suite's defaults).
		std::vector<std::string> Locales{ "en" };
		std::vector<StringTableEntry> Entries{}; // at most MaxStringTableEntries; authored order in a file, sorted by Key once cooked
	};

	// Registers the structs "StringTableEntry" and "StringTable" (StringTableData) with the rules across fields
	// (ValidateStringTable) as Validate hooks, and Generate hooks that keep the registry suite's random objects valid.
	// RegisterAssetTypes calls it, so tables read and write in the editor and in exported games alike.
	void RegisterStringTableTypes(TypeRegistry& registry);

	// Checks every rule of the file comment: 1 to MaxStringTableLocales locales, each a valid tag unique ignoring ASCII case;
	// at most MaxStringTableEntries entries, each Key valid and unique, each Values key one of Locales, each value valid, the
	// fallback locale's value present (missing: at the entry's "/Values"). Pure. Errors: Validation with one issue per
	// violation, in document order (the locales, then each entry's Key and its Values by locale), each located by its JSON
	// pointer into the .stringtable document ("/Locales/2", "/Entries/5/Key", "/Entries/5/Values/fr").
	[[nodiscard]] Status ValidateStringTable(const StringTableData& table);

	// The index in table.Locales of the locale a lookup in `locale` reads (see the file comment): an exact match ignoring ASCII
	// case, else the language's match, else 0 (the fallback; also for an empty locale). The table has at least one locale
	// (asserted). Pure.
	[[nodiscard]] uint32_t ResolveStringTableLocale(const StringTableData& table, std::string_view locale);

	// The entry whose Key is `key`, by binary search: `table.Entries` must be sorted by Key, byte-wise, as every loaded table's
	// are (the cooked payload's order); the result for an unsorted table is unspecified (never out of bounds). nullptr when
	// there is none. Pure.
	[[nodiscard]] const StringTableEntry* FindStringTableEntry(const StringTableData& table, std::string_view key);

	// The string of `key` in `locale` (see the file comment), or nullptr when the table has no entry `key`. As
	// FindStringTableEntry, for a table sorted by Key. Pure.
	[[nodiscard]] const std::string* FindLocalizedString(const StringTableData& table, std::string_view key, std::string_view locale);

	// The assets `table` references: none (the file comment). Its importer's Import.Dependencies.
	[[nodiscard]] std::vector<AssetHandle> GetStringTableDependencies(const StringTableData& table);

	// What a table load reports besides its result: the file's version and its warnings (unknown members) in document order.
	struct StringTableLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics{};
	};

	// The canonical .stringtable document ("Format", "Version", then the registry fields in member order, every field written,
	// §6). Errors: Validation for a value that cannot be written, located. Requires a frozen registry on which
	// RegisterStringTableTypes ran (asserted; InvalidState without asserts).
	[[nodiscard]] Result<Json> StringTableToJson(const StringTableData& table, const TypeRegistry& registry);

	// The canonical text of StringTableToJson: Pretty for .stringtable files.
	[[nodiscard]] Result<std::string> StringTableToText(const StringTableData& table, const TypeRegistry& registry, JsonStyle style = JsonStyle::Pretty);

	// Reads a .stringtable document strictly through the registry (StructInfo::FromJson of "StringTable": missing keys keep
	// their defaults, unknown keys warn into `report`, or fail with `strictUnknowns`, every value validated), then
	// ValidateStringTable. Errors: Validation (located, every bad field an issue) for a wrong "Format", a version below 1, a
	// wrong JSON type or a ValidateStringTable violation; UnsupportedVersion naming both versions for a newer file.
	[[nodiscard]] Result<StringTableData> StringTableFromJson(const Json& document, const TypeRegistry& registry, StringTableLoadReport& report,
		bool strictUnknowns = false);

	// JsonReader::Parse, then StringTableFromJson. Errors: Parse (line and column), and as StringTableFromJson.
	[[nodiscard]] Result<StringTableData> StringTableFromText(std::string_view text, const TypeRegistry& registry, StringTableLoadReport& report,
		bool strictUnknowns = false);

	// The cooked string table payload (FormatVersion 1), little-endian, strings as uint32 byte count then UTF-8 bytes:
	//     uint32 LocaleCount; LocaleCount x string Locale;
	//     uint32 EntryCount;
	//     EntryCount x { string Key; LocaleCount x { uint8 Present (0 or 1); Present ? string Value : nothing } }
	// Locales in the table's order; entries sorted by Key, byte-wise (so FindStringTableEntry binary-searches every loaded
	// table). Asserts ValidateStringTable. Pure; identical tables give identical bytes in every configuration.
	[[nodiscard]] Buffer SerializeStringTablePayload(const StringTableData& table);

	// Reads a payload written by SerializeStringTablePayload, checking every count against its limit and against the bytes left
	// before allocating, then ValidateStringTable. Never asserts on data (fuzzed: "StringTableData: 1,000 seeded mutations of a
	// string table file and of its cooked form never crash"). Errors: Parse for truncation, a count above its limit, a Present
	// byte other than 0 or 1, entries out of Key order, or trailing bytes; Validation from ValidateStringTable.
	[[nodiscard]] Result<StringTableData> DeserializeStringTablePayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `table`. Asserts ValidateStringTable.
	[[nodiscard]] Buffer CookStringTable(const StringTableData& table, uint32_t importerVersion);

	// The table of a cooked artifact (both asset managers' loader for AssetType::StringTable, RegisterBuiltinLoaders). Errors:
	// as ReadCookedArtifact and DeserializeStringTablePayload.
	[[nodiscard]] Result<AssetRef<StringTableData>> LoadCookedStringTable(std::span<const std::byte> cooked);

}
