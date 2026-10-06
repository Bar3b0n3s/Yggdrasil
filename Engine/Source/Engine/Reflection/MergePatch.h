#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"

namespace Engine {

	// RFC 7386 JSON Merge Patch on JSON trees (Architecture §5.4: Map fields, automation component patches, prefab
	// override updates). Pure functions; thread-safe for distinct arguments. Both inputs nest at most MaxJsonDepth deep
	// (asserted; JsonReader::Parse never produces deeper trees), and the recursion is bounded by that depth.

	// The result of applying `patch` to `target`, exactly as RFC 7386 section 2 defines MergePatch(Target, Patch):
	//   - a non-object patch replaces the target whole;
	//   - otherwise a non-object target is treated as {}, and for each patch member: null deletes the key, any other value
	//     replaces the key's value with MergePatch(target[key], value);
	//   - arrays are replaced whole, never merged.
	// Member order: surviving target members keep their order and new members are appended in patch order, so a canonical
	// writer still decides the final key order (registry order or sorted Map keys).
	[[nodiscard]] Json ApplyMergePatch(const Json& target, const Json& patch);

	// A merge patch that turns `source` into `target`: ApplyMergePatch(source, CreateMergePatch(source, target)) == target
	// whenever `target` contains no null member value inside an object (RFC 7386 cannot express setting a member to null;
	// such a member is set by replacing its parent object whole). Equal inputs give {}. Members are listed in target order,
	// deletions (null) after them in source order.
	[[nodiscard]] Json CreateMergePatch(const Json& source, const Json& target);

}
