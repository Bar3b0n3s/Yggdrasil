#include "TestsPCH.h"

#include "Engine/Reflection/StructInfo.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/ReflectionTestTypes.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace Engine {

	static Scope<TypeRegistry> CreateStructTestRegistry()
	{
		Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
		Test::RegisterReflectionTestTypes(*registry);
		registry->Freeze();
		return registry;
	}

	static Json ParseStructJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	static void AppendFirst(Test::TestInner& inner, Random& /*random*/)
	{
		inner.Label += "first";
	}

	static void AppendSecond(Test::TestInner& inner, Random& /*random*/)
	{
		inner.Label += "second";
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("StructInfo: ToJson writes every serialized field in registration order with the §6 spelling")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			const Test::TestAllFields object;
			const Result<Json> json = type->ToJson(&object);
			REQUIRE(json.has_value());
			const Result<std::string> text = JsonWriter::Write(*json, JsonStyle::Minified);
			REQUIRE(text.has_value());
			CHECK(*text
				== R"({"Flag":true,"Count":3,"Size":4,"Mass":1,"Offset":[0,0],"Position":[0,0,0],"Plane":[0,1,0,0],"Rotation":[0,0,0,1],)"
				   R"("Tint":[1,1,1],"Glow":[1,1,1,1],"Locks":[false,false,false],"Name":"Sample","Target":null,"Mesh":null,"Shape":"Box",)"
				   R"("Labels":[],"Inner":{"Weight":1,"Label":"Inner"},"Scores":{},"Extra":null,"Scale":[1,1,1]})");
			CHECK(type->MakeDefaultJson() == *json);
		}

		TEST_CASE("StructInfo: FromJson keeps defaults for missing fields and warns about unknown ones")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			const Json document = ParseStructJson(R"({ "Count": 5, "Mas": 2, "Shape": "Sphere", "Target": "5d1c9a7e33b04f12" })");
			std::vector<ValidationIssue> diagnostics;
			ReadContext context;
			context.Diagnostics = &diagnostics;

			Test::TestAllFields object;
			REQUIRE(type->FromJson(&object, JsonReader(document), context).has_value());
			CHECK(object.Count == 5);
			CHECK(object.Mass == doctest::Approx(1.0f));
			CHECK(object.Shape == Test::TestShape::Sphere);
			CHECK(object.Target == UUID(0x5d1c9a7e33b04f12));

			REQUIRE(diagnostics.size() == 1);
			CHECK(diagnostics[0].Code == UnknownFieldCode);
			CHECK(diagnostics[0].JsonPointer == "/Mas");
			REQUIRE_FALSE(diagnostics[0].Suggestions.empty());
			CHECK(diagnostics[0].Suggestions[0] == "Mass");

			ReadContext strict;
			strict.Strict = true;
			Test::TestAllFields strictObject;
			CHECK_FALSE(type->FromJson(&strictObject, JsonReader(document), strict).has_value());
		}

		TEST_CASE("StructInfo: FromJson reports every invalid field and changes nothing")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			const Json document = ParseStructJson(R"({ "Count": 11, "Mass": "heavy", "Shape": "sphere", "Scale": [1, 0, 1], "Name": "Changed" })");
			Test::TestAllFields object;
			const Result<void> result = type->FromJson(&object, JsonReader(document), ReadContext{});
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Validation);
			std::vector<std::string> pointers;
			for (const ErrorIssue& issue : result.error().GetIssues())
				pointers.push_back(issue.JsonPointer);
			CHECK(pointers == std::vector<std::string>{ "/Count", "/Mass", "/Shape", "/Scale/1" });
			CHECK(object.Name == "Sample"); // atomic
		}

		TEST_CASE("StructInfo: ApplyMergePatch resets null members to defaults and replaces Variant values whole")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			Test::TestAllFields object;
			object.Count = 7;
			object.Inner.Weight = 3.0f;
			object.Inner.Label = "Changed";
			object.Extra = VariantValue(ParseStructJson(R"({ "a": 1, "b": 2 })"));

			const Json patch = ParseStructJson(R"({ "Count": null, "Inner": { "Weight": 2 }, "Extra": { "b": 3 } })");
			REQUIRE(type->ApplyMergePatch(&object, JsonReader(patch), ReadContext{}).has_value());
			CHECK(object.Count == 3);                                      // null resets to the default member initializer
			CHECK(object.Inner.Weight == doctest::Approx(2.0f));           // a nested struct merges...
			CHECK(object.Inner.Label == "Changed");                        // ...and keeps the members the patch does not name
			CHECK(object.Extra.Get() == ParseStructJson(R"({ "b": 3 })")); // a Variant is replaced, not merged

			const Json notAnObject = ParseStructJson("[1]");
			const Status rejected = type->ApplyMergePatch(&object, JsonReader(notAnObject), ReadContext{});
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("StructInfo: ToJson rejects a non-finite value written directly")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			Test::TestAllFields object;
			object.Position.y = std::numeric_limits<float>::quiet_NaN();
			const Result<Json> json = type->ToJson(&object);
			REQUIRE_FALSE(json.has_value());
			CHECK(json.error().GetCode() == ErrorCode::Validation);
			CHECK(json.error().GetLocation().JsonPointer == "/Position/1");
		}

		TEST_CASE("StructInfo: CreateDefault and the self field describe the whole struct")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestInner>();
			REQUIRE(type != nullptr);

			const ObjectPtr object = type->CreateDefault();
			REQUIRE(object != nullptr);
			CHECK(static_cast<const Test::TestInner*>(object.get())->Label == "Inner");

			const FieldInfo& self = type->GetSelfField();
			CHECK(self.GetName() == "TestInner");
			CHECK(self.GetKind() == FieldType::Struct);
			CHECK_FALSE(self.IsStored());
			CHECK(self.GetType().GetStruct() == type);
		}

		TEST_CASE("StructInfo: FromJson locates problems below the reader's pointer, a single one exactly")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			const Json single = ParseStructJson(R"({ "Inner": { "Weight": -1 } })");
			Test::TestAllFields object;
			const Status one = type->FromJson(&object, JsonReader(single, "/Entities/3/Components/Test"), ReadContext{});
			REQUIRE_FALSE(one.has_value());
			CHECK(one.error().GetLocation().JsonPointer == "/Entities/3/Components/Test/Inner/Weight");
			REQUIRE(one.error().GetIssues().size() == 1);

			const Json several = ParseStructJson(R"({ "Flag": 1, "Locks": [true, false], "Mesh": "nope" })");
			const Status many = type->FromJson(&object, JsonReader(several, "/Root"), ReadContext{});
			REQUIRE_FALSE(many.has_value());
			CHECK(many.error().GetLocation().JsonPointer == "/Root");
			CHECK(many.error().GetMessageText() == "3 invalid fields in TestAllFields");
			REQUIRE(many.error().GetIssues().size() == 3);
			CHECK(many.error().GetIssues()[2].JsonPointer == "/Root/Mesh");

			const Json notAnObject = ParseStructJson("[]");
			const Status array = type->FromJson(&object, JsonReader(notAnObject), ReadContext{});
			REQUIRE_FALSE(array.has_value());
			CHECK(array.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("StructInfo: FromJson reads every field kind and the result writes back identically")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			const std::string_view text =
				R"({"Flag":false,"Count":-10,"Size":100,"Mass":2.5,"Offset":[1,2],"Position":[1,2,3],"Plane":[0,0,1,-2],)"
				R"("Rotation":[0,0.70710677,0,0.70710677],"Tint":[0.25,0.5,1],"Glow":[1,0,0,0.5],"Locks":[true,false,true],"Name":"Ball",)"
				R"("Target":"5d1c9a7e33b04f12","Mesh":"0000000000000102","Shape":"Capsule","Labels":["a","b"],"Inner":{"Weight":0,"Label":""},)"
				R"("Scores":{"A":1,"b":-2},"Extra":{"z":[1,{"y":null}]},"Scale":[-1,1e-04,2]})";
			const Json document = ParseStructJson(text);
			Test::TestAllFields object;
			REQUIRE(type->FromJson(&object, JsonReader(document), ReadContext{}).has_value());
			CHECK(object.Rotation.y == doctest::Approx(0.70710677f));
			CHECK(object.Mesh.GetHandle() == AssetHandle(0x102));
			CHECK(object.Shape == Test::TestShape::Capsule);
			CHECK(object.Locks == glm::bvec3(true, false, true));

			const Result<Json> written = type->ToJson(&object);
			REQUIRE(written.has_value());
			const Result<std::string> canonical = JsonWriter::Write(*written, JsonStyle::Minified);
			REQUIRE(canonical.has_value());
			CHECK(*canonical == text);
		}

		TEST_CASE("StructInfo: Validate reports nested fields at their pointers and skips type rules of invalid structs")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* allFields = registry->FindStruct<Test::TestAllFields>();
			const ComponentInfo* component = registry->FindComponent<Test::TestComponent>();
			REQUIRE(allFields != nullptr);
			REQUIRE(component != nullptr);

			Test::TestAllFields object;
			object.Inner.Weight = -2.0f;
			object.Scores["bad"] = std::numeric_limits<float>::infinity();
			object.Shape = static_cast<Test::TestShape>(9);
			ResolveContext resolve;
			resolve.Registry = registry.get();
			ValidationContext validation("/data");
			allFields->Validate(&object, resolve, validation);
			std::vector<std::string> pointers;
			for (const ValidationIssue& issue : validation.GetIssues())
				pointers.push_back(issue.JsonPointer);
			CHECK(pointers == std::vector<std::string>{ "/data/Shape", "/data/Inner/Weight", "/data/Scores/bad" });

			// A negative mass fails its Min, so the sphere rule (which would report the same field) does not run.
			Test::TestComponent sphere;
			sphere.Shape = Test::TestShape::Sphere;
			sphere.Mass = -1.0f;
			ValidationContext rules;
			component->Validate(&sphere, resolve, rules);
			REQUIRE(rules.GetErrorCount() == 1);
			CHECK(rules.GetIssues()[0].Message.find(">=") != std::string::npos);
		}

		TEST_CASE("StructInfo: ApplyMergePatch replaces Variant map values whole and deletes them on null")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const ComponentInfo* type = registry->FindComponent<Test::TestComponent>();
			REQUIRE(type != nullptr);
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			ReadContext context;
			context.Schemas = &schemas;

			Test::TestComponent object;
			object.Owner = TypedAssetHandle<AssetType::Script>(Test::FixtureSchemaSource::DefaultOwner);
			object.Overrides["Tint"] = VariantValue(ParseStructJson("[1, 1, 1, 1]"));
			object.Overrides["Label"] = VariantValue(Json("old"));
			object.Overrides["Count"] = VariantValue(Json(3));

			const Json patch = ParseStructJson(R"({ "Overrides": { "Tint": [0, 0, 0, 1], "Label": null, "Torque": 12.5 }, "Mass": 4 })");
			REQUIRE(type->ApplyMergePatch(&object, JsonReader(patch), context).has_value());
			CHECK(object.Overrides.size() == 3);
			CHECK(object.Overrides.at("Tint").Get() == ParseStructJson("[0, 0, 0, 1]"));
			CHECK(object.Overrides.at("Torque").Get() == Json(12.5));
			CHECK(object.Overrides.at("Count").Get() == Json(3));
			CHECK_FALSE(object.Overrides.contains("Label"));
			CHECK(object.Mass == 4.0f);

			// A patch whose result breaks the type-level rule is rejected as a whole.
			const Json breaksRule = ParseStructJson(R"({ "Shape": "Sphere", "Mass": 0 })");
			CHECK_FALSE(type->ApplyMergePatch(&object, JsonReader(breaksRule), context).has_value());
			CHECK(object.Shape == Test::TestShape::Box);
			CHECK(object.Mass == 4.0f);
		}

		TEST_CASE("StructInfo: field lookup is exact and suggestions cover every field")
		{
			const Scope<TypeRegistry> registry = CreateStructTestRegistry();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);
			CHECK(type->FindField("Mass") != nullptr);
			CHECK(type->FindField("mass") == nullptr);
			CHECK(type->SuggestFieldNames("Posiiton") == std::vector<std::string>{ "Position" });
			CHECK(type->SuggestFieldNames("Mas", 1) == std::vector<std::string>{ "Mass" });
			CHECK(type->GetRegistry().FindStruct("TestAllFields") == type);
			CHECK(type->GetType().GetKind() == FieldType::Struct);
			CHECK(type->GetType().GetName() == "TestAllFields");
			CHECK_FALSE(type->HasValidators());
			CHECK_FALSE(type->HasGenerators());
		}

		TEST_CASE("StructInfo: a schema-only struct validates JSON without C++ storage")
		{
			// The shape of a script-declared struct schema (M13): schema-only fields, no TypeOps.
			TypeRegistry registry;
			StructInfo schema("ScriptPoint", "A script-declared struct schema.", registry);
			TypeInfo::Specification structSpecification;
			structSpecification.Kind = FieldType::Struct;
			structSpecification.Name = "ScriptPoint";
			structSpecification.Struct = &schema;
			const TypeInfo structType(std::move(structSpecification));
			schema.SetType(structType);

			TypeInfo::Specification numberSpecification;
			numberSpecification.Kind = FieldType::Float;
			numberSpecification.Name = "Float";
			const TypeInfo numberType(std::move(numberSpecification));
			FieldInfo::Specification x;
			x.Name = "X";
			x.Description = "The x coordinate.";
			x.Type = &numberType;
			x.Meta.Min = 0.0;
			schema.AddField(std::move(x));
			REQUIRE(schema.FindField("X") != nullptr);
			CHECK_FALSE(schema.FindField("X")->IsStored());

			const Json document = ParseStructJson(R"({ "X": -1, "Y": 2 })");
			ValidationContext validation;
			schema.GetSelfField().ValidateJson(JsonReader(document), ResolveContext{}, validation);
			REQUIRE(validation.GetIssues().size() == 2);
			CHECK(validation.GetIssues()[0].JsonPointer == "/ScriptPoint/Y");
			CHECK(validation.GetIssues()[0].Code == UnknownFieldCode);
			CHECK(validation.GetIssues()[0].Severity == DiagnosticSeverity::Warning);
			CHECK(validation.GetIssues()[1].JsonPointer == "/ScriptPoint/X");
			CHECK(validation.GetIssues()[1].Severity == DiagnosticSeverity::Error);

			const Json jsonSchema = JsonSchema::ForStruct(schema);
			CHECK(JsonSchema::Validate(jsonSchema, ParseStructJson(R"({ "X": 1 })")).has_value());
			CHECK_FALSE(JsonSchema::Validate(jsonSchema, ParseStructJson(R"({ "X": -1 })")).has_value());
		}

		TEST_CASE("StructInfo: Generate runs the hooks in registration order")
		{
			TypeRegistry registry;
			registry.Struct<Test::TestInner>("HookedInner", "A struct with two hooks.")
				.Field("Weight", &Test::TestInner::Weight, "A weight.")
				.Field("Label", &Test::TestInner::Label, "A label.")
				.Generate(&AppendFirst)
				.Generate(&AppendSecond);
			registry.Freeze();

			const StructInfo* type = registry.FindStruct("HookedInner");
			REQUIRE(type != nullptr);
			CHECK(type->HasGenerators());
			Test::TestInner inner;
			inner.Label.clear();
			Random random(1);
			type->Generate(&inner, random);
			CHECK(inner.Label == "firstsecond");
		}
	}

}
