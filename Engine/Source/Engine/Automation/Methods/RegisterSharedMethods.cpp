#include "EnginePCH.h"
#include "Engine/Automation/Methods/RegisterSharedMethods.h"

#include "Engine/Automation/Methods/InputMethods.h"
#include "Engine/Automation/Methods/PlayMethods.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"

namespace Engine {

	void RegisterSharedMethodTypes(TypeRegistry& registry)
	{
		// The play types use the input types (InputEventParams, PlayInputSummary).
		RegisterInputMethodTypes(registry);
		RegisterPlayMethodTypes(registry);
		RegisterViewportScreenshotMethodTypes(registry);
	}

	void RegisterSharedMethods(MethodRegistry& methods, AutomationHost host)
	{
		RegisterInputMethods(methods);
		RegisterPlayMethods(methods, host == AutomationHost::Editor);
		RegisterViewportScreenshotMethods(methods);
	}

}
