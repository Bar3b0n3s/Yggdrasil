#include "EditorPCH.h"
#include "EditorCore/Automation/TestMethods.h"

#include "Engine/Automation/Protocol/PendingOperation.h"

namespace Engine {

	namespace Automation {

		Result<TestListResult> TestList(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "test.list contract is not implemented");
		}

		Result<Scope<PendingOperation>> TestRun(EditorMethodContext& /*context*/, const TestRunParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "test.run contract is not implemented");
		}

	}

	void RegisterTestMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterTestMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
