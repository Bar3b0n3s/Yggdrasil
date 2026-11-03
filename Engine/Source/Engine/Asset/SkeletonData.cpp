#include "EnginePCH.h"
#include "Engine/Asset/SkeletonData.h"

#include "Engine/Asset/CookedFormat.h"

#include <utility>

// The skeleton's cooked format (SkeletonData.h). The artifact wrappers are the contract's; the payload, the validation and
// the lookup are contract stubs of M23's C23.3, implemented by stream A (Docs/Decisions/0034-m23-contract.md decision 24):
// until then no skeleton serializes, reads or validates.

namespace Engine {

	Status ValidateSkeletonData(const SkeletonData& /*skeleton*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 24, stream A)");
	}

	std::optional<uint32_t> FindSkeletonJoint(const SkeletonData& /*skeleton*/, std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	Buffer SerializeSkeletonPayload(const SkeletonData& /*skeleton*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<SkeletonData> DeserializeSkeletonPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 24, stream A)");
	}

	Buffer CookSkeleton(const SkeletonData& skeleton, uint32_t importerVersion)
	{
		const Buffer payload = SerializeSkeletonPayload(skeleton);
		return WriteCookedArtifact(AssetType::Skeleton, SkeletonData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<SkeletonData>> LoadCookedSkeleton(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::Skeleton, SkeletonData::FormatVersion));
		ENGINE_TRY_ASSIGN(SkeletonData skeleton, DeserializeSkeletonPayload(artifact.Payload));
		return AssetRef<SkeletonData>(CreateRef<SkeletonData>(std::move(skeleton)));
	}

}
