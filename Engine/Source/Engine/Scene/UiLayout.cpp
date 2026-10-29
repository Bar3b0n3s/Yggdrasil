#include "EnginePCH.h"
#include "Engine/Scene/UiLayout.h"

// UI layout (UiLayout.h): stubs of the M22 contract (C22.1, Docs/Decisions/0033-m22-contract.md decision 4) that stream A
// implements, calling stream D's containers (Scene/UiContainerLayout.h).

namespace Engine {

	UiLayoutTable ComputeUiLayout(const Scene& /*scene*/, glm::uvec2 /*sceneSize*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	UiClipRect ComputeUiClipRect(glm::vec2 /*min*/, glm::vec2 /*max*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const UiLayoutNode* FindUiLayoutNode(const UiLayoutTable& /*table*/, UUID /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const UiCanvasLayout* FindUiCanvasLayout(const UiLayoutTable& /*table*/, UUID /*canvas*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
