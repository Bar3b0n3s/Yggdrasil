#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/TilemapImporter.h"

#include "Engine/Asset/TilemapData.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"

#include <array>
#include <format>
#include <utility>

namespace Engine {

	std::span<const std::string_view> TilemapImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".tilemap" };
		return Extensions;
	}

	Result<ImportResult> TilemapImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();

		// Strict: an unknown member is an error at import, so a misspelled key never silently keeps its default.
		TilemapLoadReport report;
		Result<TilemapData> tilemap = TilemapFromText(AsStringView(context.GetSourceBytes()), context.GetRegistry(), report, true);
		if (!tilemap.has_value())
			return std::unexpected(Utils::MakeImportFailed(tilemap.error(), sourcePath, std::format("while importing tilemap '{}'", sourcePath)));

		ImportResult result;
		result.Artifacts.push_back(
			ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::Tilemap, .SubAssetKey = {}, .Cooked = CookTilemap(*tilemap, Version) });
		// The palette's sheets stay referenced when they are not registered (or not sprite sheets): the validator reports them,
		// and the renderer draws nothing for their tiles.
		result.Dependencies = GetTilemapDependencies(*tilemap);
		return result;
	}

}
