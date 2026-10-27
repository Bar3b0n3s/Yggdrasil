#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .spritesheet files (M18, C18.1, R-2D-SHEET; Docs/Decisions/0029-m18-contract.md decision 17; Architecture §6.9,
	// §7.4): SpriteSheetFromText (strict: an unknown member is an error at import, so a misspelled key never silently keeps
	// its default; ValidateSpriteSheet's rules), cooked with CookSpriteSheet (Asset/SpriteSheetData.h). One artifact, the
	// sheet, under the source's handle; no sub-assets, no settings, no dependency files (the importer reads nothing but the
	// source: never the texture). Dependencies: the sheet's texture (GetSpriteSheetDependencies), as ADR 0028's phase-1
	// ledger requires of every new importer, so M19's pruned export keeps it. A referenced texture that is not registered,
	// or not a Texture, is not an import error (the reference is kept; the validator reports ASSET_MISSING or
	// ASSET_TYPE_MISMATCH and the consumer reports the missing texture), as for MaterialImporter. A pure function of the
	// source bytes and the importer version: identical bytes in every run and configuration ("SpriteSheetData: the cooked
	// payload is the documented little-endian layout"). Thread-safe; no main-thread state.
	class SpriteSheetImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "SpriteSheet";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::SpriteSheet; }
		// ".spritesheet".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: ImportFailed (Utils::MakeImportFailed, as SoundEffectImporter) keeping the message, location and issues of
		// SpriteSheetFromText's Parse, Validation or UnsupportedVersion, located in the source; atomic on failure.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
