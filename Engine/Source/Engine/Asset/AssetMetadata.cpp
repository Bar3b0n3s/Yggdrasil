#include "EnginePCH.h"
#include "Engine/Asset/AssetMetadata.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the .meta formats.

namespace Engine {

	const SubAssetEntry* AssetMetadata::FindSubAsset(std::string_view /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Result<VfsPath> GetMetaPath(const VfsPath& /*source*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GetMetaPath is an M6 contract stub");
	}

	Result<VfsPath> GetSourcePathOfMeta(const VfsPath& /*metaPath*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GetSourcePathOfMeta is an M6 contract stub");
	}

	Result<AssetMetadata> ParseAssetMetadata(std::string_view /*text*/, std::string_view /*metaPath*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ParseAssetMetadata is an M6 contract stub");
	}

	std::string SerializeAssetMetadata(const AssetMetadata& /*metadata*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
