#include "EnginePCH.h"
#include "Engine/Automation/Methods/StatsMethods.h"

namespace Engine {

	Result<StatsGetResult> Automation::StatsGet(AutomationMethodContext&, const StatsGetParams&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	void RegisterStatsMethodTypes(TypeRegistry&)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterStatsMethods(MethodRegistry&)
	{
		ENGINE_CONTRACT_STUB();
	}

}
