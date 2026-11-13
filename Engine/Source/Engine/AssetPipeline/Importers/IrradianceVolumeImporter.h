#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .irradiance files (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decisions 3 and 8; Architecture §6, §7.4):
	// the irradiance volumes lighting.bake writes beside the scene it baked (EditorCore/Lighting/LightingBake.h).
	// ReadIrradianceVolumeSource (Asset/IrradianceVolumeData.h), cooked unchanged with CookIrradianceVolume. One artifact, the
	// volume, under the source's handle; no sub-assets, no settings, no dependency files and no dependencies (a volume reads only
	// its own source and references no asset, ADR 0028's phase-3 ledger's importer rule). A pure function of the source bytes and
	// the importer version, identical in every run and configuration. Registered in RegisterBuiltinImporters, with its row in
	// Asset/SourceFormats.cpp (the importer IrradianceVolume, ".irradiance", main type IrradianceVolume, importable).
	// Thread-safe; no main-thread state.
	class IrradianceVolumeImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "IrradianceVolume";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::IrradianceVolume; }
		// ".irradiance".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: ImportFailed (Utils::MakeImportFailed) keeping the message of ReadIrradianceVolumeSource's Parse, Validation or
		// UnsupportedVersion; atomic on failure.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
