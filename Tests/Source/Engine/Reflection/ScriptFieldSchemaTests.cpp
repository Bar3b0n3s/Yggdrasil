#include "TestsPCH.h"
#include "Engine/Asset/ScriptData.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/RandomValueGenerator.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Support/SceneTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Reflection")
	{
		TEST_CASE("ScriptFieldSchema: nested array metadata is enforced at the exact element pointer")
		{
			auto number = CreateRef<ScriptFieldSchema>();
			number->Type = FieldType::Float;
			number->DefaultValue = VariantValue(Json(7.5));
			number->Meta.Min = 7.0;
			number->Meta.Max = 8.0;
			auto row = CreateRef<ScriptFieldSchema>();
			row->Type = FieldType::Array;
			row->DefaultValue = VariantValue(Json::array());
			row->Element = number;
			ScriptFieldSchema matrix{};
			matrix.Name = "Matrix";
			matrix.Type = FieldType::Array;
			matrix.DefaultValue = VariantValue(Json::array());
			matrix.Element = row;
			auto script = CreateRef<ScriptData>();
			script->Kind = ScriptKind::Behaviour;
			script->Name = "Arrays";
			script->Fields.push_back(matrix);
			const AssetHandle handle(0x200030);
			const auto source = ScriptFieldSchemaSource::Create({ { handle, script } });
			REQUIRE(source);
			const auto found = (*source)->FindField(handle, "Matrix");
			REQUIRE(found);
			const FieldInfo& field = **found;
			const Json good = Json::array({ Json::array({ 7.0, 7.5, 8.0 }), Json::array() });
			const Json bad = Json::array({ Json::array({ 7.0, 6.0 }), Json::array({ 9.0 }) });
			ResolveContext resolve{};
			resolve.Schemas = source->get();
			ValidationContext valid;
			field.ValidateJson(JsonReader(good), resolve, valid);
			CHECK_FALSE(valid.HasErrors());
			ValidationContext invalid;
			field.ValidateJson(JsonReader(bad), resolve, invalid);
			REQUIRE(invalid.GetIssues().size() == 2);
			CHECK(invalid.GetIssues()[0].JsonPointer == "/Matrix/0/1");
			CHECK(invalid.GetIssues()[1].JsonPointer == "/Matrix/1/0");
			const auto converted = ValueFromJson(JsonReader(bad), field.GetType());
			REQUIRE(converted);
			ValidationContext invalidValue;
			field.ValidateValue(*converted, resolve, invalidValue);
			REQUIRE(invalidValue.GetIssues().size() == 2);
			CHECK(invalidValue.GetIssues()[0].JsonPointer == "/Matrix/0/1");
			CHECK(invalidValue.GetIssues()[1].JsonPointer == "/Matrix/1/0");
			const Json schema = JsonSchema::ForField(field);
			CHECK(JsonSchema::Validate(schema, good).has_value());
			CHECK_FALSE(JsonSchema::Validate(schema, bad));
			Test::SceneTestFixture fixture;
			RandomValueGenerator generator(fixture.GetRegistry(), 71);
			bool sawElement = false;
			for (uint32_t iteration = 0; iteration < 200; ++iteration)
			{
				const Json generated = generator.RandomJson(field, resolve);
				CHECK(JsonSchema::Validate(schema, generated).has_value());
				ValidationContext checked;
				field.ValidateJson(JsonReader(generated), resolve, checked);
				CHECK_FALSE(checked.HasErrors());
				for (const Json& inner : generated)
					sawElement = sawElement || !inner.empty();
			}
			CHECK(sawElement);
		}
	}

}
