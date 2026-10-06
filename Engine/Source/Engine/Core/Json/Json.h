#pragma once

#include "Engine/Core/Base.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Engine {

	// The engine's JSON tree: nlohmann's ordered_json, which keeps object members in insertion order. Parsed documents
	// therefore keep their document order, so data the engine does not understand (unknown components, Variant values)
	// is written back verbatim (§6), and iteration order never depends on a hash. Public headers see only this forward
	// declaration (Architecture §3); typed reads go through JsonReader and writes through JsonWriter. json::at and
	// get<> are banned outside Core/Json (Lint banned-json-access).
	using Json = nlohmann::ordered_json;

	// The deepest nesting of arrays and objects the engine accepts: a scalar document has depth 0, "[]" depth 1 and
	// "[[1]]" depth 2. JsonReader::Parse rejects deeper input and JsonWriter asserts it is never asked to write deeper
	// trees, because copying, destroying and writing a tree recurse once per level: without the bound, a document of a
	// million '[' (an agent's mistake or a corrupted file) would overflow the stack instead of returning an error.
	inline constexpr size_t MaxJsonDepth = 512;

	// The kind of a JSON value. JSON numbers without fraction and exponent parse as Integer, all others as Float.
	enum class JsonType : uint8_t
	{
		Null,
		Bool,
		Integer,
		Float,
		String,
		Array,
		Object
	};

	// The JSON name of a type, as used in error messages: "null", "boolean", "number" (Integer and Float), "string",
	// "array", "object".
	[[nodiscard]] std::string_view JsonTypeToString(JsonType type);

	// The type of `value`.
	[[nodiscard]] JsonType GetJsonType(const Json& value);

}
