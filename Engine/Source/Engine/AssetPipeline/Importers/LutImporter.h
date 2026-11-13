#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .cube files (M26, C26.1, R-LUT; Docs/Decisions/0037-m26-contract.md decision 15; Architecture §7.4): Adobe/IRIDAS 3D
	// colour-grading LUTs, parsed first-party by ParseCubeLut (Asset/LutData.h: 3D tables only, 2 to 64 entries a side) and
	// cooked with CookLut, never block-compressed (a LUT is a type of its own, not a texture). One artifact, the LUT, under the
	// source's handle; no sub-assets, no settings and no dependency files. Its Import.Dependencies are empty, set explicitly:
	// a LUT reads only its own source and references no asset (ADR 0028's phase-3 ledger's importer rule). A pure function of
	// the source bytes and the importer version: identical bytes in every run and configuration. Thread-safe; no main-thread
	// state.
	class LutImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Lut";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Lut; }
		// ".cube".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: ImportFailed (Utils::MakeImportFailed) keeping the message and the line of ParseCubeLut's Parse, Unsupported
		// (a 1D table), InvalidArgument (a file over MaxCubeLutBytes) or Validation, located in the source; atomic on failure.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
