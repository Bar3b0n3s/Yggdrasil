#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/LutImporter.h"

#include "Engine/Asset/LutData.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"

#include <array>
#include <format>
#include <string>

namespace Engine {

	std::span<const std::string_view> LutImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".cube" };
		return Extensions;
	}

	Result<ImportResult> LutImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		Result<LutData> lut = ParseCubeLut(AsStringView(context.GetSourceBytes()));
		if (!lut.has_value())
			return std::unexpected(Utils::MakeImportFailed(lut.error(), sourcePath, std::format("while importing LUT '{}'", sourcePath)));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::Lut, .SubAssetKey = {}, .Cooked = CookLut(*lut, Version) });
		// A LUT references no asset, set explicitly as every importer's dependencies are.
		result.Dependencies = {};
		return result;
	}

}
