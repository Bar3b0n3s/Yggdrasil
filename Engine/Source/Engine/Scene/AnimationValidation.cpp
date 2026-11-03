#include "EnginePCH.h"
#include "Engine/Scene/AnimationValidation.h"

// The animation checks of a scene (AnimationValidation.h). A contract stub of M23's C23.3, implemented by stream B
// (Docs/Decisions/0034-m23-contract.md decision 30): until then nothing is reported.

namespace Engine {

	std::vector<AnimationSceneIssue> FindAnimationSceneIssues(const Scene& /*scene*/, AssetManager& /*assets*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
