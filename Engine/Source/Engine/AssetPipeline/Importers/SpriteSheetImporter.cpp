#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/SpriteSheetImporter.h"

#include <array>

namespace Engine {

	std::span<const std::string_view> SpriteSheetImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".spritesheet" };
		return Extensions;
	}

	Result<ImportResult> SpriteSheetImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 SpriteSheet contract (Docs/Decisions/0029-m18-contract.md decision 17, stream S)"));
	}

}
