#include "EnginePCH.h"
#include "Engine/Scene/SpriteValidation.h"

namespace Engine {

	std::vector<SpriteSceneIssue> FindSpriteSceneIssues(const Scene& /*scene*/, AssetManager* /*assets*/, std::span<const std::string> /*sortingLayers*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
