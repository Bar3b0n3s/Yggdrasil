#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/Value.h"

#include <cstdint>
#include <functional>
#include <optional>
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
		// With Mixed: the components of a Vec2, Vec3, Vec4 or Bool3 value that differ across the selection (bit i is
		// component i). Only those show as mixed, and an edit reports the components it changed
		// (ReflectedDrawerResult::ChangedComponents) so every object keeps its own untouched components. 0: all differ.
		uint32_t MixedComponents = 0;
		// Synchronous, borrowed for Draw only; candidates own their strings/UUIDs. The caller filters by field kind and
		// AssetFilter, searches label/path/UUID, and sorts by label, path, then UUID. Empty retains typed/drop entry.
		// Errors are displayed and returned. Neither callback nor entity/component pointers are retained by the drawer.
		std::function<Result<std::vector<ReflectedReferenceCandidate>>(const FieldInfo&, std::string_view)> FindReferences{};
		// Optional immutable declaration for authored array defaults. Borrowed only for the synchronous Draw call;
		// recursive drawers may use it until Draw returns, but must never retain it or capture it for later work.
		const ScriptFieldSchema* ScriptSchema = nullptr;
		// Presentation aliases never change reflected paths. Authored map keys are supplied verbatim here.
		std::string DisplayLabel{};
		// Optional memory-only lookup for the current reference; independent of the searchable picker.
		std::function<std::optional<ReflectedReferenceCandidate>(const FieldInfo&, UUID)> DescribeReference{};
	};
	struct ReflectedDrawerResult
	{
		bool Activated = false;
		bool Changed = false;
		bool Committed = false;
		bool Cancelled = false;
		bool Active = false; // any submitted child owns the gesture; false when its container closes
		// The components of a Vec2, Vec3, Vec4 or Bool3 value this draw changed (bit i is component i). 0 for every other
		// kind, and for containers and rotations, whose edits replace the whole value.
		uint32_t ChangedComponents = 0;
	};

	// Main thread inside an ImGui frame. Draw every FieldType (Map and Variant included), metadata range/unit/tooltips,
	// entity/asset pickers and enum choices from the registry. Edits affect the owned Value only; controller commits.
	// Map keys are stable IDs, add/rename rejects duplicates. Unresolved Variant is read-only raw JSON plus diagnostic.
	// Numbers: drags and typed entries clamp to Min/Max and step over the MinMagnitude gap, so a normal edit stays valid;
	// floats display up to 7 significant digits and are never rounded to the display format (-0 displays as 0).
	// Quat: Euler degrees in Transform.EulerAngles' convention, plus raw components that are renormalized after an edit.
	// Errors: Validation for incompatible value/schema, never cast unchecked external data or dereference unknown owner.
	[[nodiscard]] Result<ReflectedDrawerResult> DrawReflectedValue(const FieldInfo& field, Value& value,
		const ReflectedDrawerContext& context);

}
