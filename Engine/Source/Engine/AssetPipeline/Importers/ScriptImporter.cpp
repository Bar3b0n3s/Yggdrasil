#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/ScriptImporter.h"

namespace Engine {

	std::span<const std::string_view> ScriptImporter::GetExtensions() const
	{
		ENGINE_CONTRACT_STUB();
		static constexpr std::string_view Extensions[] = { ".luau" };
		return Extensions;
	}

	Result<ImportResult> ScriptImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ScriptImporter is an M13 contract stub");
	}

}
