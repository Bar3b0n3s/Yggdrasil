#include "EditorPCH.h"
#include "EditorCore/Automation/EditorMethods.h"

namespace Engine {

	Result<EditorStateResult> Automation::EditorState(EditorMethodContext& /*context*/, const NoParams& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void RegisterEditorStateMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterEditorStateMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
