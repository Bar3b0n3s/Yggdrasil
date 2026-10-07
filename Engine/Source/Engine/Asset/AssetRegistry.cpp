#include "EnginePCH.h"
#include "Engine/Asset/AssetRegistry.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the scan, the lookups and the plans.

namespace Engine {

	Result<AssetScanResult> AssetRegistry::Scan(const VirtualFileSystem& /*vfs*/, const VfsPath& /*root*/,
		std::span<const AssetImporterDescription> /*importers*/, std::span<const AssetKnownLocation> /*knownLocations*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetRegistry::Scan is an M6 contract stub");
	}

	const AssetRecord* AssetRegistry::Find(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const AssetRecord* AssetRegistry::FindBySourcePath(const VfsPath& /*sourcePath*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::optional<AssetRegistry::Location> AssetRegistry::Locate(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::optional<AssetHandle> AssetRegistry::Resolve(std::string_view /*reference*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::string AssetRegistry::GetReferencePath(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<AssetHandle> AssetRegistry::GetHandles() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<const AssetRecord*> AssetRegistry::GetRecords() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<const AssetRecord*> AssetRegistry::GetDependencyRecords(AssetHandle /*owner*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<AssetKnownLocation> AssetRegistry::GetKnownLocations() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status AssetRegistry::Add(AssetRecord /*record*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetRegistry::Add is an M6 contract stub");
	}

	Status AssetRegistry::Update(AssetRecord /*record*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetRegistry::Update is an M6 contract stub");
	}

	Status AssetRegistry::Remove(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetRegistry::Remove is an M6 contract stub");
	}

	Status AssetRegistry::Rename(AssetHandle /*handle*/, const VfsPath& /*newSourcePath*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetRegistry::Rename is an M6 contract stub");
	}

	Result<std::vector<AssetFileMove>> AssetRegistry::PlanMove(AssetHandle /*handle*/, const VfsPath& /*newSourcePath*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetRegistry::PlanMove is an M6 contract stub");
	}

	Result<std::vector<AssetFileMove>> AssetRegistry::PlanTrash(AssetHandle /*handle*/, const VfsPath& /*trashDirectory*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetRegistry::PlanTrash is an M6 contract stub");
	}

	Result<AssetScanFix> AssetRegistry::PlanFix(const AssetDiagnostic& /*diagnostic*/, AssetHandle /*newHandle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetRegistry::PlanFix is an M6 contract stub");
	}

}
