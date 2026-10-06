#include "EnginePCH.h"
#include "Engine/Reflection/Value.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/Private/ReflectionWalk.h"
#include "Engine/Reflection/TypeInfo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace Engine {

	namespace Utils {

		// True for the kinds Value stores as `Stored` (Color3 shares glm::vec3 with Vec3, AssetRef shares UUID with
		// EntityRef).
		template<typename Stored>
		static bool StoresAs(FieldType kind)
		{
			if constexpr (std::is_same_v<Stored, glm::vec3>)
				return kind == FieldType::Vec3 || kind == FieldType::Color3;
			else if constexpr (std::is_same_v<Stored, glm::vec4>)
				return kind == FieldType::Vec4 || kind == FieldType::Color4;
			else if constexpr (std::is_same_v<Stored, UUID>)
				return kind == FieldType::EntityRef || kind == FieldType::AssetRef;
			else
				return false;
		}

		// Only an assert calls it, so it is unused where asserts compile out (Dist).
		[[maybe_unused]] static bool AreUniqueAndSorted(const std::vector<std::string>& keys)
		{
			for (size_t i = 1; i < keys.size(); ++i)
			{
				if (!(keys[i - 1] < keys[i]))
					return false;
			}
			return true;
		}

		static std::string FormatComponents(std::initializer_list<float> components)
		{
			std::string text = "[";
			for (const float component : components)
			{
				if (text.size() > 1)
					text += ", ";
				text += std::format("{}", component);
			}
			text += ']';
			return text;
		}

	}

	Value Value::FromBool(bool value)
	{
		Value result;
		result.m_Kind = FieldType::Bool;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromInt32(int32_t value)
	{
		Value result;
		result.m_Kind = FieldType::Int32;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromUInt32(uint32_t value)
	{
		Value result;
		result.m_Kind = FieldType::UInt32;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromFloat(float value)
	{
		Value result;
		result.m_Kind = FieldType::Float;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromVec2(const glm::vec2& value)
	{
		Value result;
		result.m_Kind = FieldType::Vec2;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromVec3(const glm::vec3& value)
	{
		Value result;
		result.m_Kind = FieldType::Vec3;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromVec4(const glm::vec4& value)
	{
		Value result;
		result.m_Kind = FieldType::Vec4;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromQuat(const glm::quat& value)
	{
		Value result;
		result.m_Kind = FieldType::Quat;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromColor3(const glm::vec3& value)
	{
		Value result;
		result.m_Kind = FieldType::Color3;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromColor4(const glm::vec4& value)
	{
		Value result;
		result.m_Kind = FieldType::Color4;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromBool3(const glm::bvec3& value)
	{
		Value result;
		result.m_Kind = FieldType::Bool3;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromString(std::string value)
	{
		Value result;
		result.m_Kind = FieldType::String;
		result.m_Scalar = std::move(value);
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromEntityRef(UUID value)
	{
		Value result;
		result.m_Kind = FieldType::EntityRef;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromAssetRef(UUID value)
	{
		Value result;
		result.m_Kind = FieldType::AssetRef;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromEnum(int64_t value)
	{
		Value result;
		result.m_Kind = FieldType::Enum;
		result.m_Scalar = value;
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromArray(std::vector<Value> elements)
	{
		Value result;
		result.m_Kind = FieldType::Array;
		result.m_Elements = std::move(elements);
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromMap(std::vector<std::string> keys, std::vector<Value> values)
	{
		ENGINE_CORE_ASSERT(keys.size() == values.size(), "A Map value needs one value per key ({} keys, {} values)", keys.size(), values.size());
		ENGINE_CORE_ASSERT(Utils::AreUniqueAndSorted(keys), "The keys of a Map value must be unique and byte-wise sorted");
		Value result;
		result.m_Kind = FieldType::Map;
		result.m_Keys = std::move(keys);
		result.m_Elements = std::move(values);
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromStruct(std::vector<std::string> fieldNames, std::vector<Value> values)
	{
		ENGINE_CORE_ASSERT(fieldNames.size() == values.size(), "A Struct value needs one value per field ({} names, {} values)", fieldNames.size(),
			values.size());
		Value result;
		result.m_Kind = FieldType::Struct;
		result.m_Keys = std::move(fieldNames);
		result.m_Elements = std::move(values);
		result.m_HasValue = true;
		return result;
	}

	Value Value::FromVariant(VariantValue value)
	{
		Value result;
		result.m_Kind = FieldType::Variant;
		result.m_Scalar = std::move(value);
		result.m_HasValue = true;
		return result;
	}

	FieldType Value::GetKind() const
	{
		ENGINE_CORE_ASSERT(m_HasValue, "The null Value has no kind");
		return m_Kind;
	}

	bool Value::AsBool() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::Bool, "Value is not a Bool");
		const bool* value = std::get_if<bool>(&m_Scalar);
		return value != nullptr && *value;
	}

	int32_t Value::AsInt32() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::Int32, "Value is not an Int32");
		const int32_t* value = std::get_if<int32_t>(&m_Scalar);
		return value != nullptr ? *value : 0;
	}

	uint32_t Value::AsUInt32() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::UInt32, "Value is not a UInt32");
		const uint32_t* value = std::get_if<uint32_t>(&m_Scalar);
		return value != nullptr ? *value : 0u;
	}

	float Value::AsFloat() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::Float, "Value is not a Float");
		const float* value = std::get_if<float>(&m_Scalar);
		return value != nullptr ? *value : 0.0f;
	}

	glm::vec2 Value::AsVec2() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::Vec2, "Value is not a Vec2");
		const glm::vec2* value = std::get_if<glm::vec2>(&m_Scalar);
		return value != nullptr ? *value : glm::vec2(0.0f);
	}

	glm::vec3 Value::AsVec3() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && Utils::StoresAs<glm::vec3>(m_Kind), "Value is not a Vec3 or Color3");
		const glm::vec3* value = std::get_if<glm::vec3>(&m_Scalar);
		return value != nullptr ? *value : glm::vec3(0.0f);
	}

	glm::vec4 Value::AsVec4() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && Utils::StoresAs<glm::vec4>(m_Kind), "Value is not a Vec4 or Color4");
		const glm::vec4* value = std::get_if<glm::vec4>(&m_Scalar);
		return value != nullptr ? *value : glm::vec4(0.0f);
	}

	glm::quat Value::AsQuat() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::Quat, "Value is not a Quat");
		const glm::quat* value = std::get_if<glm::quat>(&m_Scalar);
		return value != nullptr ? *value : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	}

	glm::bvec3 Value::AsBool3() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::Bool3, "Value is not a Bool3");
		const glm::bvec3* value = std::get_if<glm::bvec3>(&m_Scalar);
		return value != nullptr ? *value : glm::bvec3(false);
	}

	const std::string& Value::AsString() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::String, "Value is not a String");
		static const std::string EmptyString;
		const std::string* value = std::get_if<std::string>(&m_Scalar);
		return value != nullptr ? *value : EmptyString;
	}

	UUID Value::AsUUID() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && Utils::StoresAs<UUID>(m_Kind), "Value is not an EntityRef or AssetRef");
		const UUID* value = std::get_if<UUID>(&m_Scalar);
		return value != nullptr ? *value : UUID();
	}

	int64_t Value::AsEnum() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::Enum, "Value is not an Enum");
		const int64_t* value = std::get_if<int64_t>(&m_Scalar);
		return value != nullptr ? *value : 0;
	}

	const VariantValue& Value::AsVariant() const
	{
		ENGINE_CORE_ASSERT(m_HasValue && m_Kind == FieldType::Variant, "Value is not a Variant");
		static const VariantValue NullVariant;
		const VariantValue* value = std::get_if<VariantValue>(&m_Scalar);
		return value != nullptr ? *value : NullVariant;
	}

	const Value* Value::FindMember(std::string_view key) const
	{
		if (!m_HasValue)
			return nullptr;
		if (m_Kind == FieldType::Map)
		{
			const auto found = std::lower_bound(m_Keys.begin(), m_Keys.end(), key, [](const std::string& entry, std::string_view wanted)
			{
				return std::string_view(entry) < wanted;
			});
			if (found == m_Keys.end() || *found != key)
				return nullptr;
			return &m_Elements[static_cast<size_t>(found - m_Keys.begin())];
		}
		if (m_Kind == FieldType::Struct)
		{
			const auto found = std::find(m_Keys.begin(), m_Keys.end(), key);
			if (found == m_Keys.end())
				return nullptr;
			return &m_Elements[static_cast<size_t>(found - m_Keys.begin())];
		}
		return nullptr;
	}

	bool Value::operator==(const Value& other) const
	{
		if (m_HasValue != other.m_HasValue)
			return false;
		if (!m_HasValue)
			return true;
		return m_Kind == other.m_Kind && m_Scalar == other.m_Scalar && m_Keys == other.m_Keys && m_Elements == other.m_Elements;
	}

	std::string Value::ToString() const
	{
		if (!m_HasValue)
			return "null";

		switch (m_Kind)
		{
			case FieldType::Bool:
				return AsBool() ? "true" : "false";
			case FieldType::Int32:
				return std::to_string(AsInt32());
			case FieldType::UInt32:
				return std::to_string(AsUInt32());
			case FieldType::Float:
				return std::format("{}", AsFloat());
			case FieldType::Vec2:
			{
				const glm::vec2 vector = AsVec2();
				return Utils::FormatComponents({ vector.x, vector.y });
			}
			case FieldType::Vec3:
			case FieldType::Color3:
			{
				const glm::vec3 vector = AsVec3();
				return Utils::FormatComponents({ vector.x, vector.y, vector.z });
			}
			case FieldType::Vec4:
			case FieldType::Color4:
			{
				const glm::vec4 vector = AsVec4();
				return Utils::FormatComponents({ vector.x, vector.y, vector.z, vector.w });
			}
			case FieldType::Quat:
			{
				const glm::quat rotation = AsQuat();
				return Utils::FormatComponents({ rotation.x, rotation.y, rotation.z, rotation.w });
			}
			case FieldType::Bool3:
			{
				const glm::bvec3 flags = AsBool3();
				return std::format("[{}, {}, {}]", flags.x, flags.y, flags.z);
			}
			case FieldType::String:
				return std::format("\"{}\"", AsString());
			case FieldType::EntityRef:
			case FieldType::AssetRef:
				return AsUUID().IsValid() ? AsUUID().ToString() : std::string("null");
			case FieldType::Enum:
				return std::format("Enum({})", AsEnum());
			case FieldType::Array:
				return std::format("Array{{{}}}", m_Elements.size());
			case FieldType::Struct:
				return std::format("Struct{{{}}}", m_Elements.size());
			case FieldType::Map:
				return std::format("Map{{{}}}", m_Elements.size());
			case FieldType::Variant:
			{
				const Result<std::string> text = JsonWriter::Write(AsVariant().Get(), JsonStyle::Minified);
				return text.has_value() ? *text : std::string("<unwritable JSON>");
			}
		}
		return "Unknown";
	}

	Result<Json> ValueToJson(const Value& value, const TypeInfo& type)
	{
		return Utils::ConvertValueToJson(value, type);
	}

	Result<Value> ValueFromJson(const JsonReader& reader, const TypeInfo& type)
	{
		return Utils::ConvertJsonToValue(reader, type);
	}

}
