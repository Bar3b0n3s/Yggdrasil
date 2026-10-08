#include "EnginePCH.h"
#include "Engine/Automation/Methods/InputMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

namespace Engine {

	namespace Automation {

		Result<PlayInputEvent> MakePlayInputEvent(const AutomationMethodContext& /*context*/, const InputEventParams& /*event*/,
			std::string_view /*pointer*/, const InputActionMap& /*actions*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "input events are not implemented yet (M7 stream A)");
		}

		Result<InputInjectResult> InputInject(AutomationMethodContext& /*context*/, const InputInjectParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "input.inject is not implemented yet (M7 stream A)");
		}

	}

	void RegisterInputMethodTypes(TypeRegistry& /*registry*/)
	{
		// Registers nothing until stream A lands input.inject with the catalogue update (ADR 0012 decision 13).
		ENGINE_CONTRACT_STUB();
	}

	void RegisterInputMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
