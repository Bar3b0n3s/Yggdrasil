#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"

#include <array>

// M6 contract stub (Roadmap rule 3): stream B (texture, material and font importers) implements the importer. The extension list is the frozen data of
// the header.

namespace Engine {

	std::span<const std::string_view> TextureImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 5> Extensions = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
		return Extensions;
	}

	Result<ImportResult> TextureImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "TextureImporter::Import is an M6 contract stub");
	}

	Result<Buffer> TextureImporter::ImportTextureFromMemory(std::span<const std::byte> /*encoded*/, const TextureImportSettings& /*settings*/,
		std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "TextureImporter::ImportTextureFromMemory is an M6 contract stub");
	}

	void TextureImporter::RegisterTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
