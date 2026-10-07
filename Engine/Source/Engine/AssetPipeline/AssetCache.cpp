#include "EnginePCH.h"
#include "Engine/AssetPipeline/AssetCache.h"

// M6 contract stub (Roadmap rule 3): stream D (caches, pak, PakMount) implements the cooked cache.

namespace Engine {

	struct AssetCache::State
	{
		VirtualFileSystem* Vfs = nullptr; // documented back-reference
		VfsPath Root;
	};

	AssetCache::AssetCache(VirtualFileSystem& vfs, VfsPath root)
		: m_State(CreateScope<State>())
	{
		m_State->Vfs = &vfs;
		m_State->Root = std::move(root);
	}

	AssetCache::~AssetCache() = default;

	uint64_t AssetCache::ComputeKey(std::span<const std::byte> /*sourceBytes*/, std::string_view /*importerId*/, uint32_t /*importerVersion*/,
		const Json& /*canonicalSettings*/, uint32_t /*engineCookVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Result<std::optional<CachedImport>> AssetCache::Find(AssetHandle /*source*/, uint64_t /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetCache::Find is an M6 contract stub");
	}

	Result<std::optional<Buffer>> AssetCache::ReadArtifact(AssetHandle /*artifact*/, uint64_t /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetCache::ReadArtifact is an M6 contract stub");
	}

	Status AssetCache::Store(AssetHandle /*source*/, uint64_t /*key*/, const CachedImport& /*import*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetCache::Store is an M6 contract stub");
	}

	Status AssetCache::Remove(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetCache::Remove is an M6 contract stub");
	}

	const VfsPath& AssetCache::GetRoot() const
	{
		return m_State->Root;
	}

	bool IsManifestCurrent(const VirtualFileSystem& /*vfs*/, std::span<const ImportDependencyRead> /*reads*/,
		std::span<const ImportAssetLookup> /*lookups*/, std::span<const ImportAssetLookupEntry> /*assets*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
