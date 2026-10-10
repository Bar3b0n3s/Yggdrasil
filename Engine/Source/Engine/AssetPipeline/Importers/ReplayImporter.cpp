#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/ReplayImporter.h"

namespace Engine {

	std::span<const std::string_view> ReplayImporter::GetExtensions() const
	{
		ENGINE_CONTRACT_STUB();
		static constexpr std::string_view Extensions[] = { ".replay" };
		return Extensions;
	}

	Result<ImportResult> ReplayImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ReplayImporter is an M13 contract stub");
	}

}
