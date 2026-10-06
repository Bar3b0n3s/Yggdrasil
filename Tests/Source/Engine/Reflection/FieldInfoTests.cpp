#include "TestsPCH.h"

#include "Engine/Reflection/FieldInfo.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/GlmApprox.h"
#include "Support/ReflectionTestTypes.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <map>

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
		TEST_CASE("FieldInfo: SetValue enforces finite values, Min, Max and MinMagnitude")
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

		TEST_CASE("FieldInfo: float bounds compare in float precision, so a value spelled as the bound is accepted")
		{
			const Scope<TypeRegistry> registry = CreateFieldTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);
			Test::TestAllFields object;
			const FieldContext context{ &object, nullptr };

			// 0.0001f is slightly below the double 1e-4, yet it is what a file writing the bound reads as.
			const FieldInfo* scale = type->FindField("Scale");
			REQUIRE(scale != nullptr);
			CHECK(scale->SetValue(context, Value::FromVec3(glm::vec3(0.0001f, -0.0001f, 1.0f))).has_value());
			CHECK_FALSE(scale->SetValue(context, Value::FromVec3(glm::vec3(0.0001f, 0.0000999f, 1.0f))).has_value());

			const FieldInfo* mass = type->FindField("Mass");
			REQUIRE(mass != nullptr);
			CHECK(mass->SetValue(context, Value::FromFloat(0.001f)).has_value());
			CHECK_FALSE(mass->SetValue(context, Value::FromFloat(0.000999f)).has_value());
		}

		TEST_CASE("FieldInfo: virtual fields use their getter and setter and are never serialized")
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

		TEST_CASE("FieldInfo: composite values are read and written element by element")
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

		TEST_CASE("FieldInfo: struct and map values are written whole and members a struct value omits take their defaults")
		{
			const Scope<TypeRegistry> registry = CreateFieldTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);
			Test::TestAllFields object;
			object.Inner.Weight = 5.0f;
			object.Inner.Label = "Old";
			object.Scores = { { "old", 1.0f } };
			const FieldContext context{ &object, nullptr };

			const FieldInfo* inner = type->FindField("Inner");
			REQUIRE(inner != nullptr);
			const Value current = inner->GetValue(context);
			REQUIRE(current.FindMember("Weight") != nullptr);
			CHECK(current.FindMember("Weight")->AsFloat() == 5.0f);

			REQUIRE(inner->SetValue(context, Value::FromStruct({ "Label" }, { Value::FromString("New") })).has_value());
			CHECK(object.Inner.Label == "New");
			CHECK(object.Inner.Weight == 1.0f);

			const Status negative = inner->SetValue(context, Value::FromStruct({ "Weight" }, { Value::FromFloat(-1.0f) }));
			REQUIRE_FALSE(negative.has_value());
			REQUIRE(negative.error().GetIssues().size() == 1);
			CHECK(negative.error().GetIssues()[0].JsonPointer == "/Inner/Weight");
			CHECK(object.Inner.Label == "New"); // atomic

			const Status unknown = inner->SetValue(context, Value::FromStruct({ "Wieght" }, { Value::FromFloat(1.0f) }));
			REQUIRE_FALSE(unknown.has_value());
			REQUIRE(unknown.error().GetIssues().size() == 1);
			CHECK(unknown.error().GetIssues()[0].Hint == "did you mean 'Weight'?");

			const FieldInfo* scores = type->FindField("Scores");
			REQUIRE(scores != nullptr);
			REQUIRE(scores->SetValue(context, Value::FromMap({ "a", "b" }, { Value::FromFloat(1.0f), Value::FromFloat(2.0f) })).has_value());
			CHECK(object.Scores == std::map<std::string, float>{ { "a", 1.0f }, { "b", 2.0f } });
			const Status nanScore = scores->SetValue(context, Value::FromMap({ "c" }, { Value::FromFloat(std::numeric_limits<float>::infinity()) }));
			REQUIRE_FALSE(nanScore.has_value());
			CHECK(nanScore.error().GetIssues()[0].JsonPointer == "/Scores/c");
			CHECK(object.Scores.size() == 2);
		}

		TEST_CASE("FieldInfo: ValidateValue checks kinds, enum values, unit quaternions and UTF-8 at the field's pointer")
		{
			const Scope<TypeRegistry> registry = CreateFieldTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);
			ResolveContext resolve;
			resolve.Registry = registry.get();
			resolve.OwnerType = type;

			const auto issuesOf = [&](std::string_view field, const Value& value)
			{
				const FieldInfo* info = type->FindField(field);
				REQUIRE(info != nullptr);
				ValidationContext validation("/object");
				info->ValidateValue(value, resolve, validation);
				std::vector<std::string> pointers;
				for (const ValidationIssue& issue : validation.GetIssues())
					pointers.push_back(issue.JsonPointer);
				return pointers;
			};

			CHECK(issuesOf("Tint", Value::FromColor3(glm::vec3(0.5f))).empty());
			CHECK(issuesOf("Tint", Value::FromVec3(glm::vec3(0.5f))) == std::vector<std::string>{ "/object/Tint" });
			CHECK(issuesOf("Shape", Value::FromEnum(2)).empty());
			CHECK(issuesOf("Shape", Value::FromEnum(3)) == std::vector<std::string>{ "/object/Shape" });
			CHECK(issuesOf("Rotation", Value::FromQuat(glm::quat(0.0f, 0.0f, 1.0f, 0.0f))).empty());
			CHECK(issuesOf("Rotation", Value::FromQuat(glm::quat(0.0f, 0.0f, 0.0f, 0.0f))) == std::vector<std::string>{ "/object/Rotation" });
			CHECK(issuesOf("Rotation", Value::FromQuat(glm::quat(2.0f, 0.0f, 0.0f, 0.0f))) == std::vector<std::string>{ "/object/Rotation" });
			CHECK(issuesOf("Name", Value::FromString("\xC3\xA9t\xC3\xA9")).empty());
			CHECK(issuesOf("Name", Value::FromString("\xFF")) == std::vector<std::string>{ "/object/Name" });
			CHECK(issuesOf("Size", Value::FromUInt32(101)) == std::vector<std::string>{ "/object/Size" });
			CHECK(issuesOf("Plane", Value::FromVec4(glm::vec4(0.0f, std::numeric_limits<float>::infinity(), 0.0f, std::numeric_limits<float>::quiet_NaN())))
				== std::vector<std::string>{ "/object/Plane/1", "/object/Plane/3" });
			CHECK(issuesOf("Labels", Value::FromArray({ Value::FromString("a"), Value::FromBool(true) })) == std::vector<std::string>{ "/object/Labels/1" });
			CHECK(issuesOf("Flag", Value()) == std::vector<std::string>{ "/object/Flag" });
		}

		TEST_CASE("FieldInfo: ValidateJson checks a schema-only field without a C++ object")
		{
			TypeInfo::Specification typeSpecification;
			typeSpecification.Kind = FieldType::Vec3;
			typeSpecification.Name = "Vec3";
			const TypeInfo vectorType(std::move(typeSpecification));

			FieldInfo::Specification specification;
			specification.Name = "Offset";
			specification.Description = "A script-declared offset.";
			specification.Type = &vectorType;
			specification.Meta.Min = -1.0;
			specification.Meta.Max = 1.0;
			const FieldInfo field(std::move(specification));
			CHECK_FALSE(field.IsStored());
			CHECK_FALSE(field.IsVirtual());

			const auto validate = [&](std::string_view text)
			{
				const Result<Json> json = JsonReader::Parse(text);
				REQUIRE(json.has_value());
				ValidationContext validation;
				field.ValidateJson(JsonReader(*json), ResolveContext{}, validation);
				std::vector<std::string> pointers;
				for (const ValidationIssue& issue : validation.GetIssues())
					pointers.push_back(issue.JsonPointer);
				return pointers;
			};

			CHECK(validate("[0, 0.5, -1]").empty());
			CHECK(validate("[0, 2, -1]") == std::vector<std::string>{ "/Offset/1" });
			CHECK(validate("[0, 0]") == std::vector<std::string>{ "/Offset" });
			CHECK(validate("\"x\"") == std::vector<std::string>{ "/Offset" });
			CHECK(validate("[0, \"a\", 0]") == std::vector<std::string>{ "/Offset/1" });
		}

		TEST_CASE("FieldInfo: Variant fields resolve through their resolver and free-form ones report NotFound")
		{
			const Scope<TypeRegistry> registry = CreateFieldTestRegistry();
			const StructInfo* allFields = registry->FindStruct<Test::TestAllFields>();
			const ComponentInfo* component = registry->FindComponent<Test::TestComponent>();
			REQUIRE(allFields != nullptr);
			REQUIRE(component != nullptr);
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();

			const FieldInfo* extra = allFields->FindField("Extra");
			REQUIRE(extra != nullptr);
			CHECK(extra->GetResolver() == nullptr);
			const Result<const FieldInfo*> freeForm = extra->ResolveVariant(ResolveContext{});
			REQUIRE_FALSE(freeForm.has_value());
			CHECK(freeForm.error().GetCode() == ErrorCode::NotFound);

			Test::TestComponent object;
			object.Owner = TypedAssetHandle<AssetType::Script>(Test::FixtureSchemaSource::DefaultOwner);
			const FieldInfo* overrides = component->FindField("Overrides");
			REQUIRE(overrides != nullptr);
			ResolveContext resolve;
			resolve.Owner = &object;
			resolve.OwnerType = component;
			resolve.Schemas = &schemas;
			resolve.Key = "Label";
			const Result<const FieldInfo*> label = overrides->ResolveVariant(resolve); // the registry comes from OwnerType
			REQUIRE(label.has_value());
			CHECK((*label)->GetKind() == FieldType::String);

			// The write path rejects a value that does not match its resolved schema; an unresolvable one only warns.
			const auto validate = [&](const Value& value)
			{
				ValidationContext validation;
				overrides->ValidateValue(value, resolve, validation);
				return validation.TakeIssues();
			};
			const Value good = Value::FromMap({ "Label", "Torque" }, { Value::FromVariant(VariantValue(Json("hi"))), Value::FromVariant(VariantValue(Json(30))) });
			CHECK(validate(good).empty());

			const std::vector<ValidationIssue> bad = validate(Value::FromMap({ "Torque" }, { Value::FromVariant(VariantValue(Json(500))) }));
			REQUIRE(bad.size() == 1);
			CHECK(bad[0].Severity == DiagnosticSeverity::Error);
			CHECK(bad[0].JsonPointer == "/Overrides/Torque");

			const std::vector<ValidationIssue> unknown = validate(Value::FromMap({ "Speed" }, { Value::FromVariant(VariantValue(Json(1))) }));
			REQUIRE(unknown.size() == 1);
			CHECK(unknown[0].Severity == DiagnosticSeverity::Warning);
			CHECK(unknown[0].Code == VariantUnresolvedCode);
			CHECK(unknown[0].Message.find("Speed") != std::string::npos);
		}

		TEST_CASE("FieldInfo: a struct value's Variant members resolve against the struct's JSON form")
		{
			const Scope<TypeRegistry> registry = CreateFieldTestRegistry();
			const ComponentInfo* component = registry->FindComponent<Test::TestComponent>();
			REQUIRE(component != nullptr);
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();

			// No C++ object holds this value, so the override resolver reads "Owner" from the JSON of the struct value.
			const Value whole = Value::FromStruct({ "Owner", "Overrides", "Mass", "Shape" },
				{
					Value::FromAssetRef(Test::FixtureSchemaSource::DefaultOwner),
					Value::FromMap({ "Torque" }, { Value::FromVariant(VariantValue(Json(500))) }),
					Value::FromFloat(1.0f),
					Value::FromEnum(0),
				});
			ResolveContext resolve;
			resolve.Schemas = &schemas;
			ValidationContext validation;
			component->GetSelfField().ValidateValue(whole, resolve, validation);
			REQUIRE(validation.GetErrorCount() == 1);
			CHECK(validation.GetIssues()[0].JsonPointer == "/TestComponent/Overrides/Torque");

			// The type-level rule runs on the assembled value too.
			const Value sphere = Value::FromStruct({ "Mass", "Shape" }, { Value::FromFloat(0.0f), Value::FromEnum(1) });
			ValidationContext rule;
			component->GetSelfField().ValidateValue(sphere, resolve, rule);
			REQUIRE(rule.GetErrorCount() == 1);
			CHECK(rule.GetIssues()[0].Message == "must be > 0 for spheres");
		}
	}

}
