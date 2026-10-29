#include "TestsPCH.h"

#include "Engine/Asset/StringTableData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/Fuzz.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// String tables (M22, C22.1, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decision 10): the .stringtable file, its rules,
// the locale resolution of a lookup, the cooked payload and its fuzz target. Skipped by the contract; stream L un-skips them.

namespace Engine {

	namespace {

		// Three locales; "Menu.Start" in all of them, "Menu.Quit" only in the fallback. Authored out of key order, so the cooked
		// form's sort is observable.
		StringTableData MakeMenuTable()
		{
			StringTableData table;
			table.Locales = { "en", "fr", "pt-BR" };
			table.Entries = {
				StringTableEntry{ .Key = "Menu.Start", .Values = { { "en", "Start" }, { "fr", "D\xC3\xA9marrer" }, { "pt-BR", "Iniciar" } } },
				StringTableEntry{ .Key = "Menu.Quit", .Values = { { "en", "Quit" } } },
			};
			return table;
		}

		// The first issue's pointer of a Validation error.
		std::string FirstPointer(const Error& error)
		{
			return error.GetIssues().empty() ? std::string() : error.GetIssues().front().JsonPointer;
		}

		void CheckInvalid(std::string_view pointer, const std::function<void(StringTableData&)>& change)
		{
			CAPTURE(pointer);
			StringTableData table = MakeMenuTable();
			change(table);
			const Status status = ValidateStringTable(table);
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			CHECK(FirstPointer(status.error()) == pointer);
		}

		// MakeMenuTable sorted by key, as a cooked table is.
		StringTableData MakeSortedMenuTable()
		{
			StringTableData table = MakeMenuTable();
			std::swap(table.Entries[0], table.Entries[1]);
			return table;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("StringTableData: a .stringtable file round-trips byte-identically" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StringTableData table = MakeMenuTable();
			const Result<std::string> text = StringTableToText(table, *registry);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(text->contains("\"Format\": \"StringTable\""));
			StringTableLoadReport report;
			const Result<StringTableData> read = StringTableFromText(*text, *registry, report, true);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(report.FileVersion == StringTableFormatVersion);
			CHECK(report.Diagnostics.empty());
			CHECK(read->Locales == table.Locales);
			CHECK(read->Entries == table.Entries); // a file keeps its authored order
			const Result<std::string> again = StringTableToText(*read, *registry);
			REQUIRE(again.has_value());
			CHECK(*again == *text);
			CHECK(GetStringTableDependencies(*read).empty());
		}

		TEST_CASE("StringTableData: invalid locales, keys and values are located Validation errors" * doctest::skip(true))
		{
			REQUIRE(ValidateStringTable(MakeMenuTable()).has_value());
			CheckInvalid("/Locales", [](StringTableData& table)
			{
				table.Locales.clear();
			});
			CheckInvalid("/Locales/1", [](StringTableData& table)
			{
				table.Locales[1] = "f r";
			});
			CheckInvalid("/Locales/2", [](StringTableData& table)
			{
				table.Locales[2] = "EN"; // a duplicate of "en", ignoring ASCII case
			});
			CheckInvalid("/Entries/1/Key", [](StringTableData& table)
			{
				table.Entries[1].Key = "Menu.Start";
			});
			CheckInvalid("/Entries/0/Key", [](StringTableData& table)
			{
				table.Entries[0].Key.clear();
			});
			CheckInvalid("/Entries/1/Values", [](StringTableData& table)
			{
				table.Entries[1].Values = { { "fr", "Quitter" } }; // no value for the fallback locale
			});
			CheckInvalid("/Entries/1/Values/de", [](StringTableData& table)
			{
				table.Entries[1].Values.emplace("de", "Beenden");
			});
			CheckInvalid("/Entries/0/Values/en", [](StringTableData& table)
			{
				table.Entries[0].Values["en"] = std::string("St\x01"
															"art");
			});
			CheckInvalid("/Entries/0/Values/fr", [](StringTableData& table)
			{
				table.Entries[0].Values["fr"] = std::string(MaxStringTableValueBytes + 1, 'x');
			});
		}

		TEST_CASE("StringTableData: a lookup takes the locale, then its language, then the fallback, and the fallback's string for a missing one" * doctest::skip(true))
		{
			const StringTableData table = MakeSortedMenuTable();
			CHECK(ResolveStringTableLocale(table, "fr") == 1);
			CHECK(ResolveStringTableLocale(table, "PT-br") == 2); // ignoring ASCII case
			CHECK(ResolveStringTableLocale(table, "fr-CA") == 1); // its language
			CHECK(ResolveStringTableLocale(table, "de") == 0);    // the fallback
			CHECK(ResolveStringTableLocale(table, "") == 0);
			const std::string* start = FindLocalizedString(table, "Menu.Start", "fr-CA");
			REQUIRE(start != nullptr);
			CHECK(*start == "D\xC3\xA9marrer");
			const std::string* quit = FindLocalizedString(table, "Menu.Quit", "fr");
			REQUIRE(quit != nullptr);
			CHECK(*quit == "Quit"); // no French string: the fallback's
			CHECK(FindLocalizedString(table, "Menu.Missing", "en") == nullptr);
			REQUIRE(FindStringTableEntry(table, "Menu.Start") != nullptr);
			CHECK(FindStringTableEntry(table, "Menu.Start")->Key == "Menu.Start");
		}

		TEST_CASE("StringTableData: the cooked payload is the documented little-endian layout, entries sorted by key" * doctest::skip(true))
		{
			// The bytes StringTableData.h documents, written here independently of the serializer, so the Debug and Release unit
			// runs both prove that a table cooks to the same bytes in every configuration.
			const StringTableData table = MakeMenuTable();
			BinaryWriter expected;
			expected.WriteU32(3);
			for (const std::string& locale : table.Locales)
				expected.WriteString(locale);
			expected.WriteU32(2);
			// "Menu.Quit" sorts before "Menu.Start".
			expected.WriteString("Menu.Quit");
			expected.WriteU8(1);
			expected.WriteString("Quit");
			expected.WriteU8(0);
			expected.WriteU8(0);
			expected.WriteString("Menu.Start");
			expected.WriteU8(1);
			expected.WriteString("Start");
			expected.WriteU8(1);
			expected.WriteString("D\xC3\xA9marrer");
			expected.WriteU8(1);
			expected.WriteString("Iniciar");
			const Buffer payload = SerializeStringTablePayload(table);
			CHECK(payload == expected.TakeBuffer());
			const Result<StringTableData> read = DeserializeStringTablePayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Entries == MakeSortedMenuTable().Entries);
			const Result<AssetRef<StringTableData>> loaded = LoadCookedStringTable(CookStringTable(table, 1));
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->Locales == table.Locales);
		}

