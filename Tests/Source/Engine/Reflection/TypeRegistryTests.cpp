#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
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

	TEST_SUITE("Reflection")
	{
		TEST_CASE("TypeRegistry: registered types are found by name and by C++ type" * doctest::skip(true))
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

		TEST_CASE("TypeRegistry: fields deduce their FieldType and keep registration order" * doctest::skip(true))
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

		TEST_CASE("TypeRegistry: unknown component names get fuzzy suggestions" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			const std::vector<std::string> suggestions = registry->SuggestComponentNames("TestComponnt");
			REQUIRE_FALSE(suggestions.empty());
			CHECK(suggestions.front() == "TestComponent");
			CHECK(registry->SuggestComponentNames("Zzzzzzzzzzzzzzzz").empty());
		}

		TEST_CASE("TypeRegistry: AreComponentsRegistered checks a whole type list" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = CreateTestRegistry();
			CHECK(registry->AreComponentsRegistered(TypeList<Test::TestComponent>{}));
			CHECK_FALSE(registry->AreComponentsRegistered(TypeList<Test::TestComponent, Test::TestInner>{}));
		}

		TEST_CASE("Map: keys serialize sorted and merge-patch deletes on null" * doctest::skip(true))
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

		TEST_CASE("Variant: value validated against the resolved schema; unresolvable values are preserved with a diagnostic" * doctest::skip(true))
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

		TEST_CASE("TypeRegistry: type-level validators run after field checks" * doctest::skip(true))
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
	}

}
