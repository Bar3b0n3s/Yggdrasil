#include "TestsPCH.h"

#include "Support/FixtureSchemaSource.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("FixtureSchemaSource: the standard schema declares its six fields in declaration order")
		{
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			const std::vector<std::string> expected = { "Torque", "Count", "Enabled", "Goal", "Tint", "Label" };
			CHECK(schemas.GetFieldNames(Test::FixtureSchemaSource::DefaultOwner) == expected);

			const std::pair<std::string_view, FieldType> kinds[] = {
				{ "Torque", FieldType::Float },
				{ "Count", FieldType::Int32 },
				{ "Enabled", FieldType::Bool },
				{ "Goal", FieldType::EntityRef },
				{ "Tint", FieldType::Color4 },
				{ "Label", FieldType::String },
			};
			for (const auto& [name, kind] : kinds)
			{
				INFO(std::string(name));
				const Result<const FieldInfo*> field = schemas.FindField(Test::FixtureSchemaSource::DefaultOwner, name);
				REQUIRE(field.has_value());
				CHECK((*field)->GetName() == std::string(name));
				CHECK((*field)->GetKind() == kind);
				CHECK_FALSE((*field)->IsStored()); // schema-only, like a script field schema
				CHECK_FALSE((*field)->GetDescription().empty());
			}

			const Result<const FieldInfo*> torque = schemas.FindField(Test::FixtureSchemaSource::DefaultOwner, "Torque");
			REQUIRE(torque.has_value());
			REQUIRE((*torque)->GetMeta().Min.has_value());
			REQUIRE((*torque)->GetMeta().Max.has_value());
			CHECK(*(*torque)->GetMeta().Min == doctest::Approx(0.0));
			CHECK(*(*torque)->GetMeta().Max == doctest::Approx(200.0));
		}

		TEST_CASE("FixtureSchemaSource: an owner without declarations answers with DefaultOwner's fields")
		{
			Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			const UUID randomScript(0x5d1c9a7e33b04f12);
			CHECK(schemas.GetFieldNames(randomScript) == schemas.GetFieldNames(Test::FixtureSchemaSource::DefaultOwner));
			const Result<const FieldInfo*> fallback = schemas.FindField(randomScript, "Goal");
			REQUIRE(fallback.has_value());
			CHECK(*fallback == *schemas.FindField(Test::FixtureSchemaSource::DefaultOwner, "Goal"));

			// An owner that declares fields of its own answers with those only.
			const UUID declaring(0x77e1a0c4d2b95f01);
			schemas.DeclareField("Speed", FieldType::Float, FieldMeta{ .Min = 0.0 }, declaring);
			CHECK(schemas.GetFieldNames(declaring) == std::vector<std::string>{ "Speed" });
			CHECK_FALSE(schemas.FindField(declaring, "Goal").has_value());
		}

		TEST_CASE("FixtureSchemaSource: an unknown field is NotFound with suggestions")
		{
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			const Result<const FieldInfo*> missing = schemas.FindField(Test::FixtureSchemaSource::DefaultOwner, "Torqe");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetHint().find("Torque") != std::string::npos);
		}

		TEST_CASE("FixtureSchemaSource: field addresses stay stable when the source moves")
		{
			Test::FixtureSchemaSource original = Test::FixtureSchemaSource::CreateStandard();
			const Result<const FieldInfo*> before = original.FindField(Test::FixtureSchemaSource::DefaultOwner, "Label");
			REQUIRE(before.has_value());

			const Test::FixtureSchemaSource moved = std::move(original);
			const Result<const FieldInfo*> after = moved.FindField(Test::FixtureSchemaSource::DefaultOwner, "Label");
			REQUIRE(after.has_value());
			CHECK(*after == *before); // VariantSchemaResolver results keep pointing at the same FieldInfo
		}
	}

}
