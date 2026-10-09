#include "EnginePCH.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"

#include "Engine/Automation/Methods/StatsMethods.h"

namespace Engine {

	const ProjectSettings* AutomationMethodContext::GetProjectSettings() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Result<StatsGetResult> AutomationMethodContext::GetHostStatistics() const
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	std::vector<UUID> AutomationMethodContext::GetSelectedEntities() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
