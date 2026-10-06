#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"

// RFC 6902 JSON Patch generation for scene.diff (Architecture §13.5 "RFC 6902 per entity", §13.7): the operations that turn
// one entity's canonical JSON into another's, so an agent reviews exactly what changed.

namespace Engine {

	// The JSON Patch (an array of operations) from `from` to `to`, deterministic and minimal per member:
	//   - objects are compared member by member: first every member only in `from` is {"op": "remove", "path"} (in `from`'s
	//     order), then `to`'s members in order: a member only in `to` is {"op": "add", "path", "value"}, a member in both
	//     contributes its own operations (recursively);
	//   - arrays that differ are replaced whole ({"op": "replace"}) unless both have the same length, in which case the
	//     elements are compared index by index (vectors, colours and quaternions then show the changed component);
	//   - any other difference, including a type change, is {"op": "replace", "path", "value"}.
	// Paths are RFC 6901 pointers with "~" and "/" escaped ("~0", "~1"). Applying the patch to `from` yields `to`. Equal
	// documents give an empty array. Pure; thread-safe.
	[[nodiscard]] Json DiffJson(const Json& from, const Json& to);

}
