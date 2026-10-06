#include "TestsPCH.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/RandomValueGenerator.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <limits>

// The registry-parametrized suite of Architecture §5.4 and Roadmap M3: for every registered component and reflected
// struct, defaults round-trip, 200 seeded random values round-trip (Map and Variant fields included, through the fixture
// schema source), out-of-range and non-finite values are rejected, and the generated schema validates the serialized
// output. Script get/set (M13) and automation set/get (M4) join the suite with their milestones.

namespace Engine {

	// Every struct of the registry, components included, so each case covers both.
	static std::vector<const StructInfo*> GetAllReflectedStructs(const TypeRegistry& registry)
	{
		std::vector<const StructInfo*> types;
		for (const ComponentInfo* component : registry.GetComponents())
			types.push_back(component);
		for (const StructInfo* type : registry.GetStructs())
			types.push_back(type);
		return types;
	}

	static std::string WriteCanonical(const Json& json)
	{
		const Result<std::string> text = JsonWriter::Write(json, JsonStyle::Minified);
		REQUIRE(text.has_value());
		return *text;
	}

	// Reads `json` into a fresh default object of `type` and writes it back.
	static std::string RoundTrip(const StructInfo& type, const Json& json, const ReadContext& context)
	{
		const ObjectPtr object = type.CreateDefault();
		REQUIRE(object != nullptr);
		const Status read = type.FromJson(object.get(), JsonReader(json), context);
		INFO(type.GetName(), ": ", read.has_value() ? std::string() : read.error().ToString());
		REQUIRE(read.has_value());
		const Result<Json> written = type.ToJson(object.get());
		REQUIRE(written.has_value());
		return WriteCanonical(*written);
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("TypeRegistry: every built-in component and reflected struct round-trips its defaults" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			REQUIRE(registry->AreComponentsRegistered(BuiltinComponents{}));
			REQUIRE(registry->GetComponents().size() == BuiltinComponents::Size);

			for (const StructInfo* type : GetAllReflectedStructs(*registry))
			{
				INFO(type->GetName());
				const Json defaults = type->MakeDefaultJson();
				CHECK(defaults.is_object());
				CHECK(RoundTrip(*type, defaults, ReadContext{}) == WriteCanonical(defaults));
				CHECK_FALSE(type->GetDescription().empty());
				for (const Scope<FieldInfo>& field : type->GetFields())
					CHECK_FALSE(field->GetDescription().empty());
			}
		}

		TEST_CASE("TypeRegistry: 200 seeded random values round-trip for every component and reflected struct" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			RandomValueOptions options;
			options.Schemas = &schemas;
			options.VariantMapKeys = schemas.GetFieldNames(Test::FixtureSchemaSource::DefaultOwner);
			ReadContext context;
			context.Schemas = &schemas;

			for (const StructInfo* type : GetAllReflectedStructs(*registry))
			{
				INFO(type->GetName());
				// Field metadata alone cannot satisfy a type-level rule (ProjectSettings' layers, SpotLight's cone angles):
				// the type's Generate hook repairs the random object (asserted by Freeze, checked here for the message).
				CHECK((!type->HasValidators() || type->HasGenerators()));
				RandomValueGenerator generator(*registry, 0x5eed, options);
				for (int iteration = 0; iteration < 200; ++iteration)
				{
					const ObjectPtr object = type->CreateDefault();
					REQUIRE(object != nullptr);
					generator.Randomize(*type, object.get());
					const Result<Json> json = type->ToJson(object.get());
					REQUIRE(json.has_value());
					CHECK(RoundTrip(*type, *json, context) == WriteCanonical(*json));
				}
			}
		}

		TEST_CASE("TypeRegistry: validation rejects out-of-range and non-finite values for every field" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			for (const StructInfo* type : GetAllReflectedStructs(*registry))
			{
				for (const Scope<FieldInfo>& field : type->GetFields())
				{
					if (!field->IsStored() || field->IsReadOnly())
						continue;
					INFO(type->GetName(), ".", field->GetName());
					const ObjectPtr object = type->CreateDefault();
					REQUIRE(object != nullptr);
					const FieldContext context{ object.get(), nullptr };
					const FieldMeta& meta = field->GetMeta();

					if (field->GetKind() == FieldType::Float)
					{
						CHECK_FALSE(field->SetValue(context, Value::FromFloat(std::numeric_limits<float>::quiet_NaN())).has_value());
						CHECK_FALSE(field->SetValue(context, Value::FromFloat(std::numeric_limits<float>::infinity())).has_value());
						if (meta.Min)
							CHECK_FALSE(field->SetValue(context, Value::FromFloat(static_cast<float>(*meta.Min) - 1.0f)).has_value());
						if (meta.Max)
							CHECK_FALSE(field->SetValue(context, Value::FromFloat(static_cast<float>(*meta.Max) + 1.0f)).has_value());
					}
					else if (field->GetKind() == FieldType::Vec3 || field->GetKind() == FieldType::Color3)
					{
						const glm::vec3 nan(0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f);
						const Value value = field->GetKind() == FieldType::Vec3 ? Value::FromVec3(nan) : Value::FromColor3(nan);
						CHECK_FALSE(field->SetValue(context, value).has_value());
						if (meta.MinMagnitude)
							CHECK_FALSE(field->SetValue(context, Value::FromVec3(glm::vec3(0.0f))).has_value());
						if (meta.Min)
						{
							const glm::vec3 below(static_cast<float>(*meta.Min) - 1.0f);
							const Value belowMin = field->GetKind() == FieldType::Vec3 ? Value::FromVec3(below) : Value::FromColor3(below);
							CHECK_FALSE(field->SetValue(context, belowMin).has_value());
						}
					}
					else if (field->GetKind() == FieldType::Int32 || field->GetKind() == FieldType::UInt32)
					{
						if (meta.Max)
						{
							const auto above = static_cast<uint32_t>(*meta.Max) + 1u;
							const Value value = field->GetKind() == FieldType::Int32 ? Value::FromInt32(static_cast<int32_t>(above))
																					 : Value::FromUInt32(above);
							CHECK_FALSE(field->SetValue(context, value).has_value());
						}
					}
					else if (field->GetKind() == FieldType::Enum)
					{
						CHECK_FALSE(field->SetValue(context, Value::FromEnum(1000)).has_value());
					}
				}
			}
		}

		TEST_CASE("TypeRegistry: the generated schema validates the serialized output of every type" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			RandomValueOptions options;
			options.Schemas = &schemas;
			options.VariantMapKeys = schemas.GetFieldNames(Test::FixtureSchemaSource::DefaultOwner);

			for (const StructInfo* type : GetAllReflectedStructs(*registry))
			{
				INFO(type->GetName());
				const Json schema = JsonSchema::ForStruct(*type);
				CHECK(JsonSchema::Validate(schema, type->MakeDefaultJson()).has_value());

				RandomValueGenerator generator(*registry, 0xc0de, options);
				for (int iteration = 0; iteration < 20; ++iteration)
				{
					const ObjectPtr object = type->CreateDefault();
					REQUIRE(object != nullptr);
					generator.Randomize(*type, object.get());
					const Result<Json> json = type->ToJson(object.get());
					REQUIRE(json.has_value());
					CHECK(JsonSchema::Validate(schema, *json).has_value());
				}
			}

			// Every serializable, non-entity-level component is a definition and a property of the root object (the shape of an
			// entity's "Components"); structs they reference, such as PrefabOverride, add definitions of their own.
			Json components = JsonSchema::ForComponents(*registry);
			for (const ComponentInfo* component : registry->GetComponents())
			{
				if (!component->HasFlag(ComponentFlags::Serializable) || component->HasFlag(ComponentFlags::EntityLevel))
					continue;
				INFO(component->GetName());
				CHECK(components["$defs"].contains(component->GetName()));
				CHECK(components["properties"].contains(component->GetName()));
			}
			CHECK(components["$defs"].contains("PrefabOverride"));

			// Every $ref resolves (an unresolvable one is InvalidArgument): the Components object of every entity of the
			// fixture that uses every component validates against the document.
			const Result<std::string> text = Test::ReadTestDataText("Scenes/AllComponents.scene");
			REQUIRE(text.has_value());
			Result<Json> scene = JsonReader::Parse(*text);
			REQUIRE(scene.has_value());
			for (const Json& entity : (*scene)["Entities"])
			{
				const Status valid = JsonSchema::Validate(components, entity["Components"]);
				INFO((valid.has_value() ? std::string() : valid.error().ToString()));
				CHECK(valid.has_value());
			}
		}
	}

}
