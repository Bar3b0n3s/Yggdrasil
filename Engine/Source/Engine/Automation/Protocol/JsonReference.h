#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>
#include <string>

// Result references of atomic batches and batch files (Architecture §13.4 "Atomic batch", §6.7): {"$ref": "3.entity.id"}
// inside an op's params stands for the value at "entity.id" in the result of op 3, so a batch can parent entity 14 under
// the entity op 13 created without knowing its id in advance.

namespace Engine {

	// Replaces, anywhere inside `value`, every object that has exactly one member "$ref" whose value is a string
	// "<index>.<path>" by the value found in results[index - indexBase] at <path>. <index> is a decimal number without sign
	// or leading zeros; <path> is a dot-separated list of object keys and array indices ("entities.0.id"); an index without
	// a path ("3") stands for the whole result. edit.batch numbers its ops from 0 (indexBase 0); batch files number their
	// lines from 1 (indexBase 1). Only earlier results can be referenced: `results` holds exactly those.
	// Objects with "$ref" next to other members are left alone (they are data). Errors, with `value` unchanged:
	// InvalidArgument located at the "$ref" object's pointer for a malformed reference, an index outside `results` (a self or
	// forward reference), or a path that names nothing ("no member 'entity' in the result of op 3", or "no member 'ids' in
	// '3.prefab' (in the result of op 3)" for an object inside the result: the message and the hint name the object
	// inspected; the hint lists the members that object has, sorted, at most 32 and then how many more, after "did you
	// mean" for close ones, also given as the issue's suggestions).
	[[nodiscard]] Status SubstituteReferences(Json& value, std::span<const Json> results, size_t indexBase);

	// M19 (Docs/Decisions/0030-m19-contract.md decision 8, A-NREF): as above, where "<index>" may also be an op's name, so
	// {"$ref": "board.entity.id"} stands for the entity of the op named "board" and does not shift when an op is inserted
	// before it. `names` holds every op's name in op order, the whole batch's ("" for an unnamed op), so it is at least as
	// long as `results` (asserted), which holds the earlier ops' results. A name is an identifier, [A-Za-z_] followed by
	// [A-Za-z0-9_] (so it never reads as an index; edit.batch checks the ops' names before any runs); the part before the
	// first '.' that is not a decimal index is looked up among `names`. Errors, with `value` unchanged, located as above:
	// InvalidArgument for a name of a later op or of this one ("'<name>' names op <k>, which runs after this one": a forward
	// reference), and for a name no op has ("no op named '<name>' before this one", the hint listing the earlier ops' names,
	// at most 32, after "did you mean" for close ones); the path errors as above, naming the op by its name ("no member
	// 'entity' in the result of op 'board'").
	[[nodiscard]] Status SubstituteReferences(Json& value, std::span<const Json> results, size_t indexBase, std::span<const std::string> names);

}
