#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .scene files (Architecture §6.2, §7.4: "strict load (structural pre-validation, §6) + migrate; cooked canonical
	// JSON"). The file is loaded with SceneSerializer in Strict mode into a scratch scene (the one load path, which
	// pre-validates the entity graph and applies migrations) and saved canonically at the current version, so the cooked
	// document is what the Runtime's strict load accepts. Dependencies: every asset handle the scene's components reference
	// (TypedAssetHandle fields, found through the registry), sorted. No settings. The scratch scene is an ECS registry,
	// which only the main thread may touch (§4.11: jobs never access the ECS), so the import runs on the main thread
	// (RequiresMainThread). PrefabImporter needs no such rule: Prefab::FromJson validates the document without a scene.
	class SceneImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Scene";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Scene; }
		// ".scene".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }
		// True: the import builds a scratch Scene (§4.11).
		[[nodiscard]] bool RequiresMainThread() const override { return true; }

		// Errors: those of SceneSerializer's strict load (Parse, Validation located at the JSON pointer, UnsupportedVersion).
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
