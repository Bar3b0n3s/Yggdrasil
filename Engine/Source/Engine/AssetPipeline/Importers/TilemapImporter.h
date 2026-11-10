#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .tilemap files (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 23; Architecture §6.9, §7.4):
	// TilemapFromText (strict: an unknown member is an error at import, so a misspelled key never silently keeps its default;
	// ValidateTilemap's rules), cooked with CookTilemap (Asset/TilemapData.h). One artifact, the tilemap, under the source's
	// handle; no sub-assets, no settings, no dependency files (the importer reads nothing but the source: never a sheet).
	// Dependencies: the palette's sprite sheets (GetTilemapDependencies), as ADR 0028's phase-2 ledger requires of every new
	// importer, so M19's pruned export keeps them. A referenced sheet that is not registered, or not a SpriteSheet, is not an
	// import error (the reference is kept; the validator reports ASSET_MISSING or ASSET_TYPE_MISMATCH and the renderer draws
	// nothing for its tiles), as for SpriteSheetImporter's texture. A pure function of the source bytes and the importer
	// version: identical bytes in every run and configuration ("TilemapData: the cooked payload is the documented
	// little-endian layout"). Registered by stream T1 (RegisterBuiltinImporters, with its row in Asset/SourceFormats.cpp: the
	// importer Tilemap, ".tilemap", main type Tilemap, importable). Thread-safe; no main-thread state.
	class TilemapImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Tilemap";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Tilemap; }
		// ".tilemap".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: ImportFailed (Utils::MakeImportFailed, as SpriteSheetImporter) keeping the message, location and issues of
		// TilemapFromText's Parse, Validation or UnsupportedVersion, located in the source; atomic on failure.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
