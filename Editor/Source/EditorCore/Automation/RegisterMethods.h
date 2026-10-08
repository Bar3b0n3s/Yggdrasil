#pragma once

#include "Engine/Core/Base.h"

// The registration of every editor automation method (Roadmap rule 4: RegisterMethods.cpp is a shared integration file
// with one owner per milestone). Adding a method: its param and result structs and handler in the domain's
// header and .cpp, its types and registration in that domain's Register*MethodTypes and Register*Methods, and a Python test
// (skill add-automation-method).

namespace Engine {

	class MethodRegistry;
	class TypeRegistry;

	struct EditorMethodOptions
	{
		// --automation-test-hooks: also register the debug.* test hooks.
		bool TestHooks = false;
	};

	// Every editor automation struct and enum, in dependency order: RegisterAutomationCommonTypes, the shared domains'
	// types (RegisterSharedMethodTypes, Engine/Automation/Methods, M7), the importers' settings types
	// (RegisterAssetPipelineTypes, M6), RegisterProjectValidatorTypes, then each editor domain's Register*MethodTypes
	// (session, rpc, project, scene, entity, component, edit, observe, screenshot, asset, prefab, export, debug). It is the
	// editor's EngineContextSpecification::RegisterTypes.
	void RegisterEditorMethodTypes(TypeRegistry& registry);

	// Every editor method: each editor domain's Register*Methods in the order above, then the shared methods
	// (RegisterSharedMethods with AutomationHost::Editor), then debug only with options.TestHooks.
	void RegisterEditorMethods(MethodRegistry& methods, const EditorMethodOptions& options);

}
