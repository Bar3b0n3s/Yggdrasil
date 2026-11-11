#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .stringtable files (M22, C22.1, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decision 10; Architecture §6, §7.4):
	// StringTableFromText (strict: an unknown member is an error at import, so a misspelled key never silently keeps its
	// default; ValidateStringTable's rules), cooked with CookStringTable (Asset/StringTableData.h), its entries sorted by Key.
	// One artifact, the table, under the source's handle; no sub-assets, no settings, no dependency files. Its
	// Import.Dependencies are GetStringTableDependencies' (none: a table references no asset), set explicitly, as ADR 0028's
	// phase-2 ledger requires of every new importer. A pure function of the source bytes and the importer version: identical
	// bytes in every run and configuration ("StringTableData: the cooked payload is the documented little-endian layout").
	// Thread-safe; no main-thread state.
	class StringTableImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "StringTable";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::StringTable; }
		// ".stringtable".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: ImportFailed (Utils::MakeImportFailed) keeping the message, location and issues of StringTableFromText's
		// Parse, Validation or UnsupportedVersion, located in the source; atomic on failure.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
