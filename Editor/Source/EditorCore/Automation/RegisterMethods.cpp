#include "EditorPCH.h"
#include "EditorCore/Automation/RegisterMethods.h"

#include "EditorCore/Automation/AssetMethods.h"
#include "EditorCore/Automation/AutomationTypes.h"
#include "EditorCore/Automation/ComponentMethods.h"
#include "EditorCore/Automation/DebugMethods.h"
#include "EditorCore/Automation/EditMethods.h"
#include "EditorCore/Automation/EntityMethods.h"
#include "EditorCore/Automation/ExportMethods.h"
#include "EditorCore/Automation/ObserveMethods.h"
#include "EditorCore/Automation/PrefabMethods.h"
#include "EditorCore/Automation/ProjectMethods.h"
#include "EditorCore/Automation/SceneMethods.h"
#include "EditorCore/Automation/ScreenshotMethods.h"
#include "EditorCore/Project/ProjectValidator.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Automation/Methods/RegisterSharedMethods.h"

namespace Engine {

	void RegisterEditorMethodTypes(TypeRegistry& registry)
	{
		// Shared types first: the domains' structs use them as field types (TypeRegistry asserts registration before use).
		RegisterAutomationCommonTypes(registry);
		// The domains the Runtime shares (Engine/Automation/Methods; M7): input, play, viewport.screenshot, and the session,
		// rpc, scene, entity and observe reads moved from EditorCore, whose types (SceneSummary, EntityDetails) the editor's
		// own scene and entity results use.
		RegisterSharedMethodTypes(registry);
		// The importers' settings structs (§5.4): asset.getImportSettings and asset.setImportSettings validate against them.
		RegisterAssetPipelineTypes(registry);
		RegisterProjectValidatorTypes(registry);
		RegisterProjectMethodTypes(registry);
		RegisterEditorSceneMethodTypes(registry);
		RegisterEditorEntityMethodTypes(registry);
		RegisterComponentMethodTypes(registry);
		RegisterEditMethodTypes(registry);
		RegisterEditorObserveMethodTypes(registry);
		RegisterScreenshotMethodTypes(registry);
		RegisterAssetMethodTypes(registry);
		RegisterPrefabMethodTypes(registry);
		RegisterExportMethodTypes(registry);
		RegisterDebugMethodTypes(registry);
	}

	void RegisterEditorMethods(MethodRegistry& methods, const EditorMethodOptions& options)
	{
		RegisterProjectMethods(methods);
		RegisterEditorSceneMethods(methods);
		RegisterEditorEntityMethods(methods);
		RegisterComponentMethods(methods);
		RegisterEditMethods(methods);
		RegisterEditorObserveMethods(methods);
		RegisterScreenshotMethods(methods);
		RegisterAssetMethods(methods);
		RegisterPrefabMethods(methods);
		RegisterExportMethods(methods);
		RegisterSharedMethods(methods, AutomationHost::Editor);
		if (options.TestHooks)
			RegisterDebugMethods(methods);
	}

}
