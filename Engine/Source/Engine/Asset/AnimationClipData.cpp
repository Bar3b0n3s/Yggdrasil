#include "EnginePCH.h"
#include "Engine/Asset/AnimationClipData.h"

#include "Engine/Asset/CookedFormat.h"

#include <utility>

// The animation clip's cooked format (AnimationClipData.h). The key layout and the artifact wrappers are the contract's; the
// payload and the validation are contract stubs of M23's C23.3, implemented by stream A (Docs/Decisions/0034-m23-contract.md
// decision 24): until then no clip serializes, reads or validates.

namespace Engine {

	uint32_t GetAnimationKeyFloatCount(const AnimationChannel& channel)
	{
		uint32_t count = 0;
		switch (channel.Path)
		{
			case AnimationPath::Translation:
			case AnimationPath::Scale:       count = 3; break;
			case AnimationPath::Rotation:    count = 4; break;
			case AnimationPath::Weights:     count = channel.WeightCount; break;
		}
		return channel.Interpolation == AnimationInterpolation::CubicSpline ? 3 * count : count;
	}

	Status ValidateAnimationClipData(const AnimationClipData& /*clip*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 24, stream A)");
	}

	Buffer SerializeAnimationClipPayload(const AnimationClipData& /*clip*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AnimationClipData> DeserializeAnimationClipPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 24, stream A)");
	}

	Buffer CookAnimationClip(const AnimationClipData& clip, uint32_t importerVersion)
	{
		const Buffer payload = SerializeAnimationClipPayload(clip);
		return WriteCookedArtifact(AssetType::AnimationClip, AnimationClipData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<AnimationClipData>> LoadCookedAnimationClip(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::AnimationClip, AnimationClipData::FormatVersion));
		ENGINE_TRY_ASSIGN(AnimationClipData clip, DeserializeAnimationClipPayload(artifact.Payload));
		return AssetRef<AnimationClipData>(CreateRef<AnimationClipData>(std::move(clip)));
	}

}
