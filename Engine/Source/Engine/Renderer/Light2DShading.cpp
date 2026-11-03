#include "EnginePCH.h"
#include "Engine/Renderer/Light2DShading.h"

// The M21 contract's stubs (Docs/Decisions/0032-m21-contract.md decision 22): stream L implements the 2D light model.

namespace Engine {

	glm::vec3 EvaluateLight2D(const Light2DItem& /*light*/, glm::vec2 /*position*/, const std::optional<glm::vec3>& /*normal*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	glm::vec3 EvaluateLights2D(std::span<const Light2DItem> /*lights*/, glm::vec2 /*position*/, const std::optional<glm::vec3>& /*normal*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

}
