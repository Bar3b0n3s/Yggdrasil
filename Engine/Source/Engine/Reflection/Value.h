#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Reflection/VariantValue.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Engine {

	class TypeInfo;

	// A reflected value detached from its C++ storage (Architecture §3, §5.4): what virtual field accessors return and
	// accept, and what script proxies (M13), inspector drawers (M10) and the reflected setters exchange with the registry.
	// It is tagged with a FieldType, so a Vec3 and a Color3 holding the same numbers are different values, and so are an
	// EntityRef and an AssetRef holding the same UUID. The null Value (default-constructed) has no type and is what a
	// failed or absent read yields.
	//
	// Storage per kind: scalars as the C++ type listed in FieldType.h; Enum as the enumerator's integer value (the name
	// table is the field's EnumInfo); Array as its elements; Map as byte-wise sorted unique keys plus one value per key;
	// Struct as the field names in field order plus one value per field; Variant as a VariantValue. A value type:
	// copyable, thread-compatible. Typed accessors assert the kind (a programmer error), so callers check GetKind() first
	// when the kind comes from outside.
	class Value
	{
	public:
		// The null value.
		Value() = default;

		[[nodiscard]] static Value FromBool(bool value);
		[[nodiscard]] static Value FromInt32(int32_t value);
		[[nodiscard]] static Value FromUInt32(uint32_t value);
		[[nodiscard]] static Value FromFloat(float value);
		[[nodiscard]] static Value FromVec2(const glm::vec2& value);
		[[nodiscard]] static Value FromVec3(const glm::vec3& value);
		[[nodiscard]] static Value FromVec4(const glm::vec4& value);
		[[nodiscard]] static Value FromQuat(const glm::quat& value);
		[[nodiscard]] static Value FromColor3(const glm::vec3& value);
		[[nodiscard]] static Value FromColor4(const glm::vec4& value);
		[[nodiscard]] static Value FromBool3(const glm::bvec3& value);
		[[nodiscard]] static Value FromString(std::string value);
		[[nodiscard]] static Value FromEntityRef(UUID value);
		[[nodiscard]] static Value FromAssetRef(UUID value);
		// `value` is the enumerator's integer value (std::to_underlying of the enumerator).
		[[nodiscard]] static Value FromEnum(int64_t value);
		[[nodiscard]] static Value FromArray(std::vector<Value> elements);
		// `keys` must be unique and byte-wise sorted, with one value per key (asserted).
		[[nodiscard]] static Value FromMap(std::vector<std::string> keys, std::vector<Value> values);
		// `fieldNames` in the struct's field order, with one value per field (asserted equal sizes).
		[[nodiscard]] static Value FromStruct(std::vector<std::string> fieldNames, std::vector<Value> values);
		[[nodiscard]] static Value FromVariant(VariantValue value);

		[[nodiscard]] bool IsNull() const { return !m_HasValue; }
		// The kind of a non-null value (asserted non-null).
		[[nodiscard]] FieldType GetKind() const;

		[[nodiscard]] bool AsBool() const;
		[[nodiscard]] int32_t AsInt32() const;
		[[nodiscard]] uint32_t AsUInt32() const;
		[[nodiscard]] float AsFloat() const;
		[[nodiscard]] glm::vec2 AsVec2() const;
		// Vec3 or Color3.
		[[nodiscard]] glm::vec3 AsVec3() const;
		// Vec4 or Color4.
		[[nodiscard]] glm::vec4 AsVec4() const;
		[[nodiscard]] glm::quat AsQuat() const;
		[[nodiscard]] glm::bvec3 AsBool3() const;
		[[nodiscard]] const std::string& AsString() const;
		// EntityRef or AssetRef.
		[[nodiscard]] UUID AsUUID() const;
		[[nodiscard]] int64_t AsEnum() const;
		[[nodiscard]] const VariantValue& AsVariant() const;

		// Array elements, Map values (in key order) or Struct field values (in field order); empty for other kinds.
		[[nodiscard]] std::span<const Value> GetElements() const { return m_Elements; }
		// Map keys or Struct field names, parallel to GetElements(); empty for other kinds.
		[[nodiscard]] std::span<const std::string> GetKeys() const { return m_Keys; }
		// The Map value or Struct field named `key`, or nullptr (also for other kinds).
		[[nodiscard]] const Value* FindMember(std::string_view key) const;

		// Equal kinds and equal contents. Floats compare with ==, so NaN is unequal to itself (the registry never stores
		// one); Variant values compare as JSON.
		[[nodiscard]] bool operator==(const Value& other) const;

		// A short human-readable rendering for logs and test messages ("[0, 2, 0]", "\"Ball\"", "Map{2}"); not a file
		// format.
		[[nodiscard]] std::string ToString() const;
	private:
		using Scalar = std::variant<std::monostate, bool, int32_t, uint32_t, float, glm::vec2, glm::vec3, glm::vec4, glm::quat,
			glm::bvec3, std::string, UUID, int64_t, VariantValue>;
	private:
		Scalar m_Scalar;
		std::vector<std::string> m_Keys;
		std::vector<Value> m_Elements;
		FieldType m_Kind = FieldType::Bool;
		bool m_HasValue = false;
	};

	// The §6 JSON spelling of `value` as a value of `type` (TypeInfo.h): numbers for Int32, UInt32 and Float; number arrays
	// for vectors, quaternions and colours; [b, b, b] for Bool3; a string for String; a 16-digit hex string, or null for
	// the invalid UUID, for EntityRef and AssetRef; the enumerator name for Enum; arrays, objects with sorted keys
	// (Map) and objects in field order (Struct); the JSON value itself for Variant. Errors: Validation when `value` is null
	// or its kind does not match `type`, for a non-finite float (located at the element's pointer, "/1" for the y of a
	// vector), and for an enum value without a name in the type's EnumInfo. Pure; thread-safe for distinct arguments.
	[[nodiscard]] Result<Json> ValueToJson(const Value& value, const TypeInfo& type);

	// Reads a value of `type` from `reader`, with the strict spelling of authored files: exact JSON types, exact-case enum
	// names, 16-digit hex UUIDs or null, arrays of exactly 2, 3 or 4 numbers for vectors and colours and 4 for quaternions,
	// finite numbers only. Struct members that `type` does not declare are errors here (structs read through StructInfo
	// only warn about them). Range metadata is not checked here (FieldInfo::ValidateValue does). Errors: Validation
	// located at the offending JSON pointer. Pure; thread-safe for distinct arguments.
	[[nodiscard]] Result<Value> ValueFromJson(const JsonReader& reader, const TypeInfo& type);

}
