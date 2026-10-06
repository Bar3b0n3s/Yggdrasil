#include "EditorPCH.h"
#include "EditorCore/Automation/SessionMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream C (methods) implements the session domain: its handlers, the
// registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods.

namespace Engine {

	namespace Automation {

		Result<SessionHelloResult> SessionHello(EditorMethodContext& /*context*/, const SessionHelloParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SessionHello is an M4 contract stub");
		}

		Result<SessionInfoResult> SessionInfo(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SessionInfo is an M4 contract stub");
		}

		Result<SessionShutdownResult> SessionShutdown(EditorMethodContext& /*context*/, const SessionShutdownParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SessionShutdown is an M4 contract stub");
		}

	}

	void RegisterSessionMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterSessionMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
