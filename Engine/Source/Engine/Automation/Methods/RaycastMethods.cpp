#include "EnginePCH.h"
#include "Engine/Automation/Methods/RaycastMethods.h"

namespace Engine {

	Result<SceneRaycastResult> Automation::SceneRaycast(AutomationMethodContext&, const SceneRaycastParams&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	void RegisterRaycastMethodTypes(TypeRegistry&)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterRaycastMethods(MethodRegistry&)
	{
		ENGINE_CONTRACT_STUB();
	}

}
