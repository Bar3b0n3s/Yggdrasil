#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/StringTableImporter.h"

#include <array>

// The .stringtable importer (StringTableImporter.h): its id, version, type and extension are final; Import is a stub of the
// M22 contract (C22.1, Docs/Decisions/0033-m22-contract.md decision 10) that stream L implements and registers
// (RegisterBuiltinImporters, Asset/SourceFormats.cpp).

namespace Engine {

	std::span<const std::string_view> StringTableImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".stringtable" };
		return Extensions;
	}

	Result<ImportResult> StringTableImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the string table importer (Docs/Decisions/0033-m22-contract.md decision 10, stream L)");
	}

}
