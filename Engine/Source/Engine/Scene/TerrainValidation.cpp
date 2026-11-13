#include "EnginePCH.h"
#include "Engine/Scene/TerrainValidation.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <format>
#include <optional>
#include <utility>

// The terrain checks of a scene (TerrainValidation.h; Docs/Decisions/0037-m26-contract.md decision 9), stream T-A.

namespace Engine {

	namespace Utils {

		// TextureImporter::Id and the spellings of its settings that make a data texture (AssetPipeline/Importers/
		// TextureImporter.h, which Scene does not include, §3): Usage Linear and Compression None.
		constexpr std::string_view SplatTextureImporter = "Texture";
		constexpr std::string_view SplatUsage = "Linear";
		constexpr std::string_view SplatCompression = "None";
		// What a Texture .meta written before TextureImporter version 2 (M23) holds without its Compression member.
		constexpr std::string_view DefaultCompression = "Auto";

		// The string member `key` of the import settings `settings`, `fallback` when it is absent, empty when it is not a
		// string.
		[[nodiscard]] static std::string ReadSettingsString(const JsonReader& settings, std::string_view key, std::string_view fallback)
		{
			const std::optional<JsonReader> member = settings.FindMember(key);
			if (!member.has_value())
				return std::string(fallback);
			Result<std::string> value = member->ReadString();
			return value.has_value() ? std::move(*value) : std::string();
		}

		// Whether the asset `handle` is imported as a data texture: the main asset of a Texture importer's .meta with Usage
		// Linear and Compression None.
		[[nodiscard]] static bool IsRawSplatMap(const AssetMetadata& metadata, AssetHandle handle)
		{
			if (metadata.Kind != AssetMetaKind::Asset || metadata.Handle != handle || metadata.Importer != SplatTextureImporter)
				return false;
			const JsonReader settings(metadata.Settings.Get());
			if (!settings.IsObject())
				return false;
			return ReadSettingsString(settings, "Usage", {}) == SplatUsage && ReadSettingsString(settings, "Compression", DefaultCompression) == SplatCompression;
		}

		[[nodiscard]] static bool LoadsAsHeightfield(AssetManager& assets, AssetHandle handle)
		{
			if (assets.GetAssetType(handle) != AssetType::Heightfield)
				return false;
			const Result<AssetRef<Asset>> loaded = assets.Load(handle);
			return loaded.has_value() && *loaded != nullptr && (*loaded)->GetAssetType() == AssetType::Heightfield;
		}

		static void FindTerrainIssues(const Scene& scene, ConstEntity entity, const TerrainComponent& terrain, AssetManager& assets,
			std::vector<TerrainSceneIssue>& issues)
		{
			const std::string path = scene.GetEntityPath(entity);
			const AssetHandle heightfield = terrain.Heightfield.GetHandle();
			if (!heightfield.IsValid() || !LoadsAsHeightfield(assets, heightfield))
			{
				const std::string reason = heightfield.IsValid() ? std::format("its Heightfield {} does not load as a heightfield", heightfield.ToString())
																 : std::string("it has no Heightfield");
				issues.push_back(TerrainSceneIssue{ .Code = std::string(TerrainNoHeightfieldCode),
					.Severity = DiagnosticSeverity::Warning,
					.Entity = entity.GetUUID(),
					.Component = "Terrain",
					.Field = "Heightfield",
					.Message = std::format("the Terrain of '{}' draws nothing and makes no collider body: {}", path, reason),
					.Hint = "assign a heightfield: asset.create {type: \"Heightfield\"} makes a flat one, terrain.importHeightmap converts a 16-bit "
							"PNG or RAW heightmap" });
			}

			const AssetHandle splatMap = terrain.SplatMap.GetHandle();
			if (!splatMap.IsValid() || IsBuiltinAssetHandle(splatMap) || assets.GetAssetType(splatMap) != AssetType::Texture)
				return;
			const AssetMetadata* metadata = assets.GetMetadata(splatMap);
			if (metadata == nullptr || IsRawSplatMap(*metadata, splatMap))
				return;
			issues.push_back(TerrainSceneIssue{ .Code = std::string(TerrainSplatNotRawCode),
				.Severity = DiagnosticSeverity::Warning,
				.Entity = entity.GetUUID(),
				.Component = "Terrain",
				.Field = "SplatMap",
				.Message = std::format("the SplatMap of the Terrain of '{}' ({}) is not imported as a data texture (Usage Linear, Compression None): its "
									   "weights are decoded as sRGB colour or blurred by block compression, and terrain.edit cannot paint it",
					path, assets.GetReferencePath(splatMap)),
				.Hint = "set the splat map's import settings with asset.setImportSettings {settings: {Usage: \"Linear\", Compression: \"None\"}}" });
		}

	}

	std::vector<TerrainSceneIssue> FindTerrainSceneIssues(const Scene& scene, AssetManager& assets)
	{
		std::vector<TerrainSceneIssue> issues;
		if (scene.GetRegistry().view<TerrainComponent>().empty())
			return issues;
		scene.ForEachCanonical([&scene, &assets, &issues](ConstEntity entity)
		{
			if (entity.HasComponent<HierarchyDisabledTag>())
				return;
			if (const TerrainComponent* terrain = entity.TryGetComponent<TerrainComponent>())
				Utils::FindTerrainIssues(scene, entity, *terrain, assets, issues);
		});
		return issues;
	}

}
