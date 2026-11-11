#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/GraphImporter.h"

#include "Engine/AssetPipeline/Graph/GraphDocument.h"

// GraphImporter (Docs/Decisions/0035-m24-contract.md decision 9): stream G-B implements Import, and stream G-A registers the
// importer (RegisterBuiltinImporters, Asset/SourceFormats.cpp's row).

namespace Engine {

	std::span<const std::string_view> GraphImporter::GetExtensions() const
	{
		static constexpr std::string_view Extensions[] = { GraphFileExtension };
		return Extensions;
	}

	Result<ImportResult> GraphImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphImporter::Import (Docs/Decisions/0035-m24-contract.md decision 9, stream G-B)");
	}

}
