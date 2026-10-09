#include "EnginePCH.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"

#include "Engine/Automation/Methods/StatsMethods.h"

namespace Engine {

	const ProjectSettings* AutomationMethodContext::GetProjectSettings() const
	{
		return nullptr;
	}

	Result<StatsGetResult> AutomationMethodContext::GetHostStatistics() const
	{
		return std::unexpected(Error(ErrorCode::Unsupported, "this host does not provide statistics"));
	}

	std::vector<UUID> AutomationMethodContext::GetSelectedEntities() const
	{
		return {};
	}

}
