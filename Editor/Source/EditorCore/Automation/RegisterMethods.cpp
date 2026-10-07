#include "EditorPCH.h"
#include "EditorCore/Automation/RegisterMethods.h"

#include "EditorCore/Automation/AssetMethods.h"
#include "EditorCore/Automation/AutomationTypes.h"
#include "EditorCore/Automation/ComponentMethods.h"
#include "EditorCore/Automation/DebugMethods.h"
#include "EditorCore/Automation/EditMethods.h"
#include "EditorCore/Automation/EntityMethods.h"
#include "EditorCore/Automation/ObserveMethods.h"
#include "EditorCore/Automation/PrefabMethods.h"
#include "EditorCore/Automation/ProjectMethods.h"
#include "EditorCore/Automation/RpcMethods.h"
#include "EditorCore/Automation/SceneMethods.h"
#include "EditorCore/Automation/ScreenshotMethods.h"
#include "EditorCore/Automation/SessionMethods.h"
#include "EditorCore/Project/ProjectValidator.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"

namespace Engine {

	void RegisterEditorMethodTypes(TypeRegistry& registry)
	{
		// Shared types first: the domains' structs use them as field types (TypeRegistry asserts registration before use).
		RegisterAutomationCommonTypes(registry);
		// The importers' settings structs (§5.4): asset.getImportSettings and asset.setImportSettings validate against them.
		RegisterAssetPipelineTypes(registry);
		RegisterProjectValidatorTypes(registry);
		RegisterSessionMethodTypes(registry);
		RegisterRpcMethodTypes(registry);
		RegisterProjectMethodTypes(registry);
		RegisterSceneMethodTypes(registry);
		RegisterEntityMethodTypes(registry);
		RegisterComponentMethodTypes(registry);
		RegisterEditMethodTypes(registry);
		RegisterObserveMethodTypes(registry);
		RegisterScreenshotMethodTypes(registry);
		RegisterAssetMethodTypes(registry);
		RegisterPrefabMethodTypes(registry);
		RegisterDebugMethodTypes(registry);
	}

	void RegisterEditorMethods(MethodRegistry& methods, const EditorMethodOptions& options)
	{
		RegisterSessionMethods(methods);
		RegisterRpcMethods(methods);
		RegisterProjectMethods(methods);
		RegisterSceneMethods(methods);
		RegisterEntityMethods(methods);
		RegisterComponentMethods(methods);
		RegisterEditMethods(methods);
		RegisterObserveMethods(methods);
		RegisterScreenshotMethods(methods);
		RegisterAssetMethods(methods);
		RegisterPrefabMethods(methods);
		if (options.TestHooks)
			RegisterDebugMethods(methods);
	}

}
