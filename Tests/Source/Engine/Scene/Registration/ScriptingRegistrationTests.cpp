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
		TEST_CASE("ScriptingRegistration: Script has no shortcut and a Variant map of field overrides" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* script = registry->FindComponent("Script");
			REQUIRE(script != nullptr);
			CHECK(script->HasFlag(ComponentFlags::NoShortcut));
			CHECK_FALSE(script->HasFlag(ComponentFlags::EntityLevel));

			const FieldInfo* fields = script->FindField("Fields");
			REQUIRE(fields != nullptr);
			CHECK(fields->GetKind() == FieldType::Map);
			CHECK(fields->GetType().GetElement()->GetKind() == FieldType::Variant);
			CHECK(fields->GetResolver() != nullptr);
			CHECK(script->FindField("Script")->GetMeta().AssetFilter == "Script");
		}

		TEST_CASE("ScriptingRegistration: field overrides resolve against the script's schema" * doctest::skip(true))
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
	}

}
