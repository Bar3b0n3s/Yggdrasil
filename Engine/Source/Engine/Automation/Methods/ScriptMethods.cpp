#include "EnginePCH.h"
#include "Engine/Automation/Methods/ScriptMethods.h"

#include "Engine/Core/Base.h"

namespace Engine {

	namespace Automation {

		Result<ScriptErrorsResult> ScriptErrors(AutomationMethodContext& /*context*/, const ScriptErrorsParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "script.errors contract is not implemented");
		}

		Result<ScriptEvalResult> ScriptEval(AutomationMethodContext& /*context*/, const ScriptEvalParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "script.eval contract is not implemented");
		}

	}

	void RegisterSharedScriptMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterSharedScriptMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
