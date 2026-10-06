#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/Value.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The recursive operations behind FieldInfo, StructInfo, Value, RandomValueGenerator and JsonSchema: every walk over a
// reflected type (C++ object, Value or JSON) lives here once, so reading, writing and validating cannot disagree.
//
// Location convention: the validating walks report issues through a ValidationContext whose current pointer is the
// location of the value being walked, and descend with PushKey/PopKey. The JsonReaders they receive are views of the
// value only; their own pointers are not used for locating, so callers may pass readers whose pointer is "".

namespace Engine {

	class IFieldSchemaSource;

	namespace Detail {

		// The attorney through which Reflection's walks reach the private state of FieldInfo and StructInfo: a field's
		// back-reference to its declaring struct, and a struct's type-level validators without its field checks.
		struct ReflectionAccess
		{
			[[nodiscard]] static const StructInfo* GetOwner(const FieldInfo& field) { return field.m_Owner; }
			static void SetOwner(FieldInfo& field, const StructInfo* owner) { field.m_Owner = owner; }

			static void RunValidators(const StructInfo& type, const void* object, ValidationContext& context)
			{
				for (const Scope<StructValidator>& validator : type.m_Validators)
					(*validator)(object, context);
			}
		};

	}

	namespace Utils {

		// How a walk treats Variant values whose resolved schema they do not match (ADR 0006 decision 6): reads keep them and
		// warn with REFLECTION_VARIANT_MISMATCH, writes and validation of in-memory data report the schema's errors. An
		// unresolvable value is a REFLECTION_VARIANT_UNRESOLVED warning either way.
		enum class VariantPolicy : uint8_t
		{
			Read,
			Write
		};

		// How many resolved Variant values one value may nest inside each other. A resolved schema may itself hold Variant
		// values (an AddComponent override's Script.Fields), and every level costs a full read walk on the stack, so a file
		// must not choose the depth. Legitimate data nests two or three levels.
		inline constexpr uint32_t MaxVariantNesting = 16;

		struct WalkContext
		{
			ValidationContext* Validation = nullptr; // required
			const IFieldSchemaSource* Schemas = nullptr;
			VariantPolicy Policy = VariantPolicy::Write;
			uint32_t VariantDepth = 0; // resolved Variant values around the value being walked
		};

		// --- JSON and type helpers

		// The nesting depth of `value` (a scalar is 0, "[]" is 1), computed without recursion.
		[[nodiscard]] size_t ComputeJsonDepth(const Json& value);

		// A Validation error located at `pointer`.
		[[nodiscard]] Error MakeLocatedValidationError(std::string pointer, std::string message);

		// `error` with `prefix` put in front of its JSON pointer (an unset pointer counts as "").
		[[nodiscard]] Error PrependPointer(Error error, std::string_view prefix);

		// Adds `error` to `validation` as an Error issue whose pointer is the current pointer followed by the error's own
		// pointer (relative to the value that produced it), with its hint and the suggestions of its first issue.
		void AddErrorIssue(const Error& error, ValidationContext& validation);

		// True for Variant and for arrays and maps whose elements contain Variant values.
		[[nodiscard]] bool ContainsVariant(const TypeInfo& type);

		// The name used in "expected <type>" messages: the registry name of an enum or struct, the kind name otherwise.
		[[nodiscard]] std::string DescribeType(const TypeInfo& type);

		// The serialized field of `type` named `name` (stored, or schema-only in a schema struct; never virtual), or nullptr.
		[[nodiscard]] const FieldInfo* FindSerializedField(const StructInfo& type, std::string_view name);

		// Up to three serialized field names of `type` close to `name`.
		[[nodiscard]] std::vector<std::string> SuggestSerializedFieldNames(const StructInfo& type, std::string_view name);

		// --- Scalars (every kind for which IsScalarFieldType is true, Variant included)

		// The §6 spelling of a scalar Value of `type`. Errors carry pointers relative to the value ("/1" for the y of a
		// vector).
		[[nodiscard]] Result<Json> ScalarToJson(const Value& value, const TypeInfo& type);

		// The strict reading of a scalar of `type`. Errors are located in `reader`'s pointer frame.
		[[nodiscard]] Result<Value> ScalarFromJson(const JsonReader& reader, const TypeInfo& type);

