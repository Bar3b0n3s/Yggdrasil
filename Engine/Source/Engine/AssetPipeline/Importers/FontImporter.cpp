#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/FontImporter.h"

#include <array>

// M6 contract stub (Roadmap rule 3): stream B (texture, material and font importers) implements the importer. The extension list is the frozen data of
// the header.

namespace Engine {

	std::span<const std::string_view> FontImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 2> Extensions = { ".ttf", ".otf" };
		return Extensions;
	}

	Result<ImportResult> FontImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "FontImporter::Import is an M6 contract stub");
	}

	void FontImporter::RegisterTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