		TEST_CASE("StringTableData: a damaged payload is refused, never trusted" * doctest::skip(true))
		{
			const Buffer valid = SerializeStringTablePayload(MakeMenuTable());
			REQUIRE_FALSE(valid.empty());
			// Truncated, with a trailing byte, with a Present byte of 2, with entries out of key order: each payload holds one
			// fault, so each refusal is proven on its own.
			CHECK(DeserializeStringTablePayload(std::span(valid).first(valid.size() - 1)).error().GetCode() == ErrorCode::Parse);
			Buffer trailing = valid;
			trailing.push_back(std::byte{ 0 });
			CHECK(DeserializeStringTablePayload(trailing).error().GetCode() == ErrorCode::Parse);
			// Keys in order; entry "A"'s Present byte is 2.
			BinaryWriter presence;
			presence.WriteU32(1);
			presence.WriteString("en");
			presence.WriteU32(2);
			presence.WriteString("A");
			presence.WriteU8(2);
			presence.WriteString("a");
			presence.WriteString("B");
			presence.WriteU8(1);
			presence.WriteString("b");
			CHECK(DeserializeStringTablePayload(presence.TakeBuffer()).error().GetCode() == ErrorCode::Parse);
			// Valid Present bytes; the keys out of order.
			BinaryWriter unsorted;
			unsorted.WriteU32(1);
			unsorted.WriteString("en");
			unsorted.WriteU32(2);
			unsorted.WriteString("B");
			unsorted.WriteU8(1);
			unsorted.WriteString("b");
			unsorted.WriteString("A");
			unsorted.WriteU8(1);
			unsorted.WriteString("a");
			CHECK(DeserializeStringTablePayload(unsorted.TakeBuffer()).error().GetCode() == ErrorCode::Parse);
			BinaryWriter huge;
			huge.WriteU32(MaxStringTableLocales + 1);
			CHECK(DeserializeStringTablePayload(huge.TakeBuffer()).error().GetCode() == ErrorCode::Parse);
			// Another type or format version.
			CHECK_FALSE(LoadCookedStringTable(WriteCookedArtifact(AssetType::Texture, StringTableData::FormatVersion, 1, valid)).has_value());
			CHECK(LoadCookedStringTable(WriteCookedArtifact(AssetType::StringTable, StringTableData::FormatVersion + 1, 1, valid)).error().GetCode()
				== ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("StringTableData: 1,000 seeded mutations of a string table file and of its cooked form never crash"
			* doctest::test_suite(Test::FuzzSuite) * doctest::skip(true))
		{
			// The seed corpus: the string table fixtures of Tests/Data/Assets/StringTables (stream L adds them) and this file's own
			// table. The text goes through StringTableFromText, the cooked form through LoadCookedStringTable; a table either
			// accepts is valid and writes back.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			std::vector<std::string> seeds = Test::ReadFuzzSeedTexts(Test::GetRepositoryRoot() / "Tests" / "Data" / "Assets" / "StringTables", ".stringtable");
			const Result<std::string> own = StringTableToText(MakeMenuTable(), *registry);
			REQUIRE(own.has_value());
			seeds.push_back(*own);
			const Buffer payload = SerializeStringTablePayload(MakeMenuTable());
			const Test::FuzzRun run = Test::RunFuzzCase(0x57AB1E, 1000, [&registry, &seeds, &payload](Random& random, uint64_t /*index*/)
			{
				if (random.NextBool())
				{
					std::string mutated = seeds[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(seeds.size()) - 1))];
					Test::MutateText(mutated, random);
					StringTableLoadReport report;
					const Result<StringTableData> table = StringTableFromText(mutated, *registry, report, random.NextBool());
					if (!table.has_value())
						return false;
					CHECK(ValidateStringTable(*table).has_value());
					CHECK(StringTableToText(*table, *registry).has_value());
					return true;
				}
				std::vector<std::byte> mutated(payload.begin(), payload.end());
				Test::MutateBytes(mutated, random);
				const Result<AssetRef<StringTableData>> loaded = LoadCookedStringTable(WriteCookedArtifact(AssetType::StringTable, StringTableData::FormatVersion, 1, mutated));
				if (!loaded.has_value())
					return false;
				CHECK(ValidateStringTable(**loaded).has_value());
				// Every accepted payload is canonical: it writes back byte for byte.
				CHECK(SerializeStringTablePayload(**loaded) == mutated);
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}
	}

}
