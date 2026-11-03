#pragma once

#include "Engine/Core/Base.h"

#include <vector>

namespace Engine {

	// Registry name "SkinnedMesh", version 1, category Animation (M23, C23.3, C-SKEL, C-MORPH;
	// Docs/Decisions/0034-m23-contract.md decision 28): deforms the entity's MeshRenderer mesh (required). A mesh with a skin
	// follows the pose of the nearest Animator on the entity or an ancestor, whose Skeleton must hold the joints the skin
	// addresses (ANIMATION_NO_ANIMATOR, ANIMATION_SKELETON_MISMATCH otherwise, and the mesh draws its bind pose); a mesh with
	// morph targets takes MorphWeights, one per target in order (a missing weight is the target's DefaultWeight, an extra one
	// is ignored: Asset/MeshDeform.h's ResolveMorphWeights, the one rule the renderer applies to every draw of the mesh, so a
	// mesh without a SkinnedMesh draws at its targets' defaults too), each replaced by the Animator's blended Weights channel
	// whose Target is this entity's path from the Animator's entity (AnimationClipData.h), which blends from these weights
	// (Animation/AnimationPose.h's BlendAnimationPose) and, sampled from a clip, may leave [-1, 1]. A mesh with neither draws
	// as a plain MeshRenderer. The deformation runs on the GPU in the Skinning pass of the Deform stage
	// (Renderer/SkinningPass.h); the simulation never reads deformed vertices.
	struct SkinnedMeshComponent
	{
		std::vector<float> MorphWeights{}; // at most MaxMeshMorphTargets, each in [-1, 1]
	};

}
