#include "EditorPCH.h"
#include "EditorCore/Automation/RpcMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream C (methods) implements the rpc domain: its handlers, the
// registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods.

namespace Engine {

	namespace Automation {

		Result<RpcDiscoverResult> RpcDiscover(EditorMethodContext& /*context*/, const RpcDiscoverParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::RpcDiscover is an M4 contract stub");
		}

	}

	void RegisterRpcMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterRpcMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
