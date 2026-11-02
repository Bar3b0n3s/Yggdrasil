#include "EnginePCH.h"
#include "Engine/Scene/UiSystem.h"

// The play session's UI (UiSystem.h; Docs/Decisions/0033-m22-contract.md decision 3).

namespace Engine {

	void UiSystem::LayoutStep(const Scene& scene, glm::uvec2 sceneSize)
	{
		m_StepLayout = ComputeUiLayout(scene, sceneSize);
		m_FrameLayoutCurrent = false;
	}

	void UiSystem::LayoutFrame(const Scene& scene, glm::uvec2 sceneSize)
	{
		m_FrameLayout = ComputeUiLayout(scene, sceneSize);
		m_FrameLayoutCurrent = true;
	}

	const UiLayoutTable& UiSystem::GetLayout() const
	{
		return m_FrameLayoutCurrent ? m_FrameLayout : m_StepLayout;
	}

}
