#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/NavMeshImporter.h"

#include "Engine/Asset/NavMeshData.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"

#include <array>
#include <format>
#include <string>

namespace Engine {

	std::span<const std::string_view> NavMeshImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".navmesh" };
		return Extensions;
	}

	Result<ImportResult> NavMeshImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		Result<NavMeshData> mesh = ReadNavMeshSource(context.GetSourceBytes());
		if (!mesh.has_value())
			return std::unexpected(Utils::MakeImportFailed(mesh.error(), sourcePath, std::format("while importing navigation mesh '{}'", sourcePath)));

		ImportResult result;
		result.Artifacts.push_back(
			ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::NavMesh, .SubAssetKey = {}, .Cooked = CookNavMesh(*mesh, Version) });
		return result;
	}

}
