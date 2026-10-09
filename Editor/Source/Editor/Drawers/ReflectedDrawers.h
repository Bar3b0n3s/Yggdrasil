#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/Value.h"

#include <string>

namespace Engine {

	class TypeRegistry;

	struct ReflectedDrawerContext
	{
		const TypeRegistry& Types; // borrowed for this call
		ResolveContext Resolve{};  // real owner/type/key, valid only during this call
		std::string Path{};        // stable reflected path in ImGui IDs, never just the display label
		bool ReadOnly = false;
		bool Mixed = false; // multi-selection differs; edit replaces all, never silently picks the first
	};
	struct ReflectedDrawerResult
	{
		bool Activated = false;
		bool Changed = false;
		bool Committed = false;
		bool Cancelled = false;
	};

	// Main thread inside an ImGui frame. Draw every FieldType (Map and Variant included), metadata range/unit/tooltips,
	// entity/asset pickers and enum choices from the registry. Edits affect the owned Value only; controller commits.
	// Map keys are stable IDs, add/rename rejects duplicates. Unresolved Variant is read-only raw JSON plus diagnostic.
	// Errors: Validation for incompatible value/schema, never cast unchecked external data or dereference unknown owner.
	[[nodiscard]] Result<ReflectedDrawerResult> DrawReflectedValue(const FieldInfo& field, Value& value,
		const ReflectedDrawerContext& context);

}
