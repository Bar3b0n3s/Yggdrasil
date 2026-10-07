#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/GltfImporter.h"

#include <array>

// M6 contract stub (Roadmap rule 3): stream C (glTF importer and fixtures) implements the importer. The extension list is the frozen data of
// the header.

namespace Engine {

	std::span<const std::string_view> GltfImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 2> Extensions = { ".gltf", ".glb" };
		return Extensions;
	}

	Result<ImportResult> GltfImporter::Import(ImportContext& /*context*/, const AssetMetadata& /*metadata*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GltfImporter::Import is an M6 contract stub");
	}

	Result<std::vector<VfsPath>> GltfImporter::ListDependencyFiles(std::span<const std::byte> /*source*/, const VfsPath& /*sourcePath*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GltfImporter::ListDependencyFiles is an M6 contract stub");
	}

	Result<std::vector<std::string>> GltfImporter::ListExternalUris(std::span<const std::byte> /*source*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GltfImporter::ListExternalUris is an M6 contract stub");
	}

	void GltfImporter::RegisterTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
