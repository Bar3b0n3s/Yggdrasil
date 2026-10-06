#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Support/DeathTest.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/ReflectionTestTypes.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Scope<TypeRegistry> CreateTestRegistry()
	{
		Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
		Test::RegisterReflectionTestTypes(*registry);
		registry->Freeze();
		return registry;
	}

	static Json ParseJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	namespace {

		struct RuleWithoutHook
		{
			float Value = 1.0f;
		};

		struct OtherInner
		{
			float Value = 1.0f;
		};

		struct LateStruct
		{
			float Value = 1.0f;
		};

		struct VersionTwo
		{
			float Value = 1.0f;
		};

		struct NestedVariant
		{
			VariantValue Child;
		};

	}

	// Resolves NestedVariant.Child to the whole NestedVariant struct, so each value can hold another resolution.
	static Result<const FieldInfo*> ResolveNestedVariant(const ResolveContext& context)
	{
		const StructInfo* type = context.Registry != nullptr ? context.Registry->FindStruct<NestedVariant>() : nullptr;
		if (type == nullptr)
			return MakeError(ErrorCode::NotFound, "NestedVariant is not registered");
		return &type->GetSelfField();
	}

	// `levels` NestedVariant objects inside each other, the innermost without a Child.
	static Json MakeNestedVariantJson(int levels)
	{
		Json value = Json::object();
		for (int level = 0; level < levels; ++level)
		{
			Json outer = Json::object();
			outer["Child"] = std::move(value);
			value = std::move(outer);
		}
		return value;
	}

	static void ValidateRuleWithoutHook(const RuleWithoutHook& object, ValidationContext& context)
	{
		if (object.Value > 10.0f)
			context.Error("Value", "must be <= 10");
	}

	ENGINE_DEATH_TEST("Reflection/FreezeRequiresGenerateHook")
	{
		TypeRegistry registry;
		registry.Struct<RuleWithoutHook>("RuleWithoutHook", "A struct whose rule has no Generate hook.")
			.Field("Value", &RuleWithoutHook::Value, "A value.")
			.Validate(&ValidateRuleWithoutHook);
		registry.Freeze();
	}

	ENGINE_DEATH_TEST("Reflection/DuplicateTypeName")
	{
		TypeRegistry registry;
		registry.Struct<Test::TestInner>("Inner", "The first struct named Inner.").Field("Weight", &Test::TestInner::Weight, "A weight.");
		registry.Struct<OtherInner>("Inner", "The second struct named Inner.").Field("Value", &OtherInner::Value, "A value.");
	}

	ENGINE_DEATH_TEST("Reflection/RegistrationAfterFreeze")
	{
		TypeRegistry registry;
		Test::RegisterReflectionTestTypes(registry);
		registry.Freeze();
		registry.Struct<LateStruct>("LateStruct", "Registered too late.").Field("Value", &LateStruct::Value, "A value.");
	}

	ENGINE_DEATH_TEST("Reflection/MissingMigration")
	{
		TypeRegistry registry;
		registry.Component<VersionTwo>("VersionTwo", "Version 2 without a migration from version 1.")
			.Category("Test")
			.Version(2)
			.Field("Value", &VersionTwo::Value, "A value.");
		registry.Freeze();
	}

	ENGINE_DEATH_TEST("Reflection/FieldWithoutDescription")
	{
		TypeRegistry registry;
		registry.Struct<OtherInner>("OtherInner", "A struct with an undocumented field.").Field("Value", &OtherInner::Value, "");
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("TypeRegistry: registered types are found by name and by C++ type")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			CHECK(registry->IsFrozen());

			const StructInfo* allFields = registry->FindStruct("TestAllFields");
			REQUIRE(allFields != nullptr);
			CHECK(registry->FindStruct<Test::TestAllFields>() == allFields);
			CHECK(allFields->GetDescription() == "One field of every FieldType.");
			CHECK(allFields->GetFields().size() == 20);

			const ComponentInfo* component = registry->FindComponent("TestComponent");
			REQUIRE(component != nullptr);
			CHECK(registry->FindComponent<Test::TestComponent>() == component);
			CHECK(component->GetCategory() == "Test");
			CHECK(component->GetIndex() == 0);
			CHECK(registry->GetComponents().size() == 1);

			const EnumInfo* shape = registry->FindEnum("TestShape");
			REQUIRE(shape != nullptr);
			CHECK(registry->FindEnum<Test::TestShape>() == shape);

			CHECK(registry->FindComponent("testcomponent") == nullptr); // names are case-sensitive (§6)
			CHECK(registry->FindStruct("Missing") == nullptr);
		}

		TEST_CASE("TypeRegistry: fields deduce their FieldType and keep registration order")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			const std::vector<std::pair<std::string_view, FieldType>> expected = {
				{ "Flag", FieldType::Bool },
				{ "Count", FieldType::Int32 },
				{ "Size", FieldType::UInt32 },
				{ "Mass", FieldType::Float },
				{ "Offset", FieldType::Vec2 },
				{ "Position", FieldType::Vec3 },
				{ "Plane", FieldType::Vec4 },
				{ "Rotation", FieldType::Quat },
				{ "Tint", FieldType::Color3 },
				{ "Glow", FieldType::Color4 },
				{ "Locks", FieldType::Bool3 },
				{ "Name", FieldType::String },
				{ "Target", FieldType::EntityRef },
				{ "Mesh", FieldType::AssetRef },
				{ "Shape", FieldType::Enum },
				{ "Labels", FieldType::Array },
				{ "Inner", FieldType::Struct },
				{ "Scores", FieldType::Map },
				{ "Extra", FieldType::Variant },
				{ "Scale", FieldType::Vec3 },
			};
			REQUIRE(type->GetFields().size() == expected.size());
			for (size_t i = 0; i < expected.size(); ++i)
			{
				const FieldInfo& field = *type->GetFields()[i];
				CHECK(field.GetName() == expected[i].first);
				CHECK(field.GetKind() == expected[i].second);
				CHECK_FALSE(field.GetDescription().empty());
				CHECK(field.IsStored());
			}

			const FieldInfo* mesh = type->FindField("Mesh");
			REQUIRE(mesh != nullptr);
			CHECK(mesh->GetMeta().AssetFilter == "Mesh");
			CHECK(mesh->GetType().GetAssetTypeName() == "Mesh");

			const FieldInfo* labels = type->FindField("Labels");
			REQUIRE(labels != nullptr);
			REQUIRE(labels->GetType().GetElement() != nullptr);
			CHECK(labels->GetType().GetElement()->GetKind() == FieldType::String);

			const FieldInfo* inner = type->FindField("Inner");
			REQUIRE(inner != nullptr);
			CHECK(inner->GetType().GetStruct() == registry->FindStruct<Test::TestInner>());
		}

		TEST_CASE("TypeRegistry: unknown component names get fuzzy suggestions")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			const std::vector<std::string> suggestions = registry->SuggestComponentNames("TestComponnt");
			REQUIRE_FALSE(suggestions.empty());
			CHECK(suggestions.front() == "TestComponent");
			CHECK(registry->SuggestComponentNames("Zzzzzzzzzzzzzzzz").empty());
		}

		TEST_CASE("TypeRegistry: AreComponentsRegistered checks a whole type list")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			CHECK(registry->AreComponentsRegistered(TypeList<Test::TestComponent>{}));
			CHECK_FALSE(registry->AreComponentsRegistered(TypeList<Test::TestComponent, Test::TestInner>{}));
		}

		TEST_CASE("Map: keys serialize sorted and merge-patch deletes on null")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			Test::TestAllFields object;
			object.Scores = { { "zeta", 3.0f }, { "Alpha", 1.0f }, { "beta", 2.0f } };
			const Result<Json> json = type->ToJson(&object);
			REQUIRE(json.has_value());
			const Result<std::string> scores = JsonWriter::Write((*json)["Scores"], JsonStyle::Minified);
			REQUIRE(scores.has_value());
			CHECK(*scores == R"({"Alpha":1,"beta":2,"zeta":3})"); // byte-wise: uppercase before lowercase

			const Json patch = ParseJson(R"({ "Scores": { "beta": null, "gamma": 4 }, "Count": 7 })");
			REQUIRE(type->ApplyMergePatch(&object, JsonReader(patch), ReadContext{}).has_value());
			CHECK(object.Scores.size() == 3);
			CHECK_FALSE(object.Scores.contains("beta"));
			CHECK(object.Scores.at("gamma") == doctest::Approx(4.0f));
			CHECK(object.Scores.at("zeta") == doctest::Approx(3.0f));
			CHECK(object.Count == 7);

			// A patch that fails validation changes nothing.
			const Json invalid = ParseJson(R"({ "Scores": { "zeta": null }, "Count": 99 })");
			CHECK_FALSE(type->ApplyMergePatch(&object, JsonReader(invalid), ReadContext{}).has_value());
			CHECK(object.Scores.contains("zeta"));
			CHECK(object.Count == 7);
		}

		TEST_CASE("Variant: value validated against the resolved schema; unresolvable values are preserved with a diagnostic")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			const ComponentInfo* type = registry->FindComponent<Test::TestComponent>();
			REQUIRE(type != nullptr);
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();

			const Json document = ParseJson(R"({
				"Owner": "00000000c0ffee01",
				"Overrides": { "Torque": 30, "Unknown": [1, 2], "Label": 5 },
				"Mass": 2,
				"Shape": "Sphere"
			})");

			std::vector<ValidationIssue> diagnostics;
			ReadContext context;
			context.Schemas = &schemas;
			context.Diagnostics = &diagnostics;

			Test::TestComponent object;
			REQUIRE(type->FromJson(&object, JsonReader(document), context).has_value());

			// Every value is kept verbatim, resolved or not.
			REQUIRE(object.Overrides.size() == 3);
			CHECK(object.Overrides.at("Torque").Get() == document["Overrides"]["Torque"]);
			CHECK(object.Overrides.at("Unknown").Get() == document["Overrides"]["Unknown"]);
			CHECK(object.Overrides.at("Label").Get() == document["Overrides"]["Label"]);

			// "Unknown" cannot be resolved, "Label" resolves to a String and holds a number.
			const auto hasDiagnostic = [&](std::string_view code, std::string_view pointer)
			{
				return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const ValidationIssue& issue)
				{
					return issue.Code == code && issue.JsonPointer == pointer && issue.Severity == DiagnosticSeverity::Warning;
				});
			};
			CHECK(hasDiagnostic(VariantUnresolvedCode, "/Overrides/Unknown"));
			CHECK(hasDiagnostic(VariantSchemaMismatchCode, "/Overrides/Label"));
			CHECK_FALSE(hasDiagnostic(VariantUnresolvedCode, "/Overrides/Torque"));

			// A resolved value outside its range is rejected on the write path.
			const FieldInfo* overrides = type->FindField("Overrides");
			REQUIRE(overrides != nullptr);
			ResolveContext resolve;
			resolve.Registry = registry.get();
			resolve.Owner = &object;
			resolve.OwnerType = type;
			resolve.Key = "Torque";
			resolve.Schemas = &schemas;
			const Result<const FieldInfo*> torque = overrides->ResolveVariant(resolve);
			REQUIRE(torque.has_value());
			ValidationContext validation;
			const Json tooLarge = ParseJson("500");
			(*torque)->ValidateJson(JsonReader(tooLarge), resolve, validation);
			CHECK(validation.HasErrors());

			// Without a C++ object the resolver reads the owner from the component's JSON; an owner of another type is
			// rejected instead of being misread.
			const JsonReader ownerJson(document);
			ResolveContext jsonOnly = resolve;
			jsonOnly.Owner = nullptr;
			jsonOnly.OwnerJson = &ownerJson;
			const Result<const FieldInfo*> fromJson = overrides->ResolveVariant(jsonOnly);
			REQUIRE(fromJson.has_value());
			CHECK(*fromJson == *torque);
			ResolveContext wrongOwner = resolve;
			wrongOwner.OwnerType = registry->FindStruct<Test::TestInner>();
			const Result<const FieldInfo*> misread = overrides->ResolveVariant(wrongOwner);
			REQUIRE_FALSE(misread.has_value());
			CHECK(misread.error().GetCode() == ErrorCode::InvalidArgument);

			// --strict turns the preserved values into errors.
			ReadContext strict = context;
			strict.Strict = true;
			Test::TestComponent strictObject;
			CHECK_FALSE(type->FromJson(&strictObject, JsonReader(document), strict).has_value());
		}

		TEST_CASE("Variant: values nested too deeply inside each other are reported instead of exhausting the stack")
		{
			TypeRegistry registry;
			registry.Struct<NestedVariant>("NestedVariant", "A Variant field that resolves to its own struct.")
				.VariantField("Child", &NestedVariant::Child, "Another NestedVariant, or null.", &ResolveNestedVariant);
			registry.Freeze();
			const StructInfo* type = registry.FindStruct<NestedVariant>();
			REQUIRE(type != nullptr);
			std::vector<ValidationIssue> diagnostics;
			ReadContext context;
			context.Diagnostics = &diagnostics;

			// A few levels resolve like any other value.
			NestedVariant shallow;
			REQUIRE(type->FromJson(&shallow, JsonReader(MakeNestedVariantJson(4)), context).has_value());
			CHECK(diagnostics.empty());

			// Far deeper nesting is kept as written with a warning on reads...
			const Json deep = MakeNestedVariantJson(64);
			NestedVariant kept;
			REQUIRE(type->FromJson(&kept, JsonReader(deep), context).has_value());
			REQUIRE(diagnostics.size() == 1);
			CHECK(diagnostics[0].Code == VariantSchemaMismatchCode);
			CHECK(diagnostics[0].JsonPointer.starts_with("/Child/Child"));
			CHECK(kept.Child.Get() == deep["Child"]);

			// ...and rejected on the write path.
			const FieldInfo* child = type->FindField("Child");
			REQUIRE(child != nullptr);
			ResolveContext resolve;
			resolve.Registry = &registry;
			resolve.OwnerType = type;
			ValidationContext validation;
			child->ValidateJson(JsonReader(deep["Child"]), resolve, validation);
			CHECK(validation.HasErrors());
		}

		TEST_CASE("TypeRegistry: type-level validators run after field checks")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			const ComponentInfo* type = registry->FindComponent<Test::TestComponent>();
			REQUIRE(type != nullptr);

			Test::TestComponent object;
			object.Shape = Test::TestShape::Sphere;
			object.Mass = 0.0f;

			ResolveContext resolve;
			resolve.Registry = registry.get();
			ValidationContext context("/components/TestComponent");
			type->Validate(&object, resolve, context);
			REQUIRE(context.GetErrorCount() == 1);
			CHECK(context.GetIssues()[0].JsonPointer == "/components/TestComponent/Mass");
			CHECK(context.GetIssues()[0].Message == "must be > 0 for spheres");
		}

		TEST_CASE("TypeRegistry: lists keep registration order and every type has a TypeInfo under its key")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			REQUIRE(registry->GetStructs().size() == 2);
			CHECK(registry->GetStructs()[0]->GetName() == "TestInner");
			CHECK(registry->GetStructs()[1]->GetName() == "TestAllFields");
			REQUIRE(registry->GetEnums().size() == 1);
			CHECK(registry->GetEnums()[0]->GetName() == "TestShape");

			// Components are not listed among the structs, and each kind is found only by its own lookups.
			CHECK(registry->FindStruct("TestComponent") == nullptr);
			CHECK(registry->FindComponent("TestInner") == nullptr);
			CHECK(registry->FindStructByKey(TypeKeyOf<Test::TestInner>()) == registry->FindStruct<Test::TestInner>());
			CHECK(registry->FindEnumByKey(TypeKeyOf<Test::TestShape>()) == registry->FindEnum("TestShape"));
			CHECK(registry->FindComponentByKey(TypeKeyOf<Test::TestInner>()) == nullptr);

			const TypeInfo* shape = registry->FindType(TypeKeyOf<Test::TestShape>());
			REQUIRE(shape != nullptr);
			CHECK(shape->GetKind() == FieldType::Enum);
			CHECK(shape->GetEnum() == registry->FindEnum("TestShape"));
			CHECK(shape->GetName() == "TestShape");

			const TypeInfo* number = registry->FindType(TypeKeyOf<float>());
			REQUIRE(number != nullptr);
			CHECK(number->GetKind() == FieldType::Float);
			CHECK(number->HasOps());

			// Fields of one C++ type share its TypeInfo; a colour has its own.
			const StructInfo* allFields = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(allFields != nullptr);
			CHECK(&allFields->FindField("Position")->GetType() == &allFields->FindField("Scale")->GetType());
			CHECK(&allFields->FindField("Position")->GetType() != &allFields->FindField("Tint")->GetType());
			const TypeInfo* colour = registry->FindType(TypeKeyOf<Detail::ColorOf<glm::vec3>>());
			REQUIRE(colour != nullptr);
			CHECK(colour->GetKind() == FieldType::Color3);
			CHECK(&allFields->FindField("Tint")->GetType() == colour);

			const ComponentInfo* component = registry->FindComponent<Test::TestComponent>();
			REQUIRE(component != nullptr);
			CHECK(registry->FindType(TypeKeyOf<Test::TestComponent>()) == &component->GetType());
			CHECK(component->GetType().GetStruct() == component);
		}

		TEST_CASE("TypeRegistry: Freeze is idempotent")
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			registry->Freeze();
			CHECK(registry->IsFrozen());
			CHECK(registry->GetComponents().size() == 1);
		}

		TEST_CASE("TypeRegistry: registration mistakes are programmer errors that assert")
		{
			ENGINE_CHECK_DEATH("Reflection/FreezeRequiresGenerateHook", "has a Validate rule but no Generate hook");
			ENGINE_CHECK_DEATH("Reflection/DuplicateTypeName", "The type name 'Inner' is already registered");
			ENGINE_CHECK_DEATH("Reflection/RegistrationAfterFreeze", "after TypeRegistry::Freeze");
			ENGINE_CHECK_DEATH("Reflection/MissingMigration", "has no migration from version 1");
			ENGINE_CHECK_DEATH("Reflection/FieldWithoutDescription", "needs a description");
		}
	}

}
