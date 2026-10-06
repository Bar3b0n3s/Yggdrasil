#include "TestsPCH.h"

#include "Engine/Reflection/StructInfo.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/TypeRegistry.h"
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

	TEST_SUITE("Reflection")
	{
		TEST_CASE("StructInfo: ToJson writes every serialized field in registration order with the §6 spelling" * doctest::skip(true))
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

		TEST_CASE("StructInfo: FromJson keeps defaults for missing fields and warns about unknown ones" * doctest::skip(true))
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

		TEST_CASE("StructInfo: FromJson reports every invalid field and changes nothing" * doctest::skip(true))
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

		TEST_CASE("StructInfo: ApplyMergePatch resets null members to defaults and replaces Variant values whole" * doctest::skip(true))
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

		TEST_CASE("StructInfo: ToJson rejects a non-finite value written directly" * doctest::skip(true))
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

		TEST_CASE("StructInfo: CreateDefault and the self field describe the whole struct" * doctest::skip(true))
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
	}

}
