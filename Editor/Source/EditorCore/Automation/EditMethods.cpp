#include "EditorPCH.h"
#include "EditorCore/Automation/EditMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream C (methods) implements the edit domain: its handlers, the
// registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods.

namespace Engine {

	namespace Automation {

		Result<EditBatchResult> EditBatch(EditorMethodContext& /*context*/, const EditBatchParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EditBatch is an M4 contract stub");
		}

		Result<EditUndoResult> EditUndo(EditorMethodContext& /*context*/, const EditUndoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EditUndo is an M4 contract stub");
		}

		Result<EditRedoResult> EditRedo(EditorMethodContext& /*context*/, const EditRedoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EditRedo is an M4 contract stub");
		}

		Result<EditHistoryResult> EditHistory(EditorMethodContext& /*context*/, const EditHistoryParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EditHistory is an M4 contract stub");
		}

		Result<EditSelectResult> EditSelect(EditorMethodContext& /*context*/, const EditSelectParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EditSelect is an M4 contract stub");
		}

		Result<EditGetSelectionResult> EditGetSelection(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EditGetSelection is an M4 contract stub");
		}

	}

	void RegisterEditMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterEditMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
