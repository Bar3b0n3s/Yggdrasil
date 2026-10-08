#include "EnginePCH.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"

#include <utility>

namespace Engine {

	AutomationMethodContext::AutomationMethodContext(TypeKey hostKey, MethodRequest request)
		: MethodContext(hostKey, std::move(request))
	{
	}

	AutomationMethodContext::~AutomationMethodContext() = default;

	bool AutomationMethodContext::IsHostType(TypeKey key) const
	{
		return key == TypeKeyOf<AutomationMethodContext>() || MethodContext::IsHostType(key);
	}

}
