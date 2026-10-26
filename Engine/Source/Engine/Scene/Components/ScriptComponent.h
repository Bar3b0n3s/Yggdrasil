#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Engine {

	// Registry struct "ScriptSlot" (Docs/Decisions/0031-m20-contract.md decision 17): one behaviour of a Script component.
	// Fields holds overrides of the slot's own script's declared fields by name, each a Variant resolved against that
	// script's field schema (ScriptFieldResolver in ScriptingRegistration.cpp, through ResolveContext::Schemas, with the
	// Script handle taken from the owning slot object or, without one, from the slot's JSON); an override with an unknown
	// name or the wrong type is a diagnostic, falls back to the default at run time and stays in the file until fixed
	// (§11.2). A slot without a script creates no instance.
	struct ScriptSlot
	{
		TypedAssetHandle<AssetType::Script> Script;
		std::map<std::string, VariantValue> Fields;
		int32_t ExecutionOrder = 0;
	};

	// Registry name "Script", version 2 (Architecture §5.3, §11.2; Docs/Decisions/0031-m20-contract.md decision 17):
	// attaches Behaviour scripts to the entity as an ordered list of slots. A slot is identified by its index in the list,
	// each slot with a script is its own instance, the same script may fill two slots, and an empty list is valid.
	// Callbacks run in (ExecutionOrder, canonical entity order, slot) order. Version 1's {Script, Fields, ExecutionOrder}
	// migrates to a one-slot list. No shortcut property: Entity:GetScript() returns an instance (§11.4).
	struct ScriptComponent
	{
		std::vector<ScriptSlot> Scripts;
	};

}
