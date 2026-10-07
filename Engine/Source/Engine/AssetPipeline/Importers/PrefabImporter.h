#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .prefab files (Architecture §6.3, §7.4): Prefab::FromJson in Strict mode (structural pre-validation including
	// PREFAB_INVALID_ROOT and PREFAB_NESTED_INSTANCE, migrations), cooked as the canonical document at the current version.
	// Dependencies: every asset handle its components reference, sorted. No settings.
	class PrefabImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Prefab";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Prefab; }
		// ".prefab".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: those of Prefab::FromJson's strict load.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
