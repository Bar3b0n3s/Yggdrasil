#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .material files (Architecture §6.5, §7.4 "validate + migrate; cooked canonical JSON"): MaterialFromText (strict:
	// unknown members are an error at import, so a typo never silently drops a property), validated through the registry,
	// cooked with CookMaterial. Dependencies: the referenced textures (GetMaterialTextureHandles). A referenced texture that
	// is not registered is not an import error (the reference is kept; the validator reports ASSET_MISSING and the renderer
	// binds the placeholder). No settings.
	class MaterialImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Material";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Material; }
		// ".material".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: Parse; Validation (located: the file and the JSON pointer); UnsupportedVersion; all as ImportFailed context.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
