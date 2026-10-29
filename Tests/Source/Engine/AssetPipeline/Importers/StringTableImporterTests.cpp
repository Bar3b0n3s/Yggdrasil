#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/StringTableImporter.h"

#include "Engine/Asset/StringTableData.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <string>
#include <string_view>
#include <vector>

// StringTableImporter (M22, C22.1, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decision 10). Skipped by the contract;
// stream L un-skips them.

namespace Engine {

	namespace {

		// Two locales and two entries, authored out of key order.
		constexpr std::string_view MenuTable = R"({"Format": "StringTable", "Version": 1, "Locales": ["en", "fr"],
			"Entries": [{"Key": "Menu.Start", "Values": {"en": "Start", "fr": "Commencer"}}, {"Key": "Menu.Quit", "Values": {"en": "Quit"}}]})";

		Result<ImportResult> ImportStringTable(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x57AB000000000001ull);
			metadata.Type = AssetType::StringTable;
			metadata.Importer = std::string(StringTableImporter::Id);
			metadata.ImporterVersion = StringTableImporter::Version;
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = bytes,
				.Settings = VariantValue(),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			Result<ImportResult> result = StringTableImporter().Import(context, metadata);
			// The importer reads nothing but its source.
			CHECK(context.GetDependencyReads().empty());
			return result;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("StringTableImporter: a table imports with its entries sorted by key and no dependencies" * doctest::skip(true))
		{
			CHECK(StringTableImporter().CanImport(".StringTable"));
			CHECK(StringTableImporter().GetMainType() == AssetType::StringTable);
			CHECK(StringTableImporter().GetSettingsTypeName().empty());
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Menu.stringtable", MenuTable);
			const Result<ImportResult> imported = ImportStringTable(fixture, "Assets/Menu.stringtable");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::StringTable);
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x57AB000000000001ull));
			// A table references no asset (ADR 0028's phase-2 ledger: every new importer fills Import.Dependencies).
			CHECK(imported->Dependencies.empty());
			CHECK(imported->Diagnostics.empty());
			const Result<AssetRef<StringTableData>> table = LoadCookedStringTable(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(table.has_value(), table.error().ToString());
			CHECK((*table)->Locales == std::vector<std::string>{ "en", "fr" });
			REQUIRE((*table)->Entries.size() == 2);
			CHECK((*table)->Entries[0].Key == "Menu.Quit");
			CHECK((*table)->Entries[1].Key == "Menu.Start");

			// A pure function of the source and the version: a second import gives identical bytes.
			const Result<ImportResult> again = ImportStringTable(fixture, "Assets/Menu.stringtable");
			REQUIRE(again.has_value());
			CHECK(again->Artifacts.front().Cooked == imported->Artifacts.front().Cooked);
		}

		TEST_CASE("StringTableImporter: an unknown member or an invalid table fails the import, located in its file" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Typo.stringtable", R"({"Format": "StringTable", "Version": 1, "Locales": ["en"], "Entry": []})");
			const Result<ImportResult> typo = ImportStringTable(fixture, "Assets/Typo.stringtable");
			REQUIRE_FALSE(typo.has_value());
			CHECK(typo.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(typo.error().GetLocation().File.find("Typo.stringtable") != std::string::npos);

			fixture.WriteProjectText("Assets/Bad.stringtable",
				R"({"Format": "StringTable", "Version": 1, "Locales": ["en"], "Entries": [{"Key": "A", "Values": {"en": "x", "fr": "y"}}]})");
			const Result<ImportResult> bad = ImportStringTable(fixture, "Assets/Bad.stringtable");
			REQUIRE_FALSE(bad.has_value());
			CHECK(bad.error().GetCode() == ErrorCode::ImportFailed);
			REQUIRE_FALSE(bad.error().GetIssues().empty());
			CHECK(bad.error().GetIssues().front().JsonPointer == "/Entries/0/Values/fr");

			fixture.WriteProjectText("Assets/Newer.stringtable", R"({"Format": "StringTable", "Version": 2})");
			const Result<ImportResult> newer = ImportStringTable(fixture, "Assets/Newer.stringtable");
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::ImportFailed);
		}
	}

}
