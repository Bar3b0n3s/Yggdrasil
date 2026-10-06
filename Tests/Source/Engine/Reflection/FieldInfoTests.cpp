#include "TestsPCH.h"

#include "Engine/Reflection/FieldInfo.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Support/GlmApprox.h"
#include "Support/ReflectionTestTypes.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace Engine {

	namespace {

		struct VirtualHolder
		{
			float Stored = 2.0f;
		};

	}

	static Value GetDoubled(const FieldContext& context)
	{
		return Value::FromFloat(static_cast<const VirtualHolder*>(context.Object)->Stored * 2.0f);
	}

	static Status SetDoubled(const FieldContext& context, const Value& value)
	{
		static_cast<VirtualHolder*>(context.Object)->Stored = value.AsFloat() / 2.0f;
		return {};
	}

	static Scope<TypeRegistry> CreateFieldTestRegistry()
	{
		Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
		Test::RegisterReflectionTestTypes(*registry);
		registry->Struct<VirtualHolder>("VirtualHolder", "A struct with virtual fields.")
			.Field("Stored", &VirtualHolder::Stored, "A stored float.", { .Min = 0.0 })
			.VirtualField<float>("Doubled", "Twice the stored value.", &GetDoubled, &SetDoubled, { .Min = 0.0 })
			.VirtualField<float>("DoubledReadOnly", "Twice the stored value, read-only.", &GetDoubled, nullptr);
		registry->Freeze();
		return registry;
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("FieldInfo: SetValue enforces finite values, Min, Max and MinMagnitude" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = CreateFieldTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);
			Test::TestAllFields object;
			const FieldContext context{ &object, nullptr };

			const FieldInfo* count = type->FindField("Count");
			REQUIRE(count != nullptr);
			CHECK(count->SetValue(context, Value::FromInt32(10)).has_value());
			CHECK_FALSE(count->SetValue(context, Value::FromInt32(11)).has_value());
			CHECK(object.Count == 10);

			const FieldInfo* mass = type->FindField("Mass");
			REQUIRE(mass != nullptr);
			CHECK_FALSE(mass->SetValue(context, Value::FromFloat(0.0f)).has_value());
			CHECK_FALSE(mass->SetValue(context, Value::FromFloat(std::numeric_limits<float>::infinity())).has_value());
			CHECK_FALSE(mass->SetValue(context, Value::FromFloat(std::numeric_limits<float>::quiet_NaN())).has_value());
			CHECK(object.Mass == 1.0f);

			const FieldInfo* scale = type->FindField("Scale");
			REQUIRE(scale != nullptr);
			CHECK_FALSE(scale->SetValue(context, Value::FromVec3(glm::vec3(1.0f, 0.00001f, 1.0f))).has_value());
			CHECK(scale->SetValue(context, Value::FromVec3(glm::vec3(-2.0f, 1.0f, 1.0f))).has_value());
			CHECK(object.Scale == glm::vec3(-2.0f, 1.0f, 1.0f));

			const Status wrongKind = mass->SetValue(context, Value::FromString("heavy"));
			REQUIRE_FALSE(wrongKind.has_value());
			CHECK(wrongKind.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("FieldInfo: virtual fields use their getter and setter and are never serialized" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = CreateFieldTestRegistry();
			const StructInfo* type = registry->FindStruct<VirtualHolder>();
			REQUIRE(type != nullptr);
			VirtualHolder object;
			const FieldContext context{ &object, nullptr };

			const FieldInfo* doubled = type->FindField("Doubled");
			REQUIRE(doubled != nullptr);
			CHECK(doubled->IsVirtual());
			CHECK_FALSE(doubled->IsStored());
			CHECK_FALSE(doubled->GetMeta().Serialized);
			CHECK(doubled->GetValue(context).AsFloat() == 4.0f);
			REQUIRE(doubled->SetValue(context, Value::FromFloat(10.0f)).has_value());
			CHECK(object.Stored == 5.0f);
			CHECK_FALSE(doubled->SetValue(context, Value::FromFloat(-1.0f)).has_value()); // Min applies to virtual fields too

			const FieldInfo* readOnly = type->FindField("DoubledReadOnly");
			REQUIRE(readOnly != nullptr);
			CHECK(readOnly->IsReadOnly());
			const Status denied = readOnly->SetValue(context, Value::FromFloat(1.0f));
			REQUIRE_FALSE(denied.has_value());
			CHECK(denied.error().GetCode() == ErrorCode::InvalidState);

			const Result<Json> json = type->ToJson(&object);
			REQUIRE(json.has_value());
			CHECK(json->size() == 1);
		}

		TEST_CASE("FieldInfo: composite values are read and written element by element" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = CreateFieldTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);
			Test::TestAllFields object;
			object.Labels = { "a", "b" };
			const FieldContext context{ &object, nullptr };

			const FieldInfo* labels = type->FindField("Labels");
			REQUIRE(labels != nullptr);
			const Value value = labels->GetValue(context);
			CHECK(value.GetKind() == FieldType::Array);
			REQUIRE(value.GetElements().size() == 2);
			CHECK(value.GetElements()[1].AsString() == "b");

			const Value replacement = Value::FromArray({ Value::FromString("x") });
			REQUIRE(labels->SetValue(context, replacement).has_value());
			CHECK(object.Labels == std::vector<std::string>{ "x" });
		}
	}

}
