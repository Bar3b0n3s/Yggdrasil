#pragma once

#include "Engine/Animation/AnimationPose.h"
#include "Engine/Animation/RootMotion.h"
#include "Engine/Asset/Asset.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Engine {

	// A runtime-only component (Components/RuntimeComponents.h's rules: never registered, serialized or hashed, invisible to
	// scripts and automation, and adding or patching it changes neither the scene's revision nor the change tracker): an
	// Animator's sampled pose (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 29). The AnimationSystem phase writes
	// it on every effectively enabled Animator entity whose Skeleton loads, once per fixed step; the editor's animation
	// preview writes it on an edit scene's Animator while it previews (EditorCore/Animation/AnimationPreview.h) and removes
	// it when it stops. It is a pure function of the Animator's fields, the assets and the tick, so the state hash never
	// needs it: scenes without an Animator never hold one, and a session's serializer copy never carries one.
	//
	// Readers: render extraction (a SkinnedMesh's palette: its entity's inverse rendered world matrix times the Animator
	// entity's rendered world matrix times Skin[j], Scene/RenderExtraction.h), the joint query (Animator:GetJointTransform,
	// Scene/AnimationSystem.h's FindAnimatorJointWorldTransform), Test.GetPoseDigest and EntityBounds.
	struct AnimationPoseComponent
	{
		AssetRef<SkeletonData> Skeleton{};           // the skeleton the pose was sampled for (its joint names and count)
		std::vector<glm::mat4> JointModel{};         // per joint: its transform in the Animator entity's space
		std::vector<glm::mat4> Skin{};               // per joint: JointModel[j] * InverseBind[j] (ComputeSkinMatrices)
		std::vector<AnimationMorphWeights> Morphs{}; // the blended Weights channels, by Target path
		RootMotionDelta RootMotion{};                // the last step's blended root motion (identity with RootMotion None)
		uint64_t Tick = 0;                           // the fixed step that sampled it (0 for the editor's preview)
	};

}
