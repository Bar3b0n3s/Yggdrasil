#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>

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
	// forward reference), or a path that names nothing ("no member 'entity' in the result of op 3").
	[[nodiscard]] Status SubstituteReferences(Json& value, std::span<const Json> results, size_t indexBase);

}
