#pragma once

#include "Engine/Animation/AnimationPose.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Base.h"
#include "Engine/Scene/AnimationValidation.h"
#include "Engine/Scene/Entity.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the AnimationSystem phase, the animation checks and render extraction share about an Animator's SkinnedMeshes (M23,
// Docs/Decisions/0034-m23-contract.md decisions 26, 27 and 30): the entity path a clip's Weights channel names
// (AnimationClipData.h), the morph weights a SkinnedMesh draws with, and the checks of one SkinnedMesh.

namespace Engine {

	namespace Detail {

		// `handle` loaded through `assets` as a T, or null when either is null, it does not load or it is another type.
		template<AssetDataType T>
		[[nodiscard]] AssetRef<T> LoadAnimationAsset(AssetManager* assets, AssetHandle handle)
		{
			if (assets == nullptr || !handle.IsValid())
				return nullptr;
			Result<AssetRef<Asset>> loaded = assets->Load(handle);
			if (!loaded.has_value())
				return nullptr;
			return AssetCast<T>(*loaded);
		}

		// The Target of `entity` for `animator`'s clips: the '/'-separated names of the entities from `animator` (excluded)
		// down to `entity`, empty for `animator` itself; nullopt when `entity` is neither `animator` nor below it.
		[[nodiscard]] std::optional<std::string> GetAnimationTargetPath(ConstEntity animator, ConstEntity entity);

		// The entity a Weights channel's `target` names below `animator` (each name matched exactly, the first child of that
		// name in sibling order; `animator` itself for an empty target), or an invalid handle when none matches.
		[[nodiscard]] ConstEntity FindAnimationTarget(ConstEntity animator, std::string_view target);

		// The morph weights `entity`'s SkinnedMesh draws with when its mesh loads through `assets` (ResolveMorphWeights against
		// its targets' defaults): the base an Animator's morph layers blend over (BlendAnimationPose). Nullopt when the entity
		// has no SkinnedMesh or MeshRenderer, `assets` is null or the mesh does not load.
		[[nodiscard]] std::optional<std::vector<float>> ResolveSkinnedMeshWeights(ConstEntity entity, AssetManager* assets);

		// The weights a SkinnedMesh's item draws with (RenderSnapshot.h's MeshDrawItem::MorphWeights): `component`'s, each
		// replaced by `animated`'s weight at its index, as long as the longer of the two.
		[[nodiscard]] std::vector<float> MergeAnimatedMorphWeights(std::span<const float> component, std::span<const float> animated);

		// The weights `pose` holds for `target`, or null.
		[[nodiscard]] const AnimationMorphWeights* FindPoseMorphWeights(std::span<const AnimationMorphWeights> morphs, std::string_view target);

		// The animation finding of one effectively enabled `entity` with a SkinnedMesh and a MeshRenderer whose mesh loads
		// through `assets` (AnimationValidation.h's checks): ANIMATION_NO_ANIMATOR when its mesh has a skin and no Animator
		// drives it, ANIMATION_SKELETON_MISMATCH when its driving Animator's Skeleton loads and holds fewer joints than the
		// skin addresses; nullopt otherwise, also when the mesh or skeleton is missing or fails to load.
		[[nodiscard]] std::optional<AnimationSceneIssue> FindSkinnedMeshIssue(ConstEntity entity, AssetManager& assets);

	}

}
