#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/PrefabImporter.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/AssetPipeline/Private/DocumentAssetReferences.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Prefab.h"

#include <nlohmann/json.hpp>

#include <array>
#include <utility>

namespace Engine {

	std::span<const std::string_view> PrefabImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".prefab" };
		return Extensions;
	}

	Result<ImportResult> PrefabImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		LoadOptions options;
		options.Mode = LoadMode::Strict;
		options.SourcePath = sourcePath;
		LoadReport report;
		ENGINE_TRY_ASSIGN(const Prefab prefab, WithContext(Prefab::LoadFromString(AsStringView(context.GetSourceBytes()), context.GetRegistry(), options, report), std::format("while importing prefab '{}'", sourcePath)));

		// The canonical document at the current version: what Prefab::FromJson reads in the Runtime (§7.4).
		ENGINE_TRY_ASSIGN(const Json document, prefab.ToJson());
		ENGINE_TRY_ASSIGN(Buffer cooked, CookDocument(AssetType::Prefab, document, Version));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::Prefab, .SubAssetKey = {}, .Cooked = std::move(cooked) });
		result.Dependencies = Utils::CollectDocumentAssetReferences(document, context.GetRegistry());
		return result;
	}

}
