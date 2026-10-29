#include "EnginePCH.h"
#include "Engine/Scene/UiSystem.h"

// The play session's UI (UiSystem.h): stubs of the M22 contract (C22.1, Docs/Decisions/0033-m22-contract.md decision 3) that
// stream A implements.

namespace Engine {

	void UiSystem::LayoutStep(const Scene& /*scene*/, glm::uvec2 /*sceneSize*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void UiSystem::LayoutFrame(const Scene& /*scene*/, glm::uvec2 /*sceneSize*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	const UiLayoutTable& UiSystem::GetLayout() const
	{
		ENGINE_CONTRACT_STUB();
		return m_StepLayout;
	}

}
