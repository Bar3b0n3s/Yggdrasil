#include "EnginePCH.h"
#include "Engine/Reflection/Value.h"

#include "Engine/Reflection/TypeInfo.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements Value and its JSON spelling. Every factory
// returns the null Value and every accessor a zero value until then.

namespace Engine {

	Value Value::FromBool(bool /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromInt32(int32_t /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromUInt32(uint32_t /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromFloat(float /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromVec2(const glm::vec2& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromVec3(const glm::vec3& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromVec4(const glm::vec4& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromQuat(const glm::quat& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromColor3(const glm::vec3& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromColor4(const glm::vec4& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromBool3(const glm::bvec3& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromString(std::string /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromEntityRef(UUID /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromAssetRef(UUID /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromEnum(int64_t /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromArray(std::vector<Value> /*elements*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromMap(std::vector<std::string> /*keys*/, std::vector<Value> /*values*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromStruct(std::vector<std::string> /*fieldNames*/, std::vector<Value> /*values*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Value Value::FromVariant(VariantValue /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	FieldType Value::GetKind() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Kind;
	}

	bool Value::AsBool() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	int32_t Value::AsInt32() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t Value::AsUInt32() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	float Value::AsFloat() const
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	glm::vec2 Value::AsVec2() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	glm::vec3 Value::AsVec3() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	glm::vec4 Value::AsVec4() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec4(0.0f);
	}

	glm::quat Value::AsQuat() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	}

	glm::bvec3 Value::AsBool3() const
	{
		ENGINE_CONTRACT_STUB();
		return glm::bvec3(false);
	}

	const std::string& Value::AsString() const
	{
		ENGINE_CONTRACT_STUB();
		static const std::string EmptyString;
		return EmptyString;
	}

	UUID Value::AsUUID() const
	{
		ENGINE_CONTRACT_STUB();
		return UUID();
	}

	int64_t Value::AsEnum() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	const VariantValue& Value::AsVariant() const
	{
		ENGINE_CONTRACT_STUB();
		static const VariantValue NullVariant;
		return NullVariant;
	}

	const Value* Value::FindMember(std::string_view /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	bool Value::operator==(const Value& /*other*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::string Value::ToString() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<Json> ValueToJson(const Value& /*value*/, const TypeInfo& /*type*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ValueToJson is an M3 contract stub");
	}

	Result<Value> ValueFromJson(const JsonReader& /*reader*/, const TypeInfo& /*type*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ValueFromJson is an M3 contract stub");
	}

}
