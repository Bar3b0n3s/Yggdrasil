#pragma once

#include "Engine/Animation/AnimationPose.h"
#include "Engine/Animation/AnimationSampling.h"
#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

// Root motion (M23, C23.3, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 26): the planar motion of a
// skeleton's root-motion joint (SkeletonData::RootMotionJoint: the skin's root, the hips of a humanoid rig), taken out
// of the pose so that the entity, not the mesh, moves. The planar motion is the joint's model transform's translation
// along X and Z of the Animator entity's space and the twist of its model rotation about +Y (the rotation's swing-twist
// decomposition: the twist of (x, y, z, w) is normalize((0, y, 0, w)), the identity when y and w are both 0); its
// height and its swing stay in the pose. The joint's model transform is its parent's model transform at the pose times
// its local transform; the joints above it are the importer's static joints, which hold their rest pose (no imported
// clip keys them), so their transform is constant and the planar motion is the joint's own keyed motion carried into
// model space (for a root-motion joint that is a root, its local translation's X and Z and its rotation's twist).
// Arithmetic and sqrt only (AnimationSampling.h's determinism paragraph).
//
// An Animator's RootMotion (Scene/Components/AnimatorComponent.h) chooses: None keeps the motion in the pose (the mesh walks
// away from its entity); Apply holds it out of the pose and moves the entity's Transform by the tick's blended delta, so it
// reaches physics through PreStepTransformUpdate; Extract holds it out and only reports the delta (Animator:GetRootMotion),
// for a script to move a CharacterController or a body. Frozen by the M23 contract C23.3; stream B implements it.

namespace Engine {

	// A tick's root motion in the Animator entity's space: Translation's Y is always 0, Rotation is a twist about +Y.
	struct RootMotionDelta
	{
		glm::vec3 Translation{ 0.0f };
		glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };

		bool operator==(const RootMotionDelta&) const = default;
	};

	// The planar motion of the root-motion joint of `skeleton` that `clip` keys over `step` (AdvanceAnimationTime's): with P(t)
	// the X and Z of the joint's model translation and Q(t) the twist of its model rotation at t (SampleAnimationClip over the
	// rest pose, the joints above it composed at their sampled pose), from step.PreviousTime to step.Time, across step.Wraps
	// ends (at most MaxAnimationWrapsPerStep, so the whole cycles composed stay bounded): Translation P(Duration) -
	// P(PreviousTime), plus (Wraps - 1) times P(Duration) - P(0), plus P(Time) - P(0); Rotation composed the same way
	// (Q(b) * inverse(Q(a)) for each piece a to b, the later piece on the left); without a wrap, P(Time) - P(PreviousTime)
	// and Q(Time) * inverse(Q(PreviousTime)). The identity delta when the clip keys neither the joint's translation nor its
	// rotation, or the skeleton has no joint. Pure.
	[[nodiscard]] RootMotionDelta ComputeRootMotion(const AnimationClipData& clip, const SkeletonData& skeleton, const AnimationTimeStep& step);

	// Holds the root-motion joint's planar motion out of `layerPose` (a pose of `skeleton` that `clip` was sampled into): its
	// model translation's X and Z become those `clip` samples at time 0, and its model rotation r becomes Q(0) *
	// inverse(twist(r)) * r (the twist replaced by the one at time 0), both written back into the joint's local transform
	// through its parent's model transform at the pose (for a root, directly), so the mesh stays where the clip starts while
	// the entity carries the motion. Nothing changes when the clip keys neither path of the joint. Pure apart from `layerPose`.
	void HoldRootMotion(const AnimationClipData& clip, const SkeletonData& skeleton, AnimationPose& layerPose);

	// Blends a layer's delta over `result` by `weight` (clamped to [0, 1]), as BlendAnimationPose blends a joint, only on the
	// paths `keyed` names (the layer pose's AnimationPose::Keyed bits of the root-motion joint: AnimationPoseTranslationBit for
	// Translation, AnimationPoseRotationBit for Rotation): Translation linearly, Rotation by normalized linear interpolation
	// along the shorter arc; a path `keyed` lacks keeps `result`'s, so a layer whose clip keys neither path contributes nothing
	// (a turn layer keying only the rotation leaves the walk's translation whole). Weight 0 leaves `result` and weight 1 copies
	// the keyed paths of `layer` exactly. Pure apart from `result`.
	void BlendRootMotion(RootMotionDelta& result, const RootMotionDelta& layer, uint8_t keyed, float weight);

}
