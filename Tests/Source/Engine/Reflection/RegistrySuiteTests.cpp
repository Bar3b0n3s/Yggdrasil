#include "TestsPCH.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/RandomValueGenerator.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/GlmApprox.h"
#include "Support/ReflectionTestTypes.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <limits>

// The registry-parametrized suite of Architecture §5.4 and Roadmap M3: for every registered component and reflected
// struct, defaults round-trip, 200 seeded random values round-trip (Map and Variant fields included, through the fixture
// schema source), out-of-range and non-finite values are rejected, and the generated schema validates the serialized
// output. Script get/set (M13) and automation set/get (M4) join the suite with their milestones. The suite runs over the
// built-in registry and over the Reflection test types, which cover every FieldType independently of the components.

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

	static RandomValueOptions MakeSuiteOptions(const Test::FixtureSchemaSource& schemas)
	{
		RandomValueOptions options;
		options.Schemas = &schemas;
		options.VariantMapKeys = schemas.GetFieldNames(Test::FixtureSchemaSource::DefaultOwner);
		return options;
	}

	static void CheckDefaultsRoundTrip(const TypeRegistry& registry)
	{
		for (const StructInfo* type : GetAllReflectedStructs(registry))
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

	// True when `type` is a Variant or holds one (in containers or nested structs).
	static bool ContainsVariant(const TypeInfo& type)
	{
		if (type.GetKind() == FieldType::Variant)
			return true;
		if (type.GetElement() != nullptr)
			return ContainsVariant(*type.GetElement());
		if (type.GetStruct() == nullptr)
			return false;
		const std::span<const Scope<FieldInfo>> fields = type.GetStruct()->GetFields();
		return std::any_of(fields.begin(), fields.end(), [](const Scope<FieldInfo>& field)
		{
			return field->IsStored() && ContainsVariant(field->GetType());
		});
	}

	// True when `type` is PrefabOverride or holds one (the Prefab component): their random values name random components
	// and fields, which never resolve (ADR 0006 decision 23); CheckPrefabOverrideTargets covers real targets.
	static bool HasUnresolvableRandomTargets(const TypeInfo& type)
	{
		if (type.GetKey() == TypeKeyOf<PrefabOverride>())
			return true;
		if (type.GetElement() != nullptr)
			return HasUnresolvableRandomTargets(*type.GetElement());
		if (type.GetStruct() == nullptr)
			return false;
		const std::span<const Scope<FieldInfo>> fields = type.GetStruct()->GetFields();
		return std::any_of(fields.begin(), fields.end(), [](const Scope<FieldInfo>& field)
		{
			return field->IsStored() && HasUnresolvableRandomTargets(field->GetType());
		});
	}

	// Whether every Variant value in `json`, a value of `type`, has a schema to resolve against: not when random values
	// name random override targets, nor for a Script component without a script, whose Fields have no schema (§11.2).
	static bool CanResolveEveryVariant(const StructInfo& type, const Json& json)
	{
		if (HasUnresolvableRandomTargets(type.GetType()))
			return false;
		if (type.GetType().GetKey() != TypeKeyOf<ScriptComponent>())
			return true;
		const auto script = json.find("Script");
		return script != json.end() && !script->is_null();
	}

	static void CheckRandomRoundTrips(const TypeRegistry& registry, int iterations)
	{
		const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
		ReadContext context;
		context.Schemas = &schemas;

		for (const StructInfo* type : GetAllReflectedStructs(registry))
		{
			INFO(type->GetName());
			// Field metadata alone cannot satisfy a type-level rule (ProjectSettings' layers, SpotLight's cone angles): the
			// type's Generate hook repairs the random object (asserted by Freeze, checked here for the message).
			CHECK((!type->HasValidators() || type->HasGenerators()));
			RandomValueGenerator generator(registry, 0x5eed, MakeSuiteOptions(schemas));
			for (int iteration = 0; iteration < iterations; ++iteration)
			{
				const ObjectPtr object = type->CreateDefault();
				REQUIRE(object != nullptr);
				generator.Randomize(*type, object.get());

				ResolveContext resolve;
				resolve.Registry = &registry;
				resolve.Schemas = &schemas;
				ValidationContext validation;
				type->Validate(object.get(), resolve, validation);
				CHECK_FALSE(validation.HasErrors());

				const Result<Json> json = type->ToJson(object.get());
				REQUIRE(json.has_value());
				std::vector<ValidationIssue> diagnostics;
				ReadContext observed = context;
				observed.Diagnostics = &diagnostics;
				CHECK(RoundTrip(*type, *json, observed) == WriteCanonical(*json));
				if (CanResolveEveryVariant(*type, *json))
				{
					for (const ValidationIssue& issue : diagnostics)
					{
						INFO(issue.JsonPointer, ": ", issue.Message);
						CHECK(issue.Code != VariantUnresolvedCode);
						CHECK(issue.Code != VariantSchemaMismatchCode);
					}
				}
			}
		}
	}

	// Components that prefab overrides can target, as PrefabInstantiator applies them.
	static std::vector<const ComponentInfo*> GetOverridableComponents(const TypeRegistry& registry)
	{
		std::vector<const ComponentInfo*> components;
		for (const ComponentInfo* component : registry.GetComponents())
		{
			if (component->HasFlag(ComponentFlags::Serializable) && !component->HasFlag(ComponentFlags::EntityLevel)
				&& !component->HasFlag(ComponentFlags::Hidden))
				components.push_back(component);
		}
		return components;
	}

	// One PrefabOverride's JSON in the file spelling.
	static Json MakeOverrideJson(std::string_view kind, std::string_view component, std::string_view field, Json value)
	{
		Json json = Json::object();
		json["PrefabEntityID"] = "00000000000b0002";
		json["Kind"] = std::string(kind);
		json["Component"] = std::string(component);
		json["Field"] = std::string(field);
		json["Value"] = std::move(value);
		return json;
	}

	// PrefabOverride.Value is resolved by the target component and field, or the whole component struct (§5.4). The
	// random round trip draws random target names, which never resolve, so every overridable component and each of its
	// serialized fields is a target here, with random values drawn from the resolved schema: each must resolve, be checked
	// against that schema and round-trip without a Variant diagnostic, and a value the schema rejects must be reported.
	static void CheckPrefabOverrideTargets(const TypeRegistry& registry, int iterations)
	{
		const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
		const StructInfo* overrideType = registry.FindStruct<PrefabOverride>();
		REQUIRE(overrideType != nullptr);
		RandomValueGenerator generator(registry, 0x7a46, MakeSuiteOptions(schemas));
		const auto roundTrips = [&](const Json& json, bool resolvesWithoutOwner)
		{
			std::vector<ValidationIssue> diagnostics;
			ReadContext context;
			context.Schemas = &schemas;
			context.Diagnostics = &diagnostics;
			CHECK(RoundTrip(*overrideType, json, context) == WriteCanonical(json));
			if (!resolvesWithoutOwner)
				return; // values whose own schema needs the member's component (ADR 0006 decision 6)
			for (const ValidationIssue& issue : diagnostics)
			{
				INFO(issue.JsonPointer, ": ", issue.Message);
				CHECK(issue.Code != VariantUnresolvedCode);
				CHECK(issue.Code != VariantSchemaMismatchCode);
			}
		};

		for (const ComponentInfo* component : GetOverridableComponents(registry))
		{
			INFO(component->GetName());
			ResolveContext owner;
			owner.Registry = &registry;
			owner.OwnerType = component;
			owner.Schemas = &schemas;
			for (int iteration = 0; iteration < iterations; ++iteration)
			{
				const Json added = generator.RandomJson(component->GetSelfField(), owner);
				roundTrips(MakeOverrideJson("AddComponent", component->GetName(), "", added), CanResolveEveryVariant(*component, added));
				for (const Scope<FieldInfo>& field : component->GetFields())
				{
					if (!field->IsStored() || !field->GetMeta().Serialized)
						continue;
					INFO(field->GetName());
					// A Field override of a Map of Variants (Script.Fields) resolves without the member's ScriptComponent, so
					// its values stay unresolved until PrefabInstantiator applies them.
					const bool resolvesWithoutOwner = !ContainsVariant(field->GetType());
					const Json value = generator.RandomJson(*field, owner);
					roundTrips(MakeOverrideJson("Field", component->GetName(), field->GetName(), value), resolvesWithoutOwner);
				}
			}
			roundTrips(MakeOverrideJson("RemoveComponent", component->GetName(), "", Json()), true);
		}

		// The value is checked against the target's schema, not taken as free-form JSON.
		std::vector<ValidationIssue> diagnostics;
		ReadContext context;
		context.Diagnostics = &diagnostics;
		PrefabOverride read;
		const Json flat = MakeOverrideJson("Field", "Transform", "Scale", Json::array({ 1, 0, 1 }));
		REQUIRE(overrideType->FromJson(&read, JsonReader(flat), context).has_value());
		REQUIRE(diagnostics.size() == 1);
		CHECK(diagnostics[0].Code == VariantSchemaMismatchCode);
		CHECK(diagnostics[0].JsonPointer == "/Value/1");
	}

	// The number of float components of a Float, vector or colour kind; 0 for every other kind.
	static size_t GetFloatComponentCount(FieldType kind)
	{
		switch (kind)
		{
			case FieldType::Float:
				return 1;
			case FieldType::Vec2:
				return 2;
			case FieldType::Vec3:
			case FieldType::Color3:
				return 3;
			case FieldType::Vec4:
			case FieldType::Color4:
				return 4;
			case FieldType::Bool:
			case FieldType::Int32:
			case FieldType::UInt32:
			case FieldType::Quat:
			case FieldType::Bool3:
			case FieldType::String:
			case FieldType::EntityRef:
			case FieldType::AssetRef:
			case FieldType::Enum:
			case FieldType::Array:
			case FieldType::Struct:
			case FieldType::Map:
			case FieldType::Variant:
				return 0;
		}
		FAIL("unknown FieldType ", static_cast<int>(kind));
		return 0;
	}

	// The components of `value`, a Float, vector or colour Value, widened to a vec4 (unused components 0).
	static glm::vec4 GetFloatComponents(const Value& value)
	{
		REQUIRE(GetFloatComponentCount(value.GetKind()) != 0);
		if (value.GetKind() == FieldType::Float)
			return glm::vec4(value.AsFloat(), 0.0f, 0.0f, 0.0f);
		if (value.GetKind() == FieldType::Vec2)
			return glm::vec4(value.AsVec2(), 0.0f, 0.0f);
		if (value.GetKind() == FieldType::Vec3 || value.GetKind() == FieldType::Color3)
			return glm::vec4(value.AsVec3(), 0.0f);
		return value.AsVec4();
	}

	// A Value of the Float, vector or colour kind `kind` made of the first components of `components`.
	static Value MakeFloatValue(FieldType kind, const glm::vec4& components)
	{
		REQUIRE(GetFloatComponentCount(kind) != 0);
		if (kind == FieldType::Float)
			return Value::FromFloat(components.x);
		if (kind == FieldType::Vec2)
			return Value::FromVec2(glm::vec2(components));
		if (kind == FieldType::Vec3)
			return Value::FromVec3(glm::vec3(components));
		if (kind == FieldType::Color3)
			return Value::FromColor3(glm::vec3(components));
		if (kind == FieldType::Vec4)
			return Value::FromVec4(components);
		return Value::FromColor4(components);
	}

	// Writes through the reflected setter must reject every value outside the field's metadata (§5.4 "Min/Max/MinMagnitude
	// are enforced on every write path"; non-finite floats; enum values without a name; non-unit quaternions). Each float
	// check starts from the field's valid default and breaks one component, so every component is held to the rule.
	static void CheckRejectsInvalidValues(const TypeRegistry& registry)
	{
		for (const StructInfo* type : GetAllReflectedStructs(registry))
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
				const FieldType kind = field->GetKind();
				// A rejection for any other reason (an owner the setter needs, a resolver failure) would hide a missing rule.
				const auto rejects = [&](const Value& value)
				{
					INFO(value.ToString());
					const Status status = field->SetValue(context, value);
					REQUIRE_FALSE(status.has_value());
					INFO(status.error().ToString());
					CHECK(status.error().GetCode() == ErrorCode::Validation);
				};

				if (const size_t count = GetFloatComponentCount(kind); count != 0)
				{
					const glm::vec4 valid = GetFloatComponents(field->GetValue(context));
					for (size_t component = 0; component < count; ++component)
					{
						const auto index = static_cast<glm::length_t>(component);
						const auto withComponent = [&](float broken)
						{
							glm::vec4 components = valid;
							components[index] = broken;
							return MakeFloatValue(kind, components);
						};
						rejects(withComponent(std::numeric_limits<float>::quiet_NaN()));
						rejects(withComponent(std::numeric_limits<float>::infinity()));
						rejects(withComponent(-std::numeric_limits<float>::infinity()));
						if (meta.MinMagnitude)
							rejects(withComponent(0.0f));
						if (meta.Min)
							rejects(withComponent(static_cast<float>(*meta.Min) - 1.0f));
						if (meta.Max)
							rejects(withComponent(static_cast<float>(*meta.Max) + 1.0f));
					}
				}
				else if (kind == FieldType::Int32)
				{
					if (meta.Min && *meta.Min > static_cast<double>(std::numeric_limits<int32_t>::min()))
						rejects(Value::FromInt32(static_cast<int32_t>(*meta.Min) - 1));
					if (meta.Max && *meta.Max < static_cast<double>(std::numeric_limits<int32_t>::max()))
						rejects(Value::FromInt32(static_cast<int32_t>(*meta.Max) + 1));
				}
				else if (kind == FieldType::UInt32)
				{
					if (meta.Min && *meta.Min > 0.0)
						rejects(Value::FromUInt32(static_cast<uint32_t>(*meta.Min) - 1u));
					if (meta.Max && *meta.Max < static_cast<double>(std::numeric_limits<uint32_t>::max()))
						rejects(Value::FromUInt32(static_cast<uint32_t>(*meta.Max) + 1u));
				}
				else if (kind == FieldType::Quat)
				{
					rejects(Value::FromQuat(glm::quat(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f)));
					rejects(Value::FromQuat(glm::quat(0.0f, 0.0f, 0.0f, 0.0f)));
					rejects(Value::FromQuat(glm::quat(2.0f, 0.0f, 0.0f, 0.0f)));
				}
				else if (kind == FieldType::Enum)
				{
					rejects(Value::FromEnum(1000));
				}
				else if ((kind == FieldType::Array || kind == FieldType::Map) && field->GetType().GetElement() != nullptr)
				{
					// Containers of a float kind hold every element to finite values; one broken component of one element
					// is enough to reject the write.
					const FieldType elementKind = field->GetType().GetElement()->GetKind();
					const size_t elementCount = GetFloatComponentCount(elementKind);
					for (size_t component = 0; component < elementCount; ++component)
					{
						for (const float broken : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
						{
							glm::vec4 components(0.0f);
							components[static_cast<glm::length_t>(component)] = broken;
							const Value valid = MakeFloatValue(elementKind, glm::vec4(0.0f));
							const Value invalid = MakeFloatValue(elementKind, components);
							if (kind == FieldType::Array)
								rejects(Value::FromArray({ valid, invalid }));
							else
								rejects(Value::FromMap({ "a", "b" }, { valid, invalid }));
						}
					}
				}

				// A rejected write changes nothing.
				const Result<Json> after = type->ToJson(object.get());
				REQUIRE(after.has_value());
				CHECK(*after == type->MakeDefaultJson());
			}
		}
	}

	static void CheckSchemaValidatesOutput(const TypeRegistry& registry, int iterations)
	{
		const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
		for (const StructInfo* type : GetAllReflectedStructs(registry))
		{
			INFO(type->GetName());
			const Json schema = JsonSchema::ForStruct(*type);
			CHECK(JsonSchema::Validate(schema, type->MakeDefaultJson()).has_value());

			RandomValueGenerator generator(registry, 0xc0de, MakeSuiteOptions(schemas));
			for (int iteration = 0; iteration < iterations; ++iteration)
			{
				const ObjectPtr object = type->CreateDefault();
				REQUIRE(object != nullptr);
				generator.Randomize(*type, object.get());
				const Result<Json> json = type->ToJson(object.get());
				REQUIRE(json.has_value());
				const Status valid = JsonSchema::Validate(schema, *json);
				INFO((valid.has_value() ? std::string() : valid.error().ToString()));
				CHECK(valid.has_value());
			}
		}
	}

	static Scope<TypeRegistry> CreateReflectionTestRegistry()
	{
		Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
		Test::RegisterReflectionTestTypes(*registry);
		registry->Freeze();
		return registry;
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("TypeRegistry: every built-in component and reflected struct round-trips its defaults")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			REQUIRE(registry->AreComponentsRegistered(BuiltinComponents{}));
			REQUIRE(registry->GetComponents().size() == BuiltinComponents::Size);
			CheckDefaultsRoundTrip(*registry);
		}

		TEST_CASE("TypeRegistry: 200 seeded random values round-trip for every component and reflected struct")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CheckRandomRoundTrips(*registry, 200);
		}

		TEST_CASE("TypeRegistry: prefab override values resolve against every overridable component and field")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CheckPrefabOverrideTargets(*registry, 20);
		}

		TEST_CASE("TypeRegistry: validation rejects out-of-range and non-finite values for every field")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CheckRejectsInvalidValues(*registry);
		}

		TEST_CASE("TypeRegistry: the generated schema validates the serialized output of every type")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CheckSchemaValidatesOutput(*registry, 20);

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

		TEST_CASE("TypeRegistry: the registry suite passes for the reflection test types")
		{
			const Scope<TypeRegistry> registry = CreateReflectionTestRegistry();
			CheckDefaultsRoundTrip(*registry);
			CheckRandomRoundTrips(*registry, 200);
			CheckRejectsInvalidValues(*registry);
			CheckSchemaValidatesOutput(*registry, 20);
		}
	}

}
