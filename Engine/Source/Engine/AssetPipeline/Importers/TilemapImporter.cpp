#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/TilemapImporter.h"

#include <array>

namespace Engine {

	std::span<const std::string_view> TilemapImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".tilemap" };
		return Extensions;
	}

	Result<ImportResult> TilemapImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the tilemap importer (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

}
