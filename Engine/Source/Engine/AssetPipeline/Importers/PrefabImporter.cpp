#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/PrefabImporter.h"

#include <array>

// M6 contract stub (Roadmap rule 3): stream B (texture, material and font importers) implements the importer. The extension list is the frozen data of
// the header.

namespace Engine {

	std::span<const std::string_view> PrefabImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".prefab" };
		return Extensions;
	}

	Result<ImportResult> PrefabImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PrefabImporter::Import is an M6 contract stub");
	}

}
