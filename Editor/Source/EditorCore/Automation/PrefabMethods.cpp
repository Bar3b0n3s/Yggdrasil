#include "EditorPCH.h"
#include "EditorCore/Automation/PrefabMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Reflection/TypeRegistry.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements the prefab domain: its handlers,
// the registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods, with
// their Python tests, the method coverage and the regenerated Tools/MCP/catalog.json.

namespace Engine {

	namespace Automation {

		Result<PrefabCreateResult> PrefabCreate(EditorMethodContext& /*context*/, const PrefabCreateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::PrefabCreate is an M6 contract stub");
		}

		Result<PrefabInstantiateResult> PrefabInstantiate(EditorMethodContext& /*context*/, const PrefabInstantiateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::PrefabInstantiate is an M6 contract stub");
		}

		Result<PrefabApplyResult> PrefabApply(EditorMethodContext& /*context*/, const PrefabApplyParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::PrefabApply is an M6 contract stub");
		}

		Result<PrefabRevertResult> PrefabRevert(EditorMethodContext& /*context*/, const PrefabRevertParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::PrefabRevert is an M6 contract stub");
		}

		Result<PrefabUnpackResult> PrefabUnpack(EditorMethodContext& /*context*/, const PrefabUnpackParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::PrefabUnpack is an M6 contract stub");
		}

	}

	void RegisterPrefabMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterPrefabMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