		// Checks a scalar Value whose kind equals type.GetKind(): finite floats, the ranges of `meta` when it is not null,
		// unit quaternions, valid UTF-8 strings and enum values that name an enumerator. Variant values are checked by
		// CheckVariant.
		void CheckScalarValue(const Value& value, const TypeInfo& type, const FieldMeta* meta, ValidationContext& validation);

		// Errors for a JSON tree the canonical writer would reject (non-finite or out-of-float-range numbers, invalid UTF-8).
		void CheckWritableJson(const Json& value, ValidationContext& validation);

		// --- Values

		// ValueToJson: errors carry pointers relative to the value.
		[[nodiscard]] Result<Json> ConvertValueToJson(const Value& value, const TypeInfo& type);

		// ValueFromJson: errors are located in `reader`'s pointer frame.
		[[nodiscard]] Result<Value> ConvertJsonToValue(const JsonReader& reader, const TypeInfo& type);

		// --- Walks over C++ objects, Values and JSON

		// The canonical JSON of the object of `type` at `object`. Errors carry pointers relative to the object.
		[[nodiscard]] Result<Json> ObjectToJson(const TypeInfo& type, const void* object);

		// The Value of the object of `type` at `object` (composites element by element; structs list their stored fields).
		[[nodiscard]] Value ObjectToValue(const TypeInfo& type, const void* object);

		// Writes a Value that passed ValidateValue into the object of `type` at `object`. A Struct value resets the object to
		// its default first, so members the value does not list take their default values.
		void ValueToObject(const TypeInfo& type, const Value& value, void* object);

		// Reads (object not null) or validates (object null) the JSON value of `type` held by `field` or by one of its
		// containers: its spelling, `field`'s metadata when `applyMeta`, Variant values through `field`'s resolver with
		// `owner` (whose Key the walk replaces for map values), nested structs with their fields and type-level validators.
		// It writes what it read without error; callers that need atomicity read into a copy.
		void ReadJson(const TypeInfo& type, const FieldInfo& field, bool applyMeta, const JsonReader& reader, void* object,
			const ResolveContext& owner, WalkContext& walk);

		// ReadJson for a struct object: every serialized field present in `reader` (which must be an object), unknown members
		// as REFLECTION_UNKNOWN_FIELD warnings, then the type-level validators when nothing failed. With `object` null a
		// struct with C++ storage is read into a scratch object, so its validators still run.
		void ReadStructJson(const StructInfo& type, const JsonReader& reader, void* object, WalkContext& walk);

		// Validates the object of `type` at `object` held by `field` or by one of its containers.
		void ValidateObject(const TypeInfo& type, const FieldInfo& field, bool applyMeta, const void* object, const ResolveContext& owner,
			WalkContext& walk);

		// Validates every stored field of a struct object, then runs its type-level validators when the fields passed.
		void ValidateStructObject(const StructInfo& type, const void* object, WalkContext& walk);

		// Validates a Value of `type` held by `field` or by one of its containers.
		void ValidateValue(const TypeInfo& type, const FieldInfo& field, bool applyMeta, const Value& value, const ResolveContext& owner,
			WalkContext& walk);

		// Checks a Variant value held by `field`: that the canonical writer accepts it, then its resolved schema per
		// walk.Policy. `resolve` is the complete resolution context (owner and key).
		void CheckVariant(const FieldInfo& field, const JsonReader& reader, const ResolveContext& resolve, WalkContext& walk);

		// The context a value is checked with against its resolved schema `schema`: no owner object or JSON, the schema's
		// declaring struct as OwnerType, no key (FieldInfo.h, ResolveContext).
		[[nodiscard]] ResolveContext MakeResolvedSchemaContext(const FieldInfo& schema, const ResolveContext& resolve);

		// RFC 7386 MergePatch(target, patch) guided by `type`: struct members and map entries merge recursively, and a value
		// of Variant type is replaced whole even when both sides are objects (StructInfo::ApplyMergePatch).
		[[nodiscard]] Json MergePatchForType(const TypeInfo& type, const Json& target, const Json& patch);

	}

}
