#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <string_view>

namespace Engine {

	class FieldInfo;
	class StructInfo;
	class TypeRegistry;

	// JSON Schema (draft 2020-12) generation from the registry and a validator for the subset the generator emits
	// (Architecture §5.4, §13.4: automation parameter schemas with $defs per component, Docs/Reference/Schemas/, and the
	// registry suite's "schema validates output" check). Static functions only; pure and thread-safe once the registry is
	// frozen.
	//
	// Mapping (every schema carries "description" from the registry):
	//   Bool -> {"type": "boolean"}; Int32/UInt32 -> {"type": "integer"} with the type's range and Min/Max as
	//   "minimum"/"maximum"; Float -> {"type": "number"} with Min/Max; Vec2/Vec3/Vec4/Quat/Color3/Color4 -> an array of
	//   exactly 2, 3 or 4 numbers ("prefixItems" with the per-component range, "minItems" = "maxItems"); MinMagnitude -> per
	//   component "not": {"exclusiveMinimum": -m, "exclusiveMaximum": m}; Bool3 -> an array of exactly 3 booleans;
	//   String -> {"type": "string"}; EntityRef -> {"type": ["string", "null"], "pattern": "^[0-9a-fA-F]{16}$"}; AssetRef ->
	//   {"type": ["string", "null"], "pattern": "^([0-9a-fA-F]{16}|Assets/.+|engine://.+)$"} (a handle, which files store,
	//   or the project and engine paths automation params also accept, §7.1 and MethodRegistry.h convention 13), plus
	//   "x-assetType" with an AssetFilter; Enum -> {"enum": [names in registration order]};
	//   Array -> {"type": "array", "items": element}; Struct -> {"$ref": "#/$defs/<StructName>"} with the definition
	//   {"type": "object", "properties": ..., "additionalProperties": false} (serialized fields only, in field order;
	//   "required" is omitted because every field is optional on read, §6); Map -> {"type": "object",
	//   "additionalProperties": value schema}; Variant -> {} (any JSON value; its schema is resolved at run time).
	class JsonSchema
	{
	public:
		static constexpr std::string_view Dialect = "https://json-schema.org/draft/2020-12/schema";

		// The schema of one serialized object of `type`: "$schema", "title" (the registry name), the object definition at
		// the root, and "$defs" holding every struct and component it references, transitively, keyed by registry name.
		[[nodiscard]] static Json ForStruct(const StructInfo& type);

		// The schema of a value of `field` (no "$schema"; "$defs" when it references structs).
		[[nodiscard]] static Json ForField(const FieldInfo& field);

		// One document with every registered component in "$defs" and a root object schema whose properties map each
		// serializable, non-entity-level component name to its definition: the shape of an entity's "Components" object.
		[[nodiscard]] static Json ForComponents(const TypeRegistry& registry);

		// Validates `instance` against `schema`, supporting exactly the keywords the generator emits: "$ref" to "#/$defs/...",
		// "type" (string or array), "properties", "additionalProperties" (boolean or schema), "items", "prefixItems",
		// "minItems", "maxItems", "enum", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "not", "pattern"
		// (the two patterns above only) and "required" (which automation params schemas add, MethodRegistry::GetParamsSchema);
		// annotations ("description", "title", "x-assetType", "$schema") are ignored.
		// Errors: Validation carrying one ErrorIssue per violation, located at the instance's JSON pointer; InvalidArgument
		// for a schema that uses another keyword or an unresolvable "$ref".
		[[nodiscard]] static Status Validate(const Json& schema, const Json& instance);
	};

}
