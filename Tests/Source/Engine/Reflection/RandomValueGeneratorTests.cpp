#include "TestsPCH.h"

#include "Engine/Reflection/RandomValueGenerator.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/GlmApprox.h"
#include "Support/ReflectionTestTypes.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static std::string RandomizedText(const TypeRegistry& registry, uint64_t seed)
	{
		const StructInfo* type = registry.FindStruct<Test::TestAllFields>();
		REQUIRE(type != nullptr);
		RandomValueGenerator generator(registry, seed);
		Test::TestAllFields object;
		generator.Randomize(*type, &object);
		const Result<Json> json = type->ToJson(&object);
		REQUIRE(json.has_value());
		const Result<std::string> text = JsonWriter::Write(*json, JsonStyle::Minified);
		REQUIRE(text.has_value());
		return *text;
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("RandomValueGenerator: the same seed gives the same values" * doctest::skip(true))
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();

			CHECK(RandomizedText(*registry, 7) == RandomizedText(*registry, 7));
			CHECK(RandomizedText(*registry, 7) != RandomizedText(*registry, 8));
		}

		TEST_CASE("RandomValueGenerator: every generated value passes validation" * doctest::skip(true))
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();
			const StructInfo* type = registry->FindStruct<Test::TestAllFields>();
			REQUIRE(type != nullptr);

			RandomValueGenerator generator(*registry, 1234);
			for (int i = 0; i < 200; ++i)
			{
				Test::TestAllFields object;
				generator.Randomize(*type, &object);
				ResolveContext resolve;
				resolve.Registry = registry.get();
				ValidationContext context;
				type->Validate(&object, resolve, context);
				CHECK_FALSE(context.HasErrors());
				CHECK(object.Scores.size() <= 8);
				CHECK(glm::abs(glm::length(object.Rotation) - 1.0f) < 1e-5f);
			}
		}

		TEST_CASE("RandomValueGenerator: Generate hooks make random objects pass the type-level validators" * doctest::skip(true))
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();
			const ComponentInfo* type = registry->FindComponent<Test::TestComponent>();
			REQUIRE(type != nullptr);
			REQUIRE(type->HasValidators());
			REQUIRE(type->HasGenerators());

			// Mass has Min 0, which allows the 0 that the sphere rule rejects; the hook rules it out.
			RandomValueGenerator generator(*registry, 4321);
			for (int i = 0; i < 200; ++i)
			{
				Test::TestComponent object;
				generator.Randomize(*type, &object);
				ResolveContext resolve;
				resolve.Registry = registry.get();
				ValidationContext context;
				type->Validate(&object, resolve, context);
				CHECK_FALSE(context.HasErrors());
				const Result<Json> json = type->ToJson(&object);
				REQUIRE(json.has_value());
				Test::TestComponent read;
				CHECK(type->FromJson(&read, JsonReader(*json), ReadContext{}).has_value());
			}
		}

		TEST_CASE("RandomValueGenerator: Variant maps draw their keys from the options and values from the resolved schema" * doctest::skip(true))
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			Test::RegisterReflectionTestTypes(*registry);
			registry->Freeze();
			const ComponentInfo* type = registry->FindComponent<Test::TestComponent>();
			REQUIRE(type != nullptr);
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			const std::vector<std::string> declared = schemas.GetFieldNames(Test::FixtureSchemaSource::DefaultOwner);
			REQUIRE_FALSE(declared.empty());

			RandomValueOptions options;
			options.Schemas = &schemas;
			options.VariantMapKeys = declared;
			RandomValueGenerator generator(*registry, 99, options);
			for (int i = 0; i < 50; ++i)
			{
				Test::TestComponent object;
				generator.Randomize(*type, &object);
				ResolveContext resolve;
				resolve.Registry = registry.get();
				resolve.Owner = &object;
				resolve.OwnerType = type;
				resolve.Schemas = &schemas;
				const FieldInfo* overrides = type->FindField("Overrides");
				REQUIRE(overrides != nullptr);
				for (const auto& [key, value] : object.Overrides)
				{
					CHECK(std::find(declared.begin(), declared.end(), key) != declared.end());
					resolve.Key = key;
					const Result<const FieldInfo*> schema = overrides->ResolveVariant(resolve);
					REQUIRE(schema.has_value());
					ValidationContext context;
					(*schema)->ValidateJson(JsonReader(value.Get()), resolve, context);
					CHECK_FALSE(context.HasErrors());
				}
			}
		}
	}

}
