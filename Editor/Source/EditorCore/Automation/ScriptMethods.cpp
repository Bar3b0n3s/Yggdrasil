#include "EditorPCH.h"
#include "EditorCore/Automation/ScriptMethods.h"

#include "Engine/Core/Base.h"

namespace Engine {

	namespace Automation {

		Result<ScriptCreateResult> ScriptCreate(EditorMethodContext& /*context*/, const ScriptCreateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "script.create contract is not implemented");
		}

		Result<ScriptReadResult> ScriptRead(EditorMethodContext& /*context*/, const ScriptReadParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "script.read contract is not implemented");
		}

		Result<ScriptWriteResult> ScriptWrite(EditorMethodContext& /*context*/, const ScriptWriteParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "script.write contract is not implemented");
		}

		Result<ScriptCheckResult> ScriptCheck(EditorMethodContext& /*context*/, const ScriptCheckParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "script.check contract is not implemented");
		}

		Result<ScriptFieldsResult> ScriptFields(EditorMethodContext& /*context*/, const ScriptFieldsParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "script.fields contract is not implemented");
		}

	}

	void RegisterEditorScriptMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterEditorScriptMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
