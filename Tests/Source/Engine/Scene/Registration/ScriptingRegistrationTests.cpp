#include "TestsPCH.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("ScriptingRegistration: Script has no shortcut and a Variant map of field overrides")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* script = registry->FindComponent("Script");
			REQUIRE(script != nullptr);
			CHECK(script->HasFlag(ComponentFlags::NoShortcut));
			CHECK_FALSE(script->HasFlag(ComponentFlags::EntityLevel));
			CHECK(script->GetCategory() == "Scripting");

			const FieldInfo* fields = script->FindField("Fields");
			REQUIRE(fields != nullptr);
			CHECK(fields->GetKind() == FieldType::Map);
			CHECK(fields->GetType().GetElement()->GetKind() == FieldType::Variant);
			CHECK(fields->GetResolver() != nullptr);
			CHECK(script->FindField("Script")->GetMeta().AssetFilter == "Script");
			CHECK(script->FindField("ExecutionOrder")->GetKind() == FieldType::Int32);
		}

		TEST_CASE("ScriptingRegistration: field overrides resolve against the script's schema")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* script = registry->FindComponent("Script");
			REQUIRE(script != nullptr);
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();

			ScriptComponent component;
			component.Script = TypedAssetHandle<AssetType::Script>(Test::FixtureSchemaSource::DefaultOwner);
			ResolveContext resolve;
			resolve.Registry = registry.get();
			resolve.Owner = &component;
			resolve.OwnerType = script;
			resolve.Schemas = &schemas;

			resolve.Key = "Torque";
			const Result<const FieldInfo*> torque = script->FindField("Fields")->ResolveVariant(resolve);
			REQUIRE(torque.has_value());
			CHECK((*torque)->GetKind() == FieldType::Float);

			// A Script component object without a C++ object (an AddComponent prefab override): the handle comes from its JSON.
			const Result<Json> componentJson = JsonReader::Parse(R"({ "Script": "00000000c0ffee01", "Fields": {}, "ExecutionOrder": 0 })");
			REQUIRE(componentJson.has_value());
			const JsonReader componentReader(*componentJson);
			ResolveContext jsonOnly = resolve;
			jsonOnly.Owner = nullptr;
			jsonOnly.OwnerJson = &componentReader;
			const Result<const FieldInfo*> fromJson = script->FindField("Fields")->ResolveVariant(jsonOnly);
			REQUIRE(fromJson.has_value());
			CHECK(*fromJson == *torque);

			// The owner of a Field override is a PrefabOverride, not a ScriptComponent: no handle, so no schema.
			ResolveContext otherOwner = resolve;
			otherOwner.Owner = nullptr;
			otherOwner.OwnerType = registry->FindStruct("PrefabOverride");
			const Result<const FieldInfo*> rejected = script->FindField("Fields")->ResolveVariant(otherOwner);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);

			resolve.Key = "Missing";
			CHECK_FALSE(script->FindField("Fields")->ResolveVariant(resolve).has_value());

			resolve.Schemas = nullptr;
			resolve.Key = "Torque";
			CHECK_FALSE(script->FindField("Fields")->ResolveVariant(resolve).has_value());
		}

		TEST_CASE("ScriptingRegistration: overrides without an assigned script stay unresolved")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* script = registry->FindComponent("Script");
			REQUIRE(script != nullptr);
			const FieldInfo* fields = script->FindField("Fields");
			REQUIRE(fields != nullptr);
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();

			ResolveContext resolve;
			resolve.Registry = registry.get();
			resolve.OwnerType = script;
			resolve.Schemas = &schemas;
			resolve.Key = "Torque";

			const ScriptComponent unassigned;
			resolve.Owner = &unassigned;
			const Result<const FieldInfo*> fromObject = fields->ResolveVariant(resolve);
			REQUIRE_FALSE(fromObject.has_value());
			CHECK(fromObject.error().GetCode() == ErrorCode::NotFound);

			// A null or absent "Script" member is no script; a malformed one is the reader's located error.
			resolve.Owner = nullptr;
			for (const std::string_view text : { R"({ "Script": null })", R"({ "Fields": {} })", R"({ "Script": "c0ffee" })" })
			{
				INFO(std::string(text));
				const Result<Json> document = JsonReader::Parse(text);
				REQUIRE(document.has_value());
				const JsonReader reader(*document);
				resolve.OwnerJson = &reader;
				const Result<const FieldInfo*> fromJson = fields->ResolveVariant(resolve);
				REQUIRE_FALSE(fromJson.has_value());
				CHECK(fromJson.error().GetCode() == (text.find("c0ffee") != std::string_view::npos ? ErrorCode::Validation : ErrorCode::NotFound));
			}

			resolve.OwnerJson = nullptr;
			const Result<const FieldInfo*> noOwner = fields->ResolveVariant(resolve);
			REQUIRE_FALSE(noOwner.has_value());
			CHECK(noOwner.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
