#pragma once

#include "Engine/Animation/AnimationSampling.h"
#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// A skeleton's pose: sampling a clip at a time, the script-driven blend of an Animator's layers, and the joint model and skin
// matrices (M23, C23.3, C-SKEL, C-MORPH, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 26). DetMath and
// IEEE-exact operations only (AnimationSampling.h's determinism paragraph).
//
// An Animator's pose at a tick (Scene/AnimationSystem.h): the rest pose of its Skeleton (MakeRestPose); then, for each layer
// in order whose clip loads and whose Weight is above 0, the clip sampled at the layer's time over the rest pose
// (SampleAnimationClip), blended over the result by the layer's Weight (BlendAnimationPose). Only the paths a layer's clip
// keys blend, so a clip of the upper body over a walking layer leaves the legs walking. No state machine: scripts choose the
// clips, times and weights (C-ANIM-BLEND; an animation state-machine graph is not built, decision 26). Frozen by the M23
// contract C23.3; stream B implements it.

namespace Engine {

	// The paths of a joint a pose holds keyed values for (AnimationPose::Keyed), as bits.
	inline constexpr uint8_t AnimationPoseTranslationBit = 1U << 0;
	inline constexpr uint8_t AnimationPoseRotationBit = 1U << 1;
	inline constexpr uint8_t AnimationPoseScaleBit = 1U << 2;

	// One morph channel's sampled weights: Target is the channel's entity path (AnimationClipData.h), Weights its values.
	struct AnimationMorphWeights
	{
		std::string Target{};
		std::vector<float> Weights{};

		bool operator==(const AnimationMorphWeights&) const = default;
	};

	// A pose of one skeleton: a local transform per joint (the skeleton's order), which of its paths a clip keyed, and the
	// morph weights of the Weights channels sampled, in the order first sampled.
	struct AnimationPose
	{
		std::vector<JointTransform> Joints{};
		std::vector<uint8_t> Keyed{}; // one per joint: AnimationPose*Bit of the paths a sampled clip keyed
		std::vector<AnimationMorphWeights> Morphs{};

		bool operator==(const AnimationPose&) const = default;
	};

	// The rest pose of `skeleton` (its joints' Translation, Rotation and Scale), no path keyed, no morph weights. Pure.
	[[nodiscard]] AnimationPose MakeRestPose(const SkeletonData& skeleton);

	// Samples `clip` at `time` (clamped to [0, Duration]) into `pose` (a pose of `skeleton`): each joint channel whose Target
	// names a joint of the skeleton (FindSkeletonJoint) sets that joint's path, by its interpolation (AnimationClipData.h:
	// before the first key the first value, after the last the last; Linear rotations by SlerpQuaternion, CubicSpline
	// rotations normalized), and marks it keyed; a channel whose joint the skeleton lacks is ignored. Each Weights channel
	// sets the morph weights of its Target (added after the pose's others when new). Pure apart from `pose`.
	void SampleAnimationClip(const AnimationClipData& clip, const SkeletonData& skeleton, float time, AnimationPose& pose);

	// Blends `layer` (a pose of the same skeleton, sampled over the rest pose) over `result` by `weight` (clamped to
	// [0, 1]): for each joint path `layer` keyed, Translation and Scale = a + (b - a) * weight, Rotation the normalized
	// linear interpolation along the shorter arc; the path becomes keyed in `result`. Each of the layer's morph weights
	// lists blends the same way over `result`'s list of that Target: the base its caller seeded there, else zeros (a
	// shorter list padded with zeros; the longer length kept). The AnimationSystem and PoseAnimator seed `result`, before
	// the first layer, with a list for each Target a loaded layer's clip keys: that entity's SkinnedMesh weights resolved
	// against its mesh's targets' defaults (ResolveMorphWeights, Asset/MeshDeform.h; none when the Target names no
	// SkinnedMesh or its mesh is not loaded), so a morph layer at weight 0.5 moves half way from the authored weights to the
	// clip's, as a joint blends from its rest pose. Weight 0 leaves `result` unchanged and weight 1 copies the layer's keyed
	// paths and weights exactly. Pure apart from `result`.
	void BlendAnimationPose(AnimationPose& result, const AnimationPose& layer, float weight);

	// Each joint's model transform: ComposeJointTransform of its local transform, times its parent's model transform first
	// (parent * local; a root's is its local transform), in joint order. Model space is the Animator entity's space. Pure.
	[[nodiscard]] std::vector<glm::mat4> ComputeJointModelTransforms(const SkeletonData& skeleton, const AnimationPose& pose);

	// Each joint's skin matrix, jointModel[j] * skeleton.Joints[j].InverseBind: what carries a vertex bound to the joint
	// from the mesh's bind space into the Animator entity's space. `jointModel` holds one matrix per joint (asserted). Pure.
	[[nodiscard]] std::vector<glm::mat4> ComputeSkinMatrices(const SkeletonData& skeleton, std::span<const glm::mat4> jointModel);

	// The pose digest: XXH64 (seed 0) over the joint model matrices' floats (column-major, in joint order) and then each
	// morph list's Target bytes, its weight count and its weights, every float and count as its little-endian bit pattern.
	// What "a sampled pose hashes equal in all three configurations" compares (Roadmap M23), and what scripts' test suites
	// read through Test.GetPoseDigest in the Release and Dist runs (decision 31). Pure.
	[[nodiscard]] uint64_t ComputeAnimationPoseDigest(std::span<const glm::mat4> jointModel, std::span<const AnimationMorphWeights> morphs);

}
