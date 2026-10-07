#include "EnginePCH.h"
#include "Engine/AssetPipeline/IAssetImporter.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the import context's recorded reads and
// lookups, the default dependency listing and the extension match.

namespace Engine {

	ImportContext::ImportContext(Specification specification)
		: m_Specification(std::move(specification))
	{
	}

	Result<Buffer> ImportContext::ReadDependency(const VfsPath& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ImportContext::ReadDependency is an M6 contract stub");
	}

	std::optional<ImportAssetLookupEntry> ImportContext::FindAsset(const VfsPath& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::vector<ImportDependencyRead> ImportContext::GetDependencyReads() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Reads;
	}

	std::vector<ImportAssetLookup> ImportContext::GetLookups() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Lookups;
	}

	Result<std::vector<VfsPath>> IAssetImporter::ListDependencyFiles(std::span<const std::byte> /*source*/, const VfsPath& /*sourcePath*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::vector<VfsPath>();
	}

	bool IAssetImporter::CanImport(std::string_view /*extension*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
