#include "EnginePCH.h"
#include "Engine/Scene/AnimationValidation.h"

#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/AnimationTargets.h"
#include "Engine/Scene/Scene.h"

#include <optional>
#include <utility>

// The animation checks of a scene (AnimationValidation.h); the AnimationSystem phase raises the same findings at run time
// (Detail::FindSkinnedMeshIssue).

namespace Engine {

	std::vector<AnimationSceneIssue> FindAnimationSceneIssues(const Scene& scene, AssetManager& assets)
	{
		std::vector<AnimationSceneIssue> issues;
		if (scene.GetRegistry().view<SkinnedMeshComponent>().empty())
			return issues;
		scene.ForEachCanonical([&issues, &assets](ConstEntity entity)
		{
			if (!entity.HasComponent<SkinnedMeshComponent>() || !entity.IsActive())
				return;
			if (std::optional<AnimationSceneIssue> issue = Detail::FindSkinnedMeshIssue(entity, assets))
				issues.push_back(std::move(*issue));
		});
		return issues;
	}

}
