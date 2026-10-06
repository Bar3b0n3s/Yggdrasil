#include "TestsPCH.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static const ComponentInfo& FindCoreComponent(const TypeRegistry& registry, std::string_view name)
	{
		const ComponentInfo* info = registry.FindComponent(name);
		REQUIRE(info != nullptr);
		return *info;
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("CoreRegistration: entity-level, required and hidden flags follow the component table" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentFlags entityLevel = ComponentFlags::EntityLevel;

			const ComponentInfo& id = FindCoreComponent(*registry, "ID");
			CHECK(id.HasFlag(ComponentFlags::Required | ComponentFlags::Hidden | entityLevel));
			CHECK_FALSE(id.HasFlag(ComponentFlags::Removable));
			CHECK(id.FindField("ID")->IsReadOnly());

			const ComponentInfo& name = FindCoreComponent(*registry, "Name");
			CHECK(name.HasFlag(ComponentFlags::Required | entityLevel));
			CHECK_FALSE(name.HasFlag(ComponentFlags::Hidden));

			const ComponentInfo& tags = FindCoreComponent(*registry, "Tags");
			CHECK(tags.HasFlag(entityLevel));
			CHECK_FALSE(tags.HasFlag(ComponentFlags::Removable));

			const ComponentInfo& relationship = FindCoreComponent(*registry, "Relationship");
			CHECK(relationship.HasFlag(ComponentFlags::Required | ComponentFlags::Hidden | entityLevel));

			const ComponentInfo& transform = FindCoreComponent(*registry, "Transform");
			CHECK(transform.HasFlag(ComponentFlags::Required));
			CHECK_FALSE(transform.HasFlag(entityLevel));
			CHECK_FALSE(transform.HasFlag(ComponentFlags::Removable));

			// Engine-maintained: only PrefabInstantiator adds and removes them.
			for (const std::string_view prefabComponent : { std::string_view("Prefab"), std::string_view("PrefabLink") })
			{
				INFO(std::string(prefabComponent));
				const ComponentInfo& info = FindCoreComponent(*registry, prefabComponent);
				CHECK(info.HasFlag(ComponentFlags::Hidden));
				CHECK_FALSE(info.HasFlag(ComponentFlags::EditorVisible));
				CHECK_FALSE(info.HasFlag(ComponentFlags::Removable));
			}

			const StructInfo* entityKeys = registry->FindStruct("PrefabEntityKeys");
			REQUIRE(entityKeys != nullptr);
			CHECK(entityKeys->FindField("Active")->GetKind() == FieldType::Bool);
		}

		TEST_CASE("CoreRegistration: Transform has the virtual world and render fields" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& transform = FindCoreComponent(*registry, "Transform");

			const FieldInfo* scale = transform.FindField("Scale");
			REQUIRE(scale != nullptr);
			REQUIRE(scale->GetMeta().MinMagnitude.has_value());
			CHECK(*scale->GetMeta().MinMagnitude == doctest::Approx(1e-4));

			struct Expected
			{
				std::string_view Name;
				bool ReadOnly = false;
			};
			const Expected virtualFields[] = {
				{ "EulerAngles", false },
				{ "WorldPosition", false },
				{ "WorldRotation", false },
				{ "WorldScale", true },
				{ "RenderPosition", true },
				{ "RenderRotation", true },
			};
			for (const Expected& expected : virtualFields)
			{
				INFO(std::string(expected.Name));
				const FieldInfo* field = transform.FindField(expected.Name);
				REQUIRE(field != nullptr);
				CHECK(field->IsVirtual());
				CHECK_FALSE(field->GetMeta().Serialized);
				CHECK(field->IsReadOnly() == expected.ReadOnly);
			}
			CHECK(transform.FindField("EulerAngles")->GetMeta().Unit == "deg");
		}

		TEST_CASE("CoreRegistration: prefab override values resolve by kind, component and field" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StructInfo* overrideType = registry->FindStruct<PrefabOverride>();
			REQUIRE(overrideType != nullptr);
			const FieldInfo* value = overrideType->FindField("Value");
			REQUIRE(value != nullptr);

			PrefabOverride fieldOverride;
			fieldOverride.Kind = PrefabOverrideKind::Field;
			fieldOverride.Component = "Transform";
			fieldOverride.Field = "Scale";
			ResolveContext resolve;
			resolve.Registry = registry.get();
			resolve.Owner = &fieldOverride;
			resolve.OwnerType = overrideType;
			const Result<const FieldInfo*> scale = value->ResolveVariant(resolve);
			REQUIRE(scale.has_value());
			CHECK((*scale)->GetName() == "Scale");
			CHECK((*scale)->GetKind() == FieldType::Vec3);

			PrefabOverride addComponent;
			addComponent.Kind = PrefabOverrideKind::AddComponent;
			addComponent.Component = "RigidBody";
			resolve.Owner = &addComponent;
			const Result<const FieldInfo*> body = value->ResolveVariant(resolve);
			REQUIRE(body.has_value());
			CHECK((*body)->GetKind() == FieldType::Struct);
			CHECK((*body)->GetType().GetStruct() == registry->FindComponent("RigidBody"));

			PrefabOverride entityKey;
			entityKey.Kind = PrefabOverrideKind::EntityKey;
			entityKey.Field = "Active";
			resolve.Owner = &entityKey;
			const Result<const FieldInfo*> active = value->ResolveVariant(resolve);
			REQUIRE(active.has_value());
			CHECK((*active)->GetKind() == FieldType::Bool);
			entityKey.Field = "Tags";
			const Result<const FieldInfo*> tags = value->ResolveVariant(resolve);
			REQUIRE(tags.has_value());
			CHECK((*tags)->GetKind() == FieldType::Array);

			PrefabOverride missing;
			missing.Component = "Transform";
			missing.Field = "Scael";
			resolve.Owner = &missing;
			const Result<const FieldInfo*> unresolved = value->ResolveVariant(resolve);
			REQUIRE_FALSE(unresolved.has_value());
			CHECK(unresolved.error().GetCode() == ErrorCode::NotFound);
		}
	}

}
