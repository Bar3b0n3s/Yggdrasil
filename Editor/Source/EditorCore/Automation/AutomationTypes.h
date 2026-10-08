#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"

// Reflected types shared by several editor method domains (the param-struct conventions of
// Engine/Automation/Protocol/MethodRegistry.h). The types the Editor and the Runtime share (SceneTarget, EntitySummary,
// AssetSummary, NoParams, ToAutomationCounter and the engine enums automation reports by name) live in
// Engine/Automation/Methods/AutomationTypes.h since the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 12);
// this header adds what only the editor reports. Registered once by RegisterAutomationCommonTypes, before every domain's
// types (RegisterEditorMethodTypes).

namespace Engine {

	class TypeRegistry;

	// RegisterAutomationSharedTypes, then the editor's own enum CommandOrigin ("CommandOrigin").
	void RegisterAutomationCommonTypes(TypeRegistry& registry);

}
