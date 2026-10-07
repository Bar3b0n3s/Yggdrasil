#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/MaterialImporter.h"

#include "Engine/Asset/MaterialData.h"

#include <array>
#include <utility>

namespace Engine {

	std::span<const std::string_view> MaterialImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".material" };
		return Extensions;
	}

	Result<ImportResult> MaterialImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();

		// Strict: an unknown member is an error at import, so a misspelled property never silently keeps its default.
		MaterialLoadReport report;
		Result<MaterialData> material = MaterialFromText(AsStringView(context.GetSourceBytes()), context.GetRegistry(), report, true);
		if (!material)
		{
			ErrorLocation location;
			location.File = sourcePath;
			return std::unexpected(std::move(material).error().WithLocation(std::move(location)).WithContext(std::format("while importing material '{}'", sourcePath)));
		}

		ENGINE_TRY_ASSIGN(Buffer cooked, CookMaterial(*material, context.GetRegistry(), Version));
		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::Material, .SubAssetKey = {}, .Cooked = std::move(cooked) });
		// A texture that is not registered stays referenced: the validator reports it and the renderer binds a placeholder.
		result.Dependencies = GetMaterialTextureHandles(*material);
		return result;
	}

}
