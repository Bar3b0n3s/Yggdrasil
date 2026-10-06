#include "TestsPCH.h"

#include "Engine/Reflection/Value.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/GlmApprox.h"
#include "Support/ReflectionTestTypes.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace Engine {

	static const TypeInfo& FindFieldType(const TypeRegistry& registry, std::string_view field)
	{
		const StructInfo* type = registry.FindStruct<Test::TestAllFields>();
		REQUIRE(type != nullptr);
		const FieldInfo* info = type->FindField(field);
		REQUIRE(info != nullptr);
		return info->GetType();
	}

	static std::string WriteMinified(const Json& json)
	{
		const Result<std::string> text = JsonWriter::Write(json, JsonStyle::Minified);
		REQUIRE(text.has_value());
		return *text;
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("Value: factories tag the kind and accessors return the stored value" * doctest::skip(true))
		{
			CHECK(Value().IsNull());
			CHECK(Value::FromBool(true).AsBool());
			CHECK(Value::FromInt32(-7).AsInt32() == -7);
			CHECK(Value::FromUInt32(7).AsUInt32() == 7u);
			CHECK(Value::FromFloat(0.5f).AsFloat() == 0.5f);
			CHECK(Value::FromVec3(glm::vec3(1.0f, 2.0f, 3.0f)).GetKind() == FieldType::Vec3);
			CHECK(Value::FromColor3(glm::vec3(1.0f, 2.0f, 3.0f)).GetKind() == FieldType::Color3);
			CHECK(Value::FromColor3(glm::vec3(1.0f, 2.0f, 3.0f)).AsVec3() == glm::vec3(1.0f, 2.0f, 3.0f));
			CHECK(Value::FromEntityRef(UUID(5)).GetKind() == FieldType::EntityRef);
			CHECK(Value::FromAssetRef(UUID(5)).GetKind() == FieldType::AssetRef);
			CHECK(Value::FromAssetRef(UUID(5)).AsUUID() == UUID(5));
			CHECK(Value::FromString("Ball").AsString() == "Ball");
			CHECK(Value::FromEnum(2).AsEnum() == 2);
		}

		TEST_CASE("Value: equality distinguishes kinds holding the same data" * doctest::skip(true))
		{
			CHECK(Value::FromVec3(glm::vec3(1.0f)) == Value::FromVec3(glm::vec3(1.0f)));
			CHECK_FALSE(Value::FromVec3(glm::vec3(1.0f)) == Value::FromColor3(glm::vec3(1.0f)));
			CHECK_FALSE(Value::FromEntityRef(UUID(1)) == Value::FromAssetRef(UUID(1)));
			CHECK_FALSE(Value() == Value::FromBool(false));

			const Value map = Value::FromMap({ "a", "b" }, { Value::FromFloat(1.0f), Value::FromFloat(2.0f) });
			REQUIRE(map.FindMember("b") != nullptr);
			CHECK(map.FindMember("b")->AsFloat() == 2.0f);
			CHECK(map.FindMember("c") == nullptr);
			CHECK(map.GetKeys().size() == 2);
		}

		TEST_CASE("Value: JSON spelling follows §6 for every kind" * doctest::skip(true))
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();

			const Result<Json> rotation = ValueToJson(Value::FromQuat(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), FindFieldType(*registry, "Rotation"));
			REQUIRE(rotation.has_value());
			CHECK(WriteMinified(*rotation) == "[0,0,0,1]");

			const Result<Json> target = ValueToJson(Value::FromEntityRef(UUID()), FindFieldType(*registry, "Target"));
			REQUIRE(target.has_value());
			CHECK(WriteMinified(*target) == "null");

			const Result<Json> shape = ValueToJson(Value::FromEnum(1), FindFieldType(*registry, "Shape"));
			REQUIRE(shape.has_value());
			CHECK(WriteMinified(*shape) == "\"Sphere\"");

			const Result<Json> locks = ValueToJson(Value::FromBool3(glm::bvec3(true, false, true)), FindFieldType(*registry, "Locks"));
			REQUIRE(locks.has_value());
			CHECK(WriteMinified(*locks) == "[true,false,true]");

			const Result<Json> nan = ValueToJson(Value::FromFloat(std::numeric_limits<float>::quiet_NaN()), FindFieldType(*registry, "Mass"));
			CHECK_FALSE(nan.has_value());
			const Result<Json> wrongKind = ValueToJson(Value::FromString("x"), FindFieldType(*registry, "Mass"));
			CHECK_FALSE(wrongKind.has_value());
		}

		TEST_CASE("Value: reading JSON is strict about types, arity and enum case" * doctest::skip(true))
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();

			const auto read = [&](std::string_view field, std::string_view text)
			{
				const Result<Json> json = JsonReader::Parse(text);
				REQUIRE(json.has_value());
				return ValueFromJson(JsonReader(*json), FindFieldType(*registry, field));
			};

			const Result<Value> position = read("Position", "[1, 2.5, -3]");
			REQUIRE(position.has_value());
			CHECK(position->AsVec3() == glm::vec3(1.0f, 2.5f, -3.0f));
			CHECK_FALSE(read("Position", "[1, 2]").has_value());
			CHECK_FALSE(read("Position", "\"1,2,3\"").has_value());
			CHECK_FALSE(read("Shape", "\"sphere\"").has_value());
			CHECK_FALSE(read("Count", "1.5").has_value());
			CHECK_FALSE(read("Target", "\"5d1c9a\"").has_value());

			const Result<Value> empty = read("Target", "null");
			REQUIRE(empty.has_value());
			CHECK_FALSE(empty->AsUUID().IsValid());

			const Result<Value> wrong = read("Position", "[1, true, 3]");
			REQUIRE_FALSE(wrong.has_value());
			CHECK(wrong.error().GetLocation().JsonPointer == "/1");
		}
	}

}
