#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/SpriteSheetImporter.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"

#include <array>
#include <format>
#include <utility>

namespace Engine {

	std::span<const std::string_view> SpriteSheetImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".spritesheet" };
		return Extensions;
	}

	Result<ImportResult> SpriteSheetImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();

		// Strict: an unknown member is an error at import, so a misspelled key never silently keeps its default.
		SpriteSheetLoadReport report;
		Result<SpriteSheetData> sheet = SpriteSheetFromText(AsStringView(context.GetSourceBytes()), context.GetRegistry(), report, true);
		if (!sheet.has_value())
			return std::unexpected(Utils::MakeImportFailed(sheet.error(), sourcePath, std::format("while importing sprite sheet '{}'", sourcePath)));

		ImportResult result;
		result.Artifacts.push_back(
			ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::SpriteSheet, .SubAssetKey = {}, .Cooked = CookSpriteSheet(*sheet, Version) });
		// The texture stays referenced when it is not registered (or not a Texture): the validator reports it, and the sheet's
		// consumers report the missing texture.
		result.Dependencies = GetSpriteSheetDependencies(*sheet);
		return result;
	}

}
