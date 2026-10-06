#include "EditorPCH.h"
#include "EditorCore/Automation/ObserveMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream C (methods) implements the observe domain: its handlers, the
// registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods.

namespace Engine {

	namespace Automation {

		Result<LogReadMethodResult> LogRead(EditorMethodContext& /*context*/, const LogReadParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::LogRead is an M4 contract stub");
		}

		Result<EventsReadResult> EventsRead(EditorMethodContext& /*context*/, const EventsReadParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EventsRead is an M4 contract stub");
		}

		Result<DocsGetResult> DocsGet(EditorMethodContext& /*context*/, const DocsGetParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::DocsGet is an M4 contract stub");
		}

	}

	void RegisterObserveMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterObserveMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
