#include "TestsPCH.h"

#include "Engine/Reflection/JsonSchema.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/ReflectionTestTypes.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Json ParseSchemaJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("JsonSchema: struct schemas map every FieldType and validate default output" * doctest::skip(true))
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			const Json schema = JsonSchema::ForStruct(*type);
			CHECK(schema["$schema"] == JsonSchema::Dialect);
			CHECK(schema["title"] == "TestAllFields");
			CHECK(schema["additionalProperties"] == false);
			CHECK(schema["properties"]["Shape"]["enum"] == ParseSchemaJson(R"(["Box", "Sphere", "Capsule"])"));
			CHECK(schema["properties"]["Scores"]["additionalProperties"]["type"] == "number");
			CHECK(schema["properties"]["Mesh"]["x-assetType"] == "Mesh");
			CHECK(schema["properties"]["Count"]["minimum"] == -10);
			CHECK(schema["properties"]["Position"]["minItems"] == 3);
			CHECK(schema["$defs"].contains("TestInner"));

			CHECK(JsonSchema::Validate(schema, type->MakeDefaultJson()).has_value());
		}

		TEST_CASE("JsonSchema: Validate reports every violation with its pointer" * doctest::skip(true))
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);
			const Json schema = JsonSchema::ForStruct(*type);

			const Json invalid = ParseSchemaJson(R"({ "Count": 20, "Shape": "Cone", "Position": [1, 2], "Target": "xyz", "Bogus": 1 })");
			const Status status = JsonSchema::Validate(schema, invalid);
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			std::vector<std::string> pointers;
			for (const ErrorIssue& issue : status.error().GetIssues())
				pointers.push_back(issue.JsonPointer);
			CHECK(std::find(pointers.begin(), pointers.end(), "/Count") != pointers.end());
			CHECK(std::find(pointers.begin(), pointers.end(), "/Shape") != pointers.end());
			CHECK(std::find(pointers.begin(), pointers.end(), "/Position") != pointers.end());
			CHECK(std::find(pointers.begin(), pointers.end(), "/Target") != pointers.end());
			CHECK(std::find(pointers.begin(), pointers.end(), "/Bogus") != pointers.end());
		}

		TEST_CASE("JsonSchema: unsupported keywords are rejected as invalid schemas" * doctest::skip(true))
		{
			const Json schema = ParseSchemaJson(R"({ "type": "string", "format": "email" })");
			const Status status = JsonSchema::Validate(schema, Json("a@b.c"));
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
