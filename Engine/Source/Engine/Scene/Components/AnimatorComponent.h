#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <vector>

namespace Engine {

	// M23 (C23.3; Docs/Decisions/0034-m23-contract.md decision 28): the bounds of an Animator's registered fields. Layers:
	// the blend stack's depth; Speed: a layer's time scale; Time: a layer's time in seconds (MaxAnimationClipDuration's order,
	// so a non-looping layer's time may stay at a long clip's end).
	inline constexpr uint32_t MaxAnimatorLayers = 8;
	inline constexpr float MaxAnimatorSpeed = 100.0f;
	inline constexpr float MaxAnimatorTime = 1.0e6f;

	// Registry enum "AnimatorRootMotion": what the Animator does with its skeleton's root motion (Animation/RootMotion.h).
	enum class AnimatorRootMotion : uint8_t
	{
		None,   // the motion stays in the pose: the mesh moves away from the entity
		Apply,  // held out of the pose; the AnimationSystem phase moves the entity's Transform by the tick's delta
		Extract // held out of the pose; the delta is only reported (Animator:GetRootMotion), for a script to apply
	};

	// Registry struct "AnimatorLayer": one layer of an Animator's blend stack. Its state is component data, so the scene's
	// serialization and the state hash cover playback (ADR 0028's phase-2 ledger: "Sprite and skeletal animation keep their
	// playback state in component data").
	struct AnimatorLayer
	{
		TypedAssetHandle<AssetType::AnimationClip> Clip{}; // null: the layer plays nothing
		float Time = 0.0f;                                 // seconds into the clip, advanced each fixed step by FixedDelta * Speed
		float Speed = 1.0f;                                // [0, MaxAnimatorSpeed]; 0 holds the layer's time
		float Weight = 1.0f;                               // [0, 1]: how much the layer's keyed paths override the layers before it
		bool Loop = true;                                  // wrap at the clip's end; otherwise hold its last pose
	};

	// Registry name "Animator", version 1, category Animation (M23, C23.3, C-SKEL, C-ANIM-BLEND; decision 28): poses a
	// Skeleton from its layers' clips in the AnimationSystem phase of the fixed step (after the tick's scripts, tasks and
	// native systems, before PreStepTransformUpdate; ADR 0028's phase-2 execution order), in Play and Simulate, and the
	// SkinnedMeshes on its entity and below it follow that pose. Requires Transform. Blending is script-driven: scripts set
	// the layers' clips, times, speeds and weights (Animator:Play, SetWeight, or the Layers field). While Playing is false
	// no layer's time advances, but the pose is still sampled at the layers' times, so a script scrubs a paused Animator by
	// writing Time. The sampled pose is runtime state (Scene/Components/AnimationPoseComponent.h), rebuilt from these fields
	// and the assets, never serialized or hashed.
	struct AnimatorComponent
	{
		TypedAssetHandle<AssetType::Skeleton> Skeleton{}; // null or failing to load: no pose
		std::vector<AnimatorLayer> Layers{};              // at most MaxAnimatorLayers, blended in order
		bool Playing = true;
		AnimatorRootMotion RootMotion = AnimatorRootMotion::None;
	};

}
