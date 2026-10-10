#include "EnginePCH.h"
#include "Engine/Automation/Methods/ReplayMethods.h"

#include "Engine/Automation/Protocol/PendingOperation.h"

namespace Engine {

	namespace Automation {

		Result<InputRecordResult> InputRecord(AutomationMethodContext& /*context*/, const InputRecordParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Input recording is an M13 contract stub");
		}

		Result<Scope<PendingOperation>> InputReplay(AutomationMethodContext& /*context*/, const InputReplayParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Input replay is an M13 contract stub");
		}

	}

	void RegisterReplayMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterReplayMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
