#include "EnginePCH.h"
#include "Engine/Automation/Methods/RegisterSharedMethods.h"

#include "Engine/Automation/Methods/AudioMethods.h"
#include "Engine/Automation/Methods/EntityMethods.h"
#include "Engine/Automation/Methods/InputMethods.h"
#include "Engine/Automation/Methods/ObserveMethods.h"
#include "Engine/Automation/Methods/PhysicsMethods.h"
#include "Engine/Automation/Methods/PlayMethods.h"
#include "Engine/Automation/Methods/RaycastMethods.h"
#include "Engine/Automation/Methods/ReplayMethods.h"
#include "Engine/Automation/Methods/RpcMethods.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"
#include "Engine/Automation/Methods/ScriptMethods.h"
#include "Engine/Automation/Methods/SessionMethods.h"
#include "Engine/Automation/Methods/StatsMethods.h"

namespace Engine {

	void RegisterSharedMethodTypes(TypeRegistry& registry)
	{
		// The play types use the input types (InputEventParams, PlayInputSummary).
		RegisterInputMethodTypes(registry);
		RegisterPlayMethodTypes(registry);
		RegisterViewportScreenshotMethodTypes(registry);
		// The domains moved from EditorCore with M7 (ADR 0012 decision 12).
		RegisterSessionMethodTypes(registry);
		RegisterRpcMethodTypes(registry);
		RegisterSceneMethodTypes(registry);
		RegisterEntityMethodTypes(registry);
		RegisterObserveMethodTypes(registry);
		// M11 (ADR 0014 decision 18).
		RegisterPhysicsMethodTypes(registry);
		// M12 (ADR 0015): audio.stats.
		RegisterAudioMethodTypes(registry);
		RegisterRaycastMethodTypes(registry);
		RegisterStatsMethodTypes(registry);
		RegisterSharedScriptMethodTypes(registry);
		RegisterReplayMethodTypes(registry);
	}

	void RegisterSharedMethods(MethodRegistry& methods, AutomationHost host)
	{
		RegisterInputMethods(methods);
		RegisterPlayMethods(methods, host == AutomationHost::Editor);
		RegisterViewportScreenshotMethods(methods);
		RegisterSessionMethods(methods);
		RegisterRpcMethods(methods);
		RegisterSceneMethods(methods);
		RegisterEntityMethods(methods);
		RegisterObserveMethods(methods);
		// M11 (ADR 0014 decision 18).
		RegisterPhysicsMethods(methods);
		// M12 (ADR 0015): audio.stats.
		RegisterAudioMethods(methods);
		RegisterRaycastMethods(methods);
		RegisterStatsMethods(methods);
		RegisterSharedScriptMethods(methods);
		RegisterReplayMethods(methods);
	}

}
