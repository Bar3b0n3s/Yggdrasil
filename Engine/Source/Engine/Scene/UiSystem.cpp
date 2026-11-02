#include "EnginePCH.h"
#include "Engine/Scene/UiSystem.h"

#include <utility>

// The play session's UI (UiSystem.h; Docs/Decisions/0033-m22-contract.md decision 3). The interaction's queue and reset are
// final; ProcessInput, SetFocus and AppendStateHash are stubs of the M22 contract (C22.2, decisions 24, 28 and 29) that
// stream A implements.

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

	void UiSystem::ProcessInput(Scene& /*scene*/, const UiInputFrame& /*input*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status UiSystem::SetFocus(Scene& /*scene*/, UUID /*element*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "UiSystem::SetFocus is not implemented yet (Docs/Decisions/0033-m22-contract.md decision 28, stream A)");
	}

	std::vector<UiEvent> UiSystem::TakeEvents()
	{
		return std::exchange(m_Events, {});
	}

	void UiSystem::ResetInteraction()
	{
		m_Interaction = {};
		m_Events.clear();
	}

	void UiSystem::AppendStateHash(XXH64Hasher& /*hasher*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

}
