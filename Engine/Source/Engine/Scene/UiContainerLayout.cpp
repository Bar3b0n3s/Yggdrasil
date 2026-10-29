#include "EnginePCH.h"
#include "Engine/Scene/UiContainerLayout.h"

// The UiLayout containers (UiContainerLayout.h): a stub of the M22 contract (C22.1, Docs/Decisions/0033-m22-contract.md
// decision 5) that stream D implements.

namespace Engine {

	std::vector<UiContainerPlacement> LayoutUiContainer(const UiLayoutComponent& /*layout*/, glm::vec2 /*containerSize*/,
		std::span<const UiContainerChild> /*children*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
