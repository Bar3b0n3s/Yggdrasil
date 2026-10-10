#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/Value.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class TypeRegistry;
	struct ScriptFieldSchema;

	struct ReflectedReferenceCandidate
	{
		UUID Id{};
		std::string Label{};
		std::string Path{};
	};

	struct ReflectedDrawerContext
	{
		const TypeRegistry& Types; // borrowed for this call
		ResolveContext Resolve{};  // real owner/type/key, valid only during this call
		std::string Path{};        // stable reflected path in ImGui IDs, never just the display label
		bool ReadOnly = false;
		bool Mixed = false; // multi-selection differs; edit replaces all, never silently picks the first
		// Synchronous, borrowed for Draw only; candidates own their strings/UUIDs. The caller filters by field kind and
		// AssetFilter, searches label/path/UUID, and sorts by label, path, then UUID. Empty retains typed/drop entry.
		// Errors are displayed and returned. Neither callback nor entity/component pointers are retained by the drawer.
		std::function<Result<std::vector<ReflectedReferenceCandidate>>(const FieldInfo&, std::string_view)> FindReferences{};
		// Optional immutable declaration for authored array defaults. Borrowed only for the synchronous Draw call;
		// recursive drawers may use it until Draw returns, but must never retain it or capture it for later work.
		const ScriptFieldSchema* ScriptSchema = nullptr;
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
