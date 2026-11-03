#include "EnginePCH.h"
#include "Engine/Animation/AnimationPose.h"

// Poses, layer blending, joint and skin matrices and the pose digest (AnimationPose.h). Contract stubs of M23's C23.3,
// implemented by stream B (Docs/Decisions/0034-m23-contract.md decision 26): until then every pose is empty.

namespace Engine {

	AnimationPose MakeRestPose(const SkeletonData& /*skeleton*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void SampleAnimationClip(const AnimationClipData& /*clip*/, const SkeletonData& /*skeleton*/, float /*time*/, AnimationPose& /*pose*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void BlendAnimationPose(AnimationPose& /*result*/, const AnimationPose& /*layer*/, float /*weight*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<glm::mat4> ComputeJointModelTransforms(const SkeletonData& /*skeleton*/, const AnimationPose& /*pose*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<glm::mat4> ComputeSkinMatrices(const SkeletonData& /*skeleton*/, std::span<const glm::mat4> /*jointModel*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint64_t ComputeAnimationPoseDigest(std::span<const glm::mat4> /*jointModel*/, std::span<const AnimationMorphWeights> /*morphs*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
