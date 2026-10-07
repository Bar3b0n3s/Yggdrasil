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
		TEST_CASE("JsonSchema: struct schemas map every FieldType and validate default output")
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

		TEST_CASE("JsonSchema: Validate reports every violation with its pointer")
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

		TEST_CASE("JsonSchema: an AssetRef accepts a handle, a project path or an engine path, an EntityRef only a handle")
		{
			// §7.1 and MethodRegistry.h convention 13: automation params take every spelling of an asset reference, so the
			// schemas rpc.discover and component.schema publish must too.
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);
			const Json schema = JsonSchema::ForStruct(*type);
			CHECK(schema["properties"]["Mesh"]["pattern"] == "^([0-9a-fA-F]{16}|Assets/.+|engine://.+)$");
			CHECK(schema["properties"]["Target"]["pattern"] == "^[0-9a-fA-F]{16}$");

			for (const std::string_view mesh : { "0000000000000101", "Assets/Models/Track.glb#mesh:0:Straight", "engine://Meshes/Cube" })
			{
				CAPTURE(std::string(mesh));
				Json instance = type->MakeDefaultJson();
				instance["Mesh"] = std::string(mesh);
				CHECK(JsonSchema::Validate(schema, instance).has_value());
			}
			for (const std::string_view mesh : { "Meshes/Cube", "Assets/", "engine://", "000000000000010" })
			{
				CAPTURE(std::string(mesh));
				Json instance = type->MakeDefaultJson();
				instance["Mesh"] = std::string(mesh);
				const Status status = JsonSchema::Validate(schema, instance);
				REQUIRE_FALSE(status.has_value());
				REQUIRE(status.error().GetIssues().size() == 1);
				CHECK(status.error().GetIssues()[0].JsonPointer == "/Mesh");
			}
			Json entity = type->MakeDefaultJson();
			entity["Target"] = "Assets/Materials/Red.material";
			CHECK_FALSE(JsonSchema::Validate(schema, entity).has_value());
		}

		TEST_CASE("JsonSchema: unsupported keywords are rejected as invalid schemas")
		{
			const Json schema = ParseSchemaJson(R"({ "type": "string", "format": "email" })");
			const Status status = JsonSchema::Validate(schema, Json("a@b.c"));
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);

			const Json dangling = ParseSchemaJson(R"({ "$ref": "#/$defs/Missing", "$defs": {} })");
			const Status unresolved = JsonSchema::Validate(dangling, Json(1));
			REQUIRE_FALSE(unresolved.has_value());
			CHECK(unresolved.error().GetCode() == ErrorCode::InvalidArgument);

			const Json cyclic = ParseSchemaJson(R"({ "$ref": "#/$defs/A", "$defs": { "A": { "$ref": "#/$defs/A" } } })");
			const Status loop = JsonSchema::Validate(cyclic, Json(1));
			REQUIRE_FALSE(loop.has_value());
			CHECK(loop.error().GetCode() == ErrorCode::InvalidArgument);

			const Json otherPattern = ParseSchemaJson(R"({ "pattern": "^a+$" })");
			CHECK(JsonSchema::Validate(otherPattern, Json("aaa")).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("JsonSchema: the validator applies number, array, enum and magnitude keywords")
		{
			const Json integer = ParseSchemaJson(R"({ "type": "integer", "minimum": 0, "maximum": 10 })");
			CHECK(JsonSchema::Validate(integer, Json(3)).has_value());
			CHECK(JsonSchema::Validate(integer, Json(3.0)).has_value()); // an integral number is an integer
			CHECK_FALSE(JsonSchema::Validate(integer, Json(3.5)).has_value());
			CHECK_FALSE(JsonSchema::Validate(integer, Json(-1)).has_value());
			CHECK_FALSE(JsonSchema::Validate(integer, Json(11)).has_value());

			const Json magnitude = ParseSchemaJson(R"({ "type": "number", "not": { "exclusiveMinimum": -0.5, "exclusiveMaximum": 0.5 } })");
			CHECK(JsonSchema::Validate(magnitude, Json(0.5)).has_value());
			CHECK(JsonSchema::Validate(magnitude, Json(-2)).has_value());
			CHECK_FALSE(JsonSchema::Validate(magnitude, Json(0.25)).has_value());

			const Json tuple = ParseSchemaJson(R"({ "type": "array", "prefixItems": [{ "type": "boolean" }, { "type": "string" }], "items": { "type": "null" }, "maxItems": 3 })");
			CHECK(JsonSchema::Validate(tuple, ParseSchemaJson(R"([true, "a", null])")).has_value());
			const Status wrong = JsonSchema::Validate(tuple, ParseSchemaJson(R"([1, "a", 2, null])"));
			REQUIRE_FALSE(wrong.has_value());
			std::vector<std::string> pointers;
			for (const ErrorIssue& issue : wrong.error().GetIssues())
				pointers.push_back(issue.JsonPointer);
			CHECK(pointers == std::vector<std::string>{ "/0", "/2", "" });

			const Json nullable = ParseSchemaJson(R"({ "type": ["string", "null"], "pattern": "^[0-9a-fA-F]{16}$" })");
			CHECK(JsonSchema::Validate(nullable, Json()).has_value());
			CHECK(JsonSchema::Validate(nullable, Json("00000000000003FF")).has_value());
			CHECK_FALSE(JsonSchema::Validate(nullable, Json("3ff")).has_value());
			CHECK_FALSE(JsonSchema::Validate(nullable, Json(5)).has_value());

			const Json anything = Json::object();
			CHECK(JsonSchema::Validate(anything, ParseSchemaJson(R"({ "a": [1, { "b": null }] })")).has_value());
		}

		TEST_CASE("JsonSchema: required lists the members an object must have")
		{
			const Json schema = ParseSchemaJson(R"({ "type": "object", "properties": { "text": { "type": "string" } }, "required": ["text"] })");
			CHECK(JsonSchema::Validate(schema, ParseSchemaJson(R"({ "text": "a" })")).has_value());
			const Status missing = JsonSchema::Validate(schema, Json::object());
			REQUIRE_FALSE(missing.has_value());
			REQUIRE(missing.error().GetIssues().size() == 1);
			CHECK(missing.error().GetIssues()[0].JsonPointer.empty());
			CHECK(missing.error().GetIssues()[0].Message.contains("text"));
			CHECK(JsonSchema::Validate(schema, Json(3)).error().GetIssues().size() == 1); // the type, not the member

			const Json malformed = ParseSchemaJson(R"({ "required": "text" })");
			CHECK(JsonSchema::Validate(malformed, Json::object()).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("JsonSchema: field and component schemas carry the definitions they reference")
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();
			const StructInfo* allFields = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(allFields != nullptr);

			const Json inner = JsonSchema::ForField(*allFields->FindField("Inner"));
			CHECK(inner["$ref"] == "#/$defs/TestInner");
			CHECK(inner["description"] == "A Struct.");
			CHECK(inner["$defs"]["TestInner"]["properties"]["Weight"]["minimum"] == 0);
			CHECK(JsonSchema::Validate(inner, ParseSchemaJson(R"({ "Weight": 2, "Label": "x" })")).has_value());
			CHECK_FALSE(JsonSchema::Validate(inner, ParseSchemaJson(R"({ "Weight": -2 })")).has_value());

			const Json scores = JsonSchema::ForField(*allFields->FindField("Scores"));
			CHECK_FALSE(scores.contains("$defs"));
			CHECK(JsonSchema::Validate(scores, ParseSchemaJson(R"({ "a": 1, "b": 2.5 })")).has_value());
			CHECK_FALSE(JsonSchema::Validate(scores, ParseSchemaJson(R"({ "a": "x" })")).has_value());

			const Json scale = JsonSchema::ForField(*allFields->FindField("Scale"));
			CHECK_FALSE(JsonSchema::Validate(scale, ParseSchemaJson("[1, 0, 1]")).has_value());
			CHECK(JsonSchema::Validate(scale, ParseSchemaJson("[1, -0.0001, 1]")).has_value());

			const Json components = JsonSchema::ForComponents(*registry);
			CHECK(components["$schema"] == JsonSchema::Dialect);
			CHECK(components["properties"]["TestComponent"]["$ref"] == "#/$defs/TestComponent");
			CHECK(components["$defs"]["TestComponent"]["properties"]["Overrides"]["additionalProperties"] == Json::object());
			CHECK(components["$defs"]["TestComponent"]["properties"]["Owner"]["x-assetType"] == "Script");
			const Json entity = ParseSchemaJson(R"({ "TestComponent": { "Owner": null, "Overrides": { "Torque": [1] }, "Mass": 1, "Shape": "Box" } })");
			CHECK(JsonSchema::Validate(components, entity).has_value());
			const Json unknown = ParseSchemaJson(R"({ "Bogus": {} })");
			CHECK_FALSE(JsonSchema::Validate(components, unknown).has_value());
		}
	}

}
