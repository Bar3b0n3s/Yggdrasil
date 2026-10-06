#include "TestsPCH.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Engine/Scene/Entity.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static const ComponentInfo& FindCoreComponent(const TypeRegistry& registry, std::string_view name)
	{
		const ComponentInfo* info = registry.FindComponent(name);
		REQUIRE(info != nullptr);
		return *info;
	}

	static const StructInfo& FindCoreStruct(const TypeRegistry& registry, std::string_view name)
	{
		const StructInfo* info = registry.FindStruct(name);
		REQUIRE(info != nullptr);
		return *info;
	}

	// The JSON pointers of the errors a validation of `object` (of `type`) reports.
	static std::vector<std::string> ValidateCore(const TypeRegistry& registry, const StructInfo& type, const void* object)
	{
		ResolveContext resolve;
		resolve.Registry = &registry;
		ValidationContext context;
		type.Validate(object, resolve, context);
		std::vector<std::string> pointers;
		for (const ValidationIssue& issue : context.GetIssues())
		{
			if (issue.Severity == DiagnosticSeverity::Error)
				pointers.push_back(issue.JsonPointer);
		}
		return pointers;
	}

	// The code of the error resolving `field`'s Variant value in `context` reports (the resolution must fail).
	static ErrorCode ResolveFailure(const FieldInfo& field, const ResolveContext& context)
	{
		const Result<const FieldInfo*> resolved = field.ResolveVariant(context);
		REQUIRE_FALSE(resolved.has_value());
		return resolved.error().GetCode();
	}

	// The code of the error `status` reports (it must have failed).
	static ErrorCode GetErrorCodeOf(const Status& status)
	{
		REQUIRE_FALSE(status.has_value());
		return status.error().GetCode();
	}

	static PrefabOverride MakeOverride(PrefabOverrideKind kind, std::string component, std::string field, Json value = Json())
	{
		PrefabOverride prefabOverride;
		prefabOverride.PrefabEntityID = UUID(0x00000000000b0002);
		prefabOverride.Kind = kind;
		prefabOverride.Component = std::move(component);
		prefabOverride.Field = std::move(field);
		prefabOverride.Value = VariantValue(std::move(value));
		return prefabOverride;
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("CoreRegistration: entity-level, required and hidden flags follow the component table")
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
			CHECK(relationship.FindField("Parent")->IsReadOnly());
			CHECK(relationship.FindField("Children")->IsReadOnly());
			CHECK(relationship.FindField("Parent")->GetMeta().Serialized);
			CHECK_FALSE(relationship.FindField("Children")->GetMeta().Serialized); // rebuilt from the parents on load (§5.2)

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
			CHECK(FindCoreComponent(*registry, "Prefab").FindField("Prefab")->GetMeta().AssetFilter == "Prefab");

			const StructInfo* entityKeys = registry->FindStruct("PrefabEntityKeys");
			REQUIRE(entityKeys != nullptr);
			CHECK(entityKeys->FindField("Active")->GetKind() == FieldType::Bool);

			const EnumInfo* kinds = registry->FindEnum("PrefabOverrideKind");
			REQUIRE(kinds != nullptr);
			std::vector<std::string> kindNames;
			for (const EnumEntry& entry : kinds->GetEntries())
				kindNames.push_back(entry.Name);
			CHECK(kindNames == std::vector<std::string>{ "Field", "AddComponent", "RemoveComponent", "EntityKey" });
		}

		TEST_CASE("CoreRegistration: Transform has the virtual world and render fields")
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
			CHECK(transform.FindField("WorldPosition")->GetKind() == FieldType::Vec3);
			CHECK(transform.FindField("WorldRotation")->GetKind() == FieldType::Quat);
			CHECK(transform.FindField("Translation")->GetMeta().Unit == "m");
		}

		TEST_CASE("CoreRegistration: Transform virtual fields treat an object outside a scene as a root")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& transform = FindCoreComponent(*registry, "Transform");
			TransformComponent component;
			component.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			component.Scale = glm::vec3(2.0f, 3.0f, 4.0f);
			const FieldContext context{ &component, nullptr };

			CHECK(transform.FindField("WorldPosition")->GetValue(context).AsVec3() == component.Translation);
			CHECK(transform.FindField("RenderPosition")->GetValue(context).AsVec3() == component.Translation);
			CHECK(transform.FindField("WorldScale")->GetValue(context).AsVec3() == component.Scale);
			CHECK(Test::ApproxEqual(transform.FindField("WorldRotation")->GetValue(context).AsQuat(), component.Rotation));
			CHECK(Test::ApproxEqual(transform.FindField("RenderRotation")->GetValue(context).AsQuat(), component.Rotation));

			REQUIRE(transform.FindField("WorldPosition")->SetValue(context, Value::FromVec3(glm::vec3(-4.0f, 5.0f, 6.0f))).has_value());
			CHECK(component.Translation == glm::vec3(-4.0f, 5.0f, 6.0f));

			// A written world rotation within the unit tolerance (1e-3) is stored normalized; a quaternion that is not a unit
			// one, the zero quaternion included, is rejected and changes nothing.
			REQUIRE(transform.FindField("WorldRotation")->SetValue(context, Value::FromQuat(glm::quat(0.0f, 0.0f, 1.0005f, 0.0f))).has_value());
			CHECK(Test::ApproxEqual(component.Rotation, glm::quat(0.0f, 0.0f, 1.0f, 0.0f), 1e-6f));
			const glm::quat stored = component.Rotation;
			CHECK_FALSE(transform.FindField("WorldRotation")->SetValue(context, Value::FromQuat(glm::quat(0.0f, 0.0f, 2.0f, 0.0f))).has_value());
			CHECK_FALSE(transform.FindField("WorldRotation")->SetValue(context, Value::FromQuat(glm::quat(0.0f, 0.0f, 0.0f, 0.0f))).has_value());
			CHECK(component.Rotation == stored);

			// 90 degrees about +Y.
			REQUIRE(transform.FindField("EulerAngles")->SetValue(context, Value::FromVec3(glm::vec3(0.0f, 90.0f, 0.0f))).has_value());
			const float halfRoot = 0.70710678f;
			CHECK(Test::ApproxEqual(component.Rotation, glm::quat(halfRoot, 0.0f, halfRoot, 0.0f)));
			CHECK(Test::ApproxEqual(transform.FindField("EulerAngles")->GetValue(context).AsVec3(), glm::vec3(0.0f, 90.0f, 0.0f), 1e-4f));

			CHECK_FALSE(transform.FindField("WorldScale")->SetValue(context, Value::FromVec3(glm::vec3(1.0f))).has_value());
			CHECK(component.Scale == glm::vec3(2.0f, 3.0f, 4.0f));
		}

		TEST_CASE("CoreRegistration: a world position whose local translation is not finite is rejected")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity parent = scene.CreateEntity("Parent");
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(-3e38f, 0.0f, 0.0f);
			});
			const Entity child = scene.CreateEntity("Child", parent);
			const uint64_t revision = scene.GetRevision();

			// Finite, so the field's own checks pass, but 6e38 metres from the parent: beyond float.
			const Value farAway = Value::FromVec3(glm::vec3(3e38f, 0.0f, 0.0f));
			const Status overflow = ComponentAccess::SetFieldValue(child, "Transform", "WorldPosition", farAway);
			REQUIRE_FALSE(overflow.has_value());
			CHECK(overflow.error().GetCode() == ErrorCode::Validation);
			CHECK(child.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
			CHECK(scene.GetRevision() == revision);

			// So is a position that a strongly shrinking parent chain magnifies beyond float.
			const Entity flat = scene.CreateEntity("Flat");
			flat.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1e-4f);
			});
			const Entity flatter = scene.CreateEntity("Flatter", flat);
			flatter.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1e-4f);
			});
			const Entity deepest = scene.CreateEntity("Deepest", flatter);
			deepest.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1e-4f);
			});
			const Entity leaf = scene.CreateEntity("Leaf", deepest);
			const Value magnified = Value::FromVec3(glm::vec3(1e30f, 0.0f, 0.0f));
			CHECK(GetErrorCodeOf(ComponentAccess::SetFieldValue(leaf, "Transform", "WorldPosition", magnified)) == ErrorCode::Validation);
			CHECK(leaf.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));

			// A representable position still moves the entity through its parent.
			REQUIRE(ComponentAccess::SetFieldValue(child, "Transform", "WorldPosition", Value::FromVec3(glm::vec3(0.0f))).has_value());
			CHECK(child.GetComponent<TransformComponent>().Translation == glm::vec3(3e38f, 0.0f, 0.0f));
		}

		TEST_CASE("CoreRegistration: prefab overrides cannot target the engine-maintained prefab components")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StructInfo* overrideType = registry->FindStruct<PrefabOverride>();
			REQUIRE(overrideType != nullptr);
			const FieldInfo* value = overrideType->FindField("Value");
			REQUIRE(value != nullptr);

			const std::array<std::string, 2> hidden = { "Prefab", "PrefabLink" };
			for (const std::string& component : hidden)
			{
				CAPTURE(component);
				for (const PrefabOverrideKind kind : { PrefabOverrideKind::AddComponent, PrefabOverrideKind::RemoveComponent })
				{
					const PrefabOverride target = MakeOverride(kind, component, "");
					ResolveContext resolve;
					resolve.Registry = registry.get();
					resolve.Owner = &target;
					resolve.OwnerType = overrideType;
					CHECK(ResolveFailure(*value, resolve) == ErrorCode::NotFound);
				}
				const PrefabOverride field = MakeOverride(PrefabOverrideKind::Field, component, component == "Prefab" ? "Overrides" : "InstanceRoot");
				ResolveContext resolve;
				resolve.Registry = registry.get();
				resolve.Owner = &field;
				resolve.OwnerType = overrideType;
				CHECK(ResolveFailure(*value, resolve) == ErrorCode::NotFound);
			}

			// So an override's Value never holds a prefab instance: it is kept unresolved instead of being read as one.
			const Result<Json> json = JsonReader::Parse(R"({ "PrefabEntityID": "00000000000b0002", "Kind": "AddComponent", "Component": "Prefab",
				"Field": "", "Value": { "Prefab": null, "Overrides": [ { "Kind": "NotAKind" } ] } })");
			REQUIRE(json.has_value());
			std::vector<ValidationIssue> diagnostics;
			ReadContext context;
			context.Diagnostics = &diagnostics;
			PrefabOverride read;
			REQUIRE(overrideType->FromJson(&read, JsonReader(*json), context).has_value());
			REQUIRE(diagnostics.size() == 1);
			CHECK(diagnostics[0].Code == VariantUnresolvedCode);
			CHECK(diagnostics[0].JsonPointer == "/Value");
			CHECK(read.Value.Get() == (*json)["Value"]);
		}

		TEST_CASE("CoreRegistration: tags are non-empty and unique")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& tagsType = FindCoreComponent(*registry, "Tags");

			TagsComponent tags;
			tags.Tags = { "Player", "Goal" };
			CHECK(ValidateCore(*registry, tagsType, &tags).empty());

			tags.Tags = { "Player", "", "Goal", "Player" };
			CHECK(ValidateCore(*registry, tagsType, &tags) == std::vector<std::string>{ "/Tags/1", "/Tags/3" });

			// The entity keys of prefab overrides follow the same rule.
			const StructInfo& keysType = FindCoreStruct(*registry, "PrefabEntityKeys");
			PrefabEntityKeys keys;
			keys.Tags = { "A", "A" };
			CHECK(ValidateCore(*registry, keysType, &keys) == std::vector<std::string>{ "/Tags/1" });

			// The Generate hooks repair random lists without reordering them.
			Random random(7);
			tags.Tags = { "", "Player", "Player", "", "Player_1" };
			tagsType.Generate(&tags, random);
			CHECK(tags.Tags == std::vector<std::string>{ "Tag", "Player", "Player_1", "Tag_1", "Player_1_1" });
			CHECK(ValidateCore(*registry, tagsType, &tags).empty());
			keysType.Generate(&keys, random);
			CHECK(keys.Tags == std::vector<std::string>{ "A", "A_1" });
		}

		TEST_CASE("CoreRegistration: prefab overrides use Component, Field and Value as their kind requires")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StructInfo& overrideType = FindCoreStruct(*registry, "PrefabOverride");

			const PrefabOverride field = MakeOverride(PrefabOverrideKind::Field, "Transform", "Scale", Json::array({ 1, 2, 1 }));
			CHECK(ValidateCore(*registry, overrideType, &field).empty());
			const PrefabOverride added = MakeOverride(PrefabOverrideKind::AddComponent, "AudioListener", "", Json::object({ { "Primary", false } }));
			CHECK(ValidateCore(*registry, overrideType, &added).empty());
			const PrefabOverride removed = MakeOverride(PrefabOverrideKind::RemoveComponent, "MeshRenderer", "");
			CHECK(ValidateCore(*registry, overrideType, &removed).empty());
			const PrefabOverride key = MakeOverride(PrefabOverrideKind::EntityKey, "", "Active", Json(false));
			CHECK(ValidateCore(*registry, overrideType, &key).empty());

			const PrefabOverride addedField = MakeOverride(PrefabOverrideKind::AddComponent, "AudioListener", "Primary",
				Json::object({ { "Primary", false } }));
			CHECK(ValidateCore(*registry, overrideType, &addedField) == std::vector<std::string>{ "/Field" });
			const PrefabOverride removedValue = MakeOverride(PrefabOverrideKind::RemoveComponent, "MeshRenderer", "", Json(true));
			CHECK(ValidateCore(*registry, overrideType, &removedValue) == std::vector<std::string>{ "/Value" });
			const PrefabOverride keyComponent = MakeOverride(PrefabOverrideKind::EntityKey, "Name", "Name", Json("Box"));
			CHECK(ValidateCore(*registry, overrideType, &keyComponent) == std::vector<std::string>{ "/Component" });
			const PrefabOverride notAKey = MakeOverride(PrefabOverrideKind::EntityKey, "", "Parent", Json());
			CHECK(ValidateCore(*registry, overrideType, &notAKey) == std::vector<std::string>{ "/Field" });

			// Targets that do not exist are not type-level errors: a default override (no prefab entity, no names) and an override
			// of a vanished component are valid but unresolved, kept until PrefabInstantiator drops them at the next update.
			const PrefabOverride defaults;
			CHECK(ValidateCore(*registry, overrideType, &defaults).empty());
			PrefabOverride unkeyed = field;
			unkeyed.PrefabEntityID = UUID();
			CHECK(ValidateCore(*registry, overrideType, &unkeyed).empty());
			const PrefabOverride unnamed = MakeOverride(PrefabOverrideKind::Field, "", "", Json(1));
			CHECK(ValidateCore(*registry, overrideType, &unnamed).empty());
			const FieldInfo* value = overrideType.FindField("Value");
			REQUIRE(value != nullptr);
			ResolveContext resolve;
			resolve.Registry = registry.get();
			resolve.OwnerType = &overrideType;
			resolve.Owner = &unnamed;
			CHECK(ResolveFailure(*value, resolve) == ErrorCode::NotFound);
			const PrefabOverride removedUnknown = MakeOverride(PrefabOverrideKind::RemoveComponent, "Vanished", "");
			CHECK(ValidateCore(*registry, overrideType, &removedUnknown).empty());
			resolve.Owner = &removedUnknown;
			CHECK(ResolveFailure(*value, resolve) == ErrorCode::NotFound);
		}

		TEST_CASE("CoreRegistration: the prefab override Generate hook makes random overrides valid")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StructInfo& overrideType = FindCoreStruct(*registry, "PrefabOverride");
			Random random(11);

			// A Field override's names are free (an unresolvable target is not a type-level error), so the hook keeps them.
			PrefabOverride field = MakeOverride(PrefabOverrideKind::Field, "Camera", "VerticalFov", Json(3));
			overrideType.Generate(&field, random);
			CHECK(field.Component == "Camera");
			CHECK(field.Field == "VerticalFov");
			CHECK(field.Value.Get() == Json(3));

			PrefabOverride removed = MakeOverride(PrefabOverrideKind::RemoveComponent, "MeshRenderer", "Mass", Json(2));
			overrideType.Generate(&removed, random);
			CHECK(removed.Component == "MeshRenderer");
			CHECK(removed.Field.empty());
			CHECK(removed.Value.IsNull());

			PrefabOverride key = MakeOverride(PrefabOverrideKind::EntityKey, "Transform", "Scale", Json::array({ 1, 1, 1 }));
			overrideType.Generate(&key, random);
			CHECK(key.Component.empty());
			CHECK((key.Field == "Name" || key.Field == "Active" || key.Field == "Tags"));

			for (const PrefabOverride* repaired : { &field, &removed, &key })
			{
				ResolveContext resolve;
				resolve.Registry = registry.get();
				ValidationContext context;
				overrideType.Validate(repaired, resolve, context);
				CHECK_FALSE(context.HasErrors());
			}
		}

		TEST_CASE("CoreRegistration: prefab override values resolve by kind, component and field")
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

		TEST_CASE("CoreRegistration: prefab override values resolve from JSON and reject other owners")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const StructInfo* overrideType = registry->FindStruct<PrefabOverride>();
			REQUIRE(overrideType != nullptr);
			const FieldInfo* value = overrideType->FindField("Value");
			REQUIRE(value != nullptr);

			// Without a C++ object the kind and names come from the override's JSON, in the file spelling.
			const Result<Json> removedJson = JsonReader::Parse(
				R"({ "PrefabEntityID": "00000000000b0002", "Kind": "RemoveComponent", "Component": "MeshRenderer", "Field": "", "Value": null })");
			REQUIRE(removedJson.has_value());
			const JsonReader removedReader(*removedJson);
			ResolveContext resolve;
			resolve.Registry = registry.get();
			resolve.OwnerType = overrideType;
			resolve.OwnerJson = &removedReader;
			const Result<const FieldInfo*> removed = value->ResolveVariant(resolve);
			REQUIRE(removed.has_value());
			CHECK((*removed)->GetKind() == FieldType::Variant);
			ValidationContext nullValue;
			(*removed)->ValidateJson(JsonReader(Json()), resolve, nullValue);
			CHECK_FALSE(nullValue.HasErrors());

			// Absent members read as their defaults: Kind Field, empty names.
			const Result<Json> fieldJson = JsonReader::Parse(R"({ "Component": "Camera", "Field": "VerticalFov" })");
			REQUIRE(fieldJson.has_value());
			const JsonReader fieldReader(*fieldJson);
			resolve.OwnerJson = &fieldReader;
			const Result<const FieldInfo*> fov = value->ResolveVariant(resolve);
			REQUIRE(fov.has_value());
			CHECK((*fov)->GetName() == "VerticalFov");

			const Result<Json> badKindJson = JsonReader::Parse(R"({ "Kind": "Rename" })");
			REQUIRE(badKindJson.has_value());
			const JsonReader badKindReader(*badKindJson);
			resolve.OwnerJson = &badKindReader;
			CHECK_FALSE(value->ResolveVariant(resolve).has_value());

			// Entity-level components and virtual fields are not override targets; unknown names get suggestions.
			PrefabOverride target;
			resolve.OwnerJson = nullptr;
			resolve.Owner = &target;
			target.Component = "Name";
			target.Field = "Name";
			CHECK(ResolveFailure(*value, resolve) == ErrorCode::NotFound);
			target.Component = "Transform";
			target.Field = "WorldPosition";
			CHECK(ResolveFailure(*value, resolve) == ErrorCode::NotFound);
			target.Component = "Trnasform";
			target.Field = "Scale";
			const Result<const FieldInfo*> misspelled = value->ResolveVariant(resolve);
			REQUIRE_FALSE(misspelled.has_value());
			CHECK(misspelled.error().GetCode() == ErrorCode::NotFound);
			CHECK(misspelled.error().GetHint() == "did you mean 'Transform'?");

			// The resolver checks its owner type before reading the owner.
			ResolveContext otherOwner = resolve;
			otherOwner.OwnerType = registry->FindComponent("Script");
			CHECK(ResolveFailure(*value, otherOwner) == ErrorCode::InvalidArgument);
			ResolveContext noOwner;
			noOwner.Registry = registry.get();
			noOwner.OwnerType = overrideType;
			CHECK(ResolveFailure(*value, noOwner) == ErrorCode::InvalidArgument);
		}
	}

}
