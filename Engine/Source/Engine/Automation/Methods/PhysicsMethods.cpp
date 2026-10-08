#include "EnginePCH.h"
#include "Engine/Automation/Methods/PhysicsMethods.h"

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md decision 18): stream D implements and registers physics.bodyInfo
// (and its types) with its tests, as ADR 0012 decision 13 had each M7 stream register its methods. The registration calls
// are wired in RegisterSharedMethods.cpp, so the catalogue and the coverage gate change only when the method lands.

namespace Engine {

	namespace Automation {

		Result<PhysicsBodyInfoResult> PhysicsBodyInfo(AutomationMethodContext& /*context*/, const PhysicsBodyInfoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "physics.bodyInfo is not implemented yet (M11 stream D)");
		}

	}

	void RegisterPhysicsMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterPhysicsMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
