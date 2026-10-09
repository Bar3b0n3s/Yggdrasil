#include "EditorPCH.h"
#include "EditorCore/Automation/EditorAutomationControls.h"

namespace Engine {

	EditorAutomationControls::EditorAutomationControls(EditorContext& /*context*/, AutomationServer& /*server*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	EditorAutomationControls::~EditorAutomationControls()
	{
		ENGINE_CONTRACT_STUB();
	}

	EditorAutomationPolicy EditorAutomationControls::GetPolicy() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	void EditorAutomationControls::SetPolicy(const EditorAutomationPolicy& /*policy*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<EditorRequestActivity> EditorAutomationControls::GetRecentRequests() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status EditorAutomationControls::SetAllowAiAutomation(bool /*allowed*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
