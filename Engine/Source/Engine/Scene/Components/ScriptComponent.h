#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>

namespace Engine {

	// Registry name "Script" (Architecture §5.3, §11.2): attaches a Behaviour script (one per entity). Fields holds overrides
	// of the script's declared fields by name, each a Variant resolved against the script's field schema
	// (ScriptFieldResolver in ScriptingRegistration.cpp, through ResolveContext::Schemas, with the Script handle taken from
	// the owner object or, without one, from the owner's JSON); an override with an unknown name or the wrong type is a
	// diagnostic, falls back to the default at run time and stays in the file until fixed (§11.2).
	// Callbacks run in (ExecutionOrder, canonical entity order). No shortcut property: Entity:GetScript() returns the
	// instance (§11.4).
	struct ScriptComponent
	{
		TypedAssetHandle<AssetType::Script> Script;
		std::map<std::string, VariantValue> Fields;
		int32_t ExecutionOrder = 0;
	};

}
