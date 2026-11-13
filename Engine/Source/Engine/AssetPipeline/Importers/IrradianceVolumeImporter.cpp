#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/IrradianceVolumeImporter.h"

#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"

#include <array>
#include <format>
#include <string>

namespace Engine {

	std::span<const std::string_view> IrradianceVolumeImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".irradiance" };
		return Extensions;
	}

	Result<ImportResult> IrradianceVolumeImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		Result<IrradianceVolumeData> volume = ReadIrradianceVolumeSource(context.GetSourceBytes());
		if (!volume.has_value())
			return std::unexpected(Utils::MakeImportFailed(volume.error(), sourcePath, std::format("while importing irradiance volume '{}'", sourcePath)));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{
			.Handle = metadata.Handle, .Type = AssetType::IrradianceVolume, .SubAssetKey = {}, .Cooked = CookIrradianceVolume(*volume, Version) });
		return result;
	}

}
