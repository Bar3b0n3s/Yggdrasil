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
		TEST_CASE("Value: factories tag the kind and accessors return the stored value")
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

		TEST_CASE("Value: equality distinguishes kinds holding the same data")
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

		TEST_CASE("Value: JSON spelling follows §6 for every kind")
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

		TEST_CASE("Value: reading JSON is strict about types, arity and enum case")
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

		TEST_CASE("Value: ToString renders a short description of every kind")
		{
			CHECK(Value().ToString() == "null");
			CHECK(Value::FromBool(true).ToString() == "true");
			CHECK(Value::FromInt32(-3).ToString() == "-3");
			CHECK(Value::FromFloat(0.5f).ToString() == "0.5");
			CHECK(Value::FromVec3(glm::vec3(0.0f, 2.0f, 0.0f)).ToString() == "[0, 2, 0]");
			CHECK(Value::FromQuat(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).ToString() == "[0, 0, 0, 1]");
			CHECK(Value::FromBool3(glm::bvec3(true, false, true)).ToString() == "[true, false, true]");
			CHECK(Value::FromString("Ball").ToString() == "\"Ball\"");
			CHECK(Value::FromEntityRef(UUID()).ToString() == "null");
			CHECK(Value::FromAssetRef(UUID(0x3ff)).ToString() == "00000000000003ff");
			CHECK(Value::FromMap({ "a", "b" }, { Value::FromInt32(1), Value::FromInt32(2) }).ToString() == "Map{2}");
			CHECK(Value::FromArray({ Value::FromBool(false) }).ToString() == "Array{1}");
			CHECK(Value::FromVariant(VariantValue(Json::array({ 1, 2 }))).ToString() == "[1,2]");
		}

		TEST_CASE("Value: struct members are found by name and composite accessors are empty for scalars")
		{
			const Value inner = Value::FromStruct({ "Weight", "Label" }, { Value::FromFloat(2.0f), Value::FromString("x") });
			REQUIRE(inner.FindMember("Label") != nullptr);
			CHECK(inner.FindMember("Label")->AsString() == "x");
			CHECK(inner.FindMember("Missing") == nullptr);
			CHECK(inner.GetKeys().size() == 2);
			CHECK(inner.GetElements()[0].AsFloat() == 2.0f);

			const Value scalar = Value::FromFloat(1.0f);
			CHECK(scalar.GetElements().empty());
			CHECK(scalar.GetKeys().empty());
			CHECK(scalar.FindMember("Weight") == nullptr);
			CHECK(Value().FindMember("Weight") == nullptr);

			CHECK(Value::FromArray({ Value::FromInt32(1) }) == Value::FromArray({ Value::FromInt32(1) }));
			CHECK_FALSE(Value::FromArray({ Value::FromInt32(1) }) == Value::FromArray({ Value::FromInt32(2) }));
			CHECK_FALSE(Value::FromFloat(std::numeric_limits<float>::quiet_NaN()) == Value::FromFloat(std::numeric_limits<float>::quiet_NaN()));
			CHECK(Value() == Value());
		}

		TEST_CASE("Value: composite values convert to and from JSON with sorted map keys and struct defaults")
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();

			// A map read from JSON lists its keys byte-wise sorted, whatever the document order.
			const Result<Json> scoresJson = JsonReader::Parse(R"({ "zeta": 3, "Alpha": 1, "beta": 2 })");
			REQUIRE(scoresJson.has_value());
			const Result<Value> scores = ValueFromJson(JsonReader(*scoresJson), FindFieldType(*registry, "Scores"));
			REQUIRE(scores.has_value());
			CHECK(std::vector<std::string>(scores->GetKeys().begin(), scores->GetKeys().end()) == std::vector<std::string>{ "Alpha", "beta", "zeta" });
			const Result<Json> scoresWritten = ValueToJson(*scores, FindFieldType(*registry, "Scores"));
			REQUIRE(scoresWritten.has_value());
			CHECK(WriteMinified(*scoresWritten) == R"({"Alpha":1,"beta":2,"zeta":3})");

			// A struct read from JSON lists every stored field in field order, missing ones with their defaults.
			const Result<Json> innerJson = JsonReader::Parse(R"({ "Label": "Changed" })");
			REQUIRE(innerJson.has_value());
			const TypeInfo& innerType = FindFieldType(*registry, "Inner");
			const Result<Value> inner = ValueFromJson(JsonReader(*innerJson), innerType);
			REQUIRE(inner.has_value());
			CHECK(std::vector<std::string>(inner->GetKeys().begin(), inner->GetKeys().end()) == std::vector<std::string>{ "Weight", "Label" });
			REQUIRE(inner->FindMember("Weight") != nullptr);
			CHECK(inner->FindMember("Weight")->AsFloat() == 1.0f);
			const Result<Json> innerWritten = ValueToJson(*inner, innerType);
			REQUIRE(innerWritten.has_value());
			CHECK(WriteMinified(*innerWritten) == R"({"Weight":1,"Label":"Changed"})");

			// Unknown struct members are errors here, located at the member, with a suggestion.
			const Result<Json> typo = JsonReader::Parse(R"({ "Wieght": 2 })");
			REQUIRE(typo.has_value());
			const Result<Value> rejected = ValueFromJson(JsonReader(*typo, "/Inner"), innerType);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetLocation().JsonPointer == "/Inner/Wieght");
			CHECK(rejected.error().GetHint() == "did you mean 'Weight'?");

			const Value unknownMember = Value::FromStruct({ "Bogus" }, { Value::FromInt32(1) });
			const Result<Json> unknownWritten = ValueToJson(unknownMember, innerType);
			REQUIRE_FALSE(unknownWritten.has_value());
			CHECK(unknownWritten.error().GetLocation().JsonPointer == "/Bogus");

			// A Variant keeps its JSON verbatim, member order included.
			const Result<Json> extraJson = JsonReader::Parse(R"({ "b": [true, null], "a": "x" })");
			REQUIRE(extraJson.has_value());
			const Result<Value> extra = ValueFromJson(JsonReader(*extraJson), FindFieldType(*registry, "Extra"));
			REQUIRE(extra.has_value());
			CHECK(extra->AsVariant().Get() == *extraJson);
			const Result<Json> extraWritten = ValueToJson(*extra, FindFieldType(*registry, "Extra"));
			REQUIRE(extraWritten.has_value());
			CHECK(WriteMinified(*extraWritten) == R"({"b":[true,null],"a":"x"})");
		}

		TEST_CASE("Value: writing JSON rejects enum values without a name and values of another kind")
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();

			const Result<Json> unnamed = ValueToJson(Value::FromEnum(7), FindFieldType(*registry, "Shape"));
			REQUIRE_FALSE(unnamed.has_value());
			CHECK(unnamed.error().GetCode() == ErrorCode::Validation);

			const Result<Json> colour = ValueToJson(Value::FromVec3(glm::vec3(1.0f)), FindFieldType(*registry, "Tint"));
			CHECK_FALSE(colour.has_value()); // a Color3 field takes Color3 values only

			const Result<Json> nullValue = ValueToJson(Value(), FindFieldType(*registry, "Flag"));
			CHECK_FALSE(nullValue.has_value());

			const Value labels = Value::FromArray({ Value::FromString("a"), Value::FromInt32(2) });
			const Result<Json> mixed = ValueToJson(labels, FindFieldType(*registry, "Labels"));
			REQUIRE_FALSE(mixed.has_value());
			CHECK(mixed.error().GetLocation().JsonPointer == "/1");

			const Result<Json> mesh = ValueToJson(Value::FromAssetRef(UUID(0x102)), FindFieldType(*registry, "Mesh"));
			REQUIRE(mesh.has_value());
			CHECK(WriteMinified(*mesh) == "\"0000000000000102\"");
		}
	}

}
