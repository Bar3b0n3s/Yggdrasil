#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Core category (Docs/Decisions/0006-m3-decisions.md, decision 9): the override types of prefab instances, then the
// entity-level components (ID, Name, Tags, Relationship), Transform with its virtual world and render fields, and the
// engine-maintained prefab components.

namespace Engine {

	namespace Utils {

		// The entity keys a PrefabOverride of kind EntityKey can name (the field names of PrefabEntityKeys).
		constexpr std::string_view EntityKeyNames[] = { "Name", "Active", "Tags" };

		// Tags are non-empty and unique per entity (TagsComponent): Entity::AddTag asserts against an empty tag, so files
		// and automation must never produce one. Issues are located at <current>/Tags/<index>.
		static void ValidateTagList(const std::vector<std::string>& tags, ValidationContext& context)
		{
			context.PushKey("Tags");
			for (auto tag = tags.begin(); tag != tags.end(); ++tag)
			{
				const std::string index = std::to_string(tag - tags.begin());
				if (tag->empty())
					context.Error(index, "must not be empty");
				else if (std::find(tags.begin(), tag, *tag) != tag)
					context.Error(index, std::format("duplicates the tag '{}' (tags are unique per entity)", *tag));
			}
			context.PopKey();
		}

		// Turns random tags into a valid list: an empty tag becomes "Tag", and a repeated tag gets the smallest "_<n>"
		// suffix that makes it unique. Order is kept.
		static void MakeTagListValid(std::vector<std::string>& tags)
		{
			std::vector<std::string> valid;
			valid.reserve(tags.size());
			for (const std::string& tag : tags)
			{
				const std::string base = tag.empty() ? std::string("Tag") : tag;
				std::string candidate = base;
				for (uint32_t suffix = 1; std::find(valid.begin(), valid.end(), candidate) != valid.end(); ++suffix)
					candidate = std::format("{}_{}", base, suffix);
				valid.push_back(std::move(candidate));
			}
			tags = std::move(valid);
		}

		static void ValidateTags(const TagsComponent& tags, ValidationContext& context)
		{
			ValidateTagList(tags.Tags, context);
		}

		static void MakeTagsValid(TagsComponent& tags, Random& /*random*/)
		{
			MakeTagListValid(tags.Tags);
		}

		static void ValidateEntityKeys(const PrefabEntityKeys& keys, ValidationContext& context)
		{
			ValidateTagList(keys.Tags, context);
		}

		static void MakeEntityKeysValid(PrefabEntityKeys& keys, Random& /*random*/)
		{
			MakeTagListValid(keys.Tags);
		}

		static bool IsEntityKeyName(std::string_view name)
		{
			return std::find(std::begin(EntityKeyNames), std::end(EntityKeyNames), name) != std::end(EntityKeyNames);
		}

		// A random value of the entity key `key` that PrefabEntityKeys' schema accepts.
		static Json MakeRandomEntityKeyValue(std::string_view key, Random& random)
		{
			if (key == "Active")
				return Json(random.NextBool());
			if (key == "Name")
				return Json(std::format("Entity{:04x}", random.NextU32() & 0xffffu));

			std::vector<std::string> tags;
			const int64_t count = random.RangeInt(0, 3);
			for (int64_t index = 0; index < count; ++index)
				tags.push_back(std::format("Tag{}", index));
			return Json(tags);
		}

		// The type-level rules of PrefabOverride (§5.5, ADR 0006 decision 18): the combinations of Kind, Component, Field and
		// Value that contradict each other. Whether PrefabEntityID, Component and Field name something that exists is not a
		// type-level rule: the Value resolver reports an unknown (or empty) component or field, and PrefabInstantiator drops
		// an override whose prefab entity or target no longer exists, with a warning, at the next update (§5.5). A
		// default-constructed override (Kind Field, no names) is therefore a valid, unresolved one, as the registry suite's
		// default round trip requires (§5.4). An entity key is never a stale name, so EntityKey's Field must be one of three.
		static void ValidatePrefabOverride(const PrefabOverride& prefabOverride, ValidationContext& context)
		{
			switch (prefabOverride.Kind)
			{
				case PrefabOverrideKind::Field:
					break;
				case PrefabOverrideKind::AddComponent:
				case PrefabOverrideKind::RemoveComponent:
				{
					const bool removes = prefabOverride.Kind == PrefabOverrideKind::RemoveComponent;
					if (!prefabOverride.Field.empty())
						context.Error("Field", std::format("must be empty for {} override", removes ? "a RemoveComponent" : "an AddComponent"));
					if (removes && !prefabOverride.Value.IsNull())
						context.Error("Value", "must be null for a RemoveComponent override");
					break;
				}
				case PrefabOverrideKind::EntityKey:
				{
					if (!prefabOverride.Component.empty())
						context.Error("Component", "must be empty for an EntityKey override");
					if (!IsEntityKeyName(prefabOverride.Field))
						context.Error("Field", "must be Name, Active or Tags for an EntityKey override");
					break;
				}
			}
		}

		// The Generate hook of PrefabOverride: makes randomized Field and Value agree with the kind. An entity key that had to
		// be chosen gets a Value drawn for it. Random component and field names stay: they do not resolve, so the free-form
		// Value drawn for them is kept with a diagnostic, as a stale override's would be.
		static void MakePrefabOverrideValid(PrefabOverride& prefabOverride, Random& random)
		{
			switch (prefabOverride.Kind)
			{
				case PrefabOverrideKind::Field:
					break;
				case PrefabOverrideKind::AddComponent:
				case PrefabOverrideKind::RemoveComponent:
				{
					prefabOverride.Field.clear();
					if (prefabOverride.Kind == PrefabOverrideKind::RemoveComponent)
						prefabOverride.Value = VariantValue();
					break;
				}
				case PrefabOverrideKind::EntityKey:
				{
					prefabOverride.Component.clear();
					if (!IsEntityKeyName(prefabOverride.Field))
					{
						const auto index = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(std::size(EntityKeyNames)) - 1));
						prefabOverride.Field = std::string(EntityKeyNames[index]);
						prefabOverride.Value = VariantValue(MakeRandomEntityKeyValue(prefabOverride.Field, random));
					}
					break;
				}
			}
		}

		// A schema-only Variant type without a resolver: free-form JSON.
		static TypeInfo::Specification MakeFreeFormVariantType()
		{
			TypeInfo::Specification specification;
			specification.Kind = FieldType::Variant;
			specification.Name = std::string(FieldTypeToString(FieldType::Variant));
			return specification;
		}

		// The schema-only field describing a RemoveComponent override's Value.
		static FieldInfo::Specification MakeRemovedComponentValueField(const TypeInfo& type)
		{
			FieldInfo::Specification specification;
			specification.Name = "Value";
			specification.Description = "A RemoveComponent override has no value: null.";
			specification.Type = &type;
			return specification;
		}

		// The schema of a RemoveComponent override's Value: free-form JSON, which ValidatePrefabOverride restricts to null.
		// Immutable constants after their thread-safe first-use initialization; they outlive every resolution.
		static const FieldInfo& GetRemovedComponentValueSchema()
		{
			static const TypeInfo RemovedValueType(MakeFreeFormVariantType());
			static const FieldInfo RemovedValueField(MakeRemovedComponentValueField(RemovedValueType));
			return RemovedValueField;
		}

		// What an override resolver reads from its owner: the kind and the names that precede Value.
		struct PrefabOverrideTarget
		{
			PrefabOverrideKind Kind = PrefabOverrideKind::Field;
			std::string Component;
			std::string Field;
		};

		// Reads the target from the PrefabOverride object or, without one, from its JSON object (members are optional: an
		// absent one has its default, as StructInfo::FromJson reads it).
		static Result<PrefabOverrideTarget> ReadPrefabOverrideTarget(const ResolveContext& context, const TypeRegistry& registry)
		{
			if (context.Owner != nullptr)
			{
				const auto& prefabOverride = *static_cast<const PrefabOverride*>(context.Owner);
				return PrefabOverrideTarget{ prefabOverride.Kind, prefabOverride.Component, prefabOverride.Field };
			}
			if (context.OwnerJson == nullptr)
				return MakeError(ErrorCode::InvalidArgument, "a prefab override value resolves only with its override object or JSON");

			PrefabOverrideTarget target;
			if (const std::optional<JsonReader> kind = context.OwnerJson->FindMember("Kind"))
			{
				ENGINE_TRY_ASSIGN(const std::string kindName, kind->ReadString());
				const EnumInfo* kinds = registry.FindEnum<PrefabOverrideKind>();
				ENGINE_CORE_ASSERT(kinds != nullptr, "PrefabOverrideKind is registered before PrefabOverride");
				const EnumEntry* entry = kinds != nullptr ? kinds->FindByName(kindName) : nullptr;
				if (entry == nullptr)
				{
					const std::vector<std::string> suggestions = kinds != nullptr ? kinds->SuggestNames(kindName) : std::vector<std::string>();
					Error error = kind->MakeLocatedError(ErrorCode::Validation, std::format("unknown override kind '{}'", kindName));
					return std::unexpected(std::move(error).WithHint(MakeDidYouMeanHint(suggestions)));
				}
				target.Kind = static_cast<PrefabOverrideKind>(entry->Value);
			}
			if (const std::optional<JsonReader> component = context.OwnerJson->FindMember("Component"))
			{
				ENGINE_TRY_ASSIGN(target.Component, component->ReadString());
			}
			if (const std::optional<JsonReader> field = context.OwnerJson->FindMember("Field"))
			{
				ENGINE_TRY_ASSIGN(target.Field, field->ReadString());
			}
			return target;
		}

		// A component that overrides can target: registered, serialized under "Components" (not an entity-level component,
		// whose changes are EntityKey overrides) and not engine-maintained. Hidden components (Prefab, PrefabLink) are never
		// overridden, as PrefabInstantiator applies overrides; resolving them would also let an override's Value hold a
		// whole prefab instance whose overrides hold another, nesting the resolution as deep as a file chooses.
		static Result<const ComponentInfo*> FindOverridableComponent(const TypeRegistry& registry, const std::string& name)
		{
			const ComponentInfo* component = registry.FindComponent(name);
			if (component == nullptr)
			{
				const std::string message = std::format("unknown component '{}'", name);
				return std::unexpected(Error(ErrorCode::NotFound, message).WithHint(MakeDidYouMeanHint(registry.SuggestComponentNames(name))));
			}
			if (component->HasFlag(ComponentFlags::EntityLevel) || !component->HasFlag(ComponentFlags::Serializable))
				return MakeError(ErrorCode::NotFound, "component '{}' is not written under \"Components\", so it has no overrides", name);
			if (component->HasFlag(ComponentFlags::Hidden))
				return MakeError(ErrorCode::NotFound, "component '{}' is maintained by the engine, so it has no overrides", name);
			return component;
		}

		// The VariantSchemaResolver of PrefabOverride.Value (§5.4): the overridden field's schema for Field, the
		// component's whole schema for AddComponent, null for RemoveComponent (of a component that exists), the
		// PrefabEntityKeys field for EntityKey.
		static Result<const FieldInfo*> ResolvePrefabOverrideValue(const ResolveContext& context)
		{
			if (context.OwnerType == nullptr || context.OwnerType->GetType().GetKey() != TypeKeyOf<PrefabOverride>())
			{
				const std::string owner = context.OwnerType != nullptr ? context.OwnerType->GetName() : std::string("none");
				return MakeError(ErrorCode::InvalidArgument, "a prefab override value resolves only on a PrefabOverride (owner type '{}')", owner);
			}

			const TypeRegistry& registry = context.Registry != nullptr ? *context.Registry : context.OwnerType->GetRegistry();
			ENGINE_TRY_ASSIGN(const PrefabOverrideTarget target, ReadPrefabOverrideTarget(context, registry));
			switch (target.Kind)
			{
				case PrefabOverrideKind::Field:
				{
					ENGINE_TRY_ASSIGN(const ComponentInfo* component, FindOverridableComponent(registry, target.Component));
					const FieldInfo* field = component->FindField(target.Field);
					if (field != nullptr && field->IsStored() && field->GetMeta().Serialized)
						return field;

					const std::string message = std::format("component '{}' has no serialized field '{}'", target.Component, target.Field);
					return std::unexpected(Error(ErrorCode::NotFound, message).WithHint(MakeDidYouMeanHint(component->SuggestFieldNames(target.Field))));
				}
				case PrefabOverrideKind::AddComponent:
				{
					ENGINE_TRY_ASSIGN(const ComponentInfo* component, FindOverridableComponent(registry, target.Component));
					return &component->GetSelfField();
				}
				case PrefabOverrideKind::RemoveComponent:
				{
					ENGINE_TRY(FindOverridableComponent(registry, target.Component));
					return &GetRemovedComponentValueSchema();
				}
				case PrefabOverrideKind::EntityKey:
				{
					const StructInfo* keys = registry.FindStruct<PrefabEntityKeys>();
					ENGINE_CORE_ASSERT(keys != nullptr, "PrefabEntityKeys is registered before PrefabOverride");
					const FieldInfo* field = keys != nullptr ? keys->FindField(target.Field) : nullptr;
					if (field == nullptr)
						return MakeError(ErrorCode::NotFound, "'{}' is not an entity key (Name, Active or Tags)", target.Field);
					return field;
				}
			}

			ENGINE_CORE_ASSERT(false, "Unknown PrefabOverrideKind {}", std::to_underlying(target.Kind));
			return MakeError(ErrorCode::InvalidArgument, "unknown prefab override kind {}", std::to_underlying(target.Kind));
		}

		// Transform's virtual fields (§5.2). With an owning entity they go through TransformSystem, which walks the parent
		// chain and patches the local transform (change tracker, revision); without one the object is a root, so its
		// world pose is its local pose.
		static TransformComponent& GetTransform(const FieldContext& context)
		{
			return *static_cast<TransformComponent*>(context.Object);
		}

		// A unit quaternion from a written rotation. Errors: Validation for a zero-length (or non-finite) quaternion,
		// which no rotation represents.
		static Result<glm::quat> NormalizeRotation(const glm::quat& rotation)
		{
			const float lengthSquared = glm::dot(rotation, rotation);
			if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared))
				return MakeError(ErrorCode::Validation, "a rotation must be a non-zero, finite quaternion");
			return glm::normalize(rotation);
		}

		static Value GetEulerAngles(const FieldContext& context)
		{
			return Value::FromVec3(TransformSystem::EulerDegreesFromQuaternion(GetTransform(context).Rotation));
		}

		static Status SetEulerAngles(const FieldContext& context, const Value& value)
		{
			const glm::quat rotation = TransformSystem::QuaternionFromEulerDegrees(value.AsVec3());
			if (context.Owner == nullptr)
			{
				GetTransform(context).Rotation = rotation;
				return {};
			}
			const Entity owner = *context.Owner;
			owner.Patch<TransformComponent>([rotation](TransformComponent& transform)
			{
				transform.Rotation = rotation;
			});
			return {};
		}

		static Value GetWorldPosition(const FieldContext& context)
		{
			if (context.Owner == nullptr)
				return Value::FromVec3(GetTransform(context).Translation);
			return Value::FromVec3(TransformSystem::GetWorldPosition(*context.Owner));
		}

		// Fails when TransformSystem::SetWorldPosition could not store `position` for `owner`: the translation relative to the
		// parent (the position through the inverse of the parent's world matrix, computed as SetWorldPosition computes it) is
		// not finite, because the position lies beyond what the parent chain can express in float or the parent's world
		// matrix cannot be inverted. External input never reaches SetWorldPosition's assert, nor stores a non-finite value.
		static Status CheckWorldPositionRepresentable(Entity owner, const glm::vec3& position)
		{
			const Entity parent = owner.GetParent();
			if (!parent.IsValid())
				return {};
			const glm::vec3 local(glm::affineInverse(TransformSystem::ComputeWorldMatrix(parent)) * glm::vec4(position, 1.0f));
			if (!glm::any(glm::isnan(local)) && !glm::any(glm::isinf(local)))
				return {};
			return MakeError(ErrorCode::Validation,
				"the world position ({}, {}, {}) is not representable: its translation relative to the parent is not finite", position.x,
				position.y, position.z);
		}

		static Status SetWorldPosition(const FieldContext& context, const Value& value)
		{
			const glm::vec3 position = value.AsVec3();
			if (context.Owner == nullptr)
			{
				GetTransform(context).Translation = position;
				return {};
			}
			ENGINE_TRY(CheckWorldPositionRepresentable(*context.Owner, position));
			TransformSystem::SetWorldPosition(*context.Owner, position);
			return {};
		}

		static Value GetWorldRotation(const FieldContext& context)
		{
			if (context.Owner == nullptr)
				return Value::FromQuat(glm::normalize(GetTransform(context).Rotation));
			return Value::FromQuat(TransformSystem::GetWorldRotation(*context.Owner));
		}

		static Status SetWorldRotation(const FieldContext& context, const Value& value)
		{
			ENGINE_TRY_ASSIGN(const glm::quat rotation, NormalizeRotation(value.AsQuat()));
			if (context.Owner == nullptr)
				GetTransform(context).Rotation = rotation;
			else
				TransformSystem::SetWorldRotation(*context.Owner, rotation);
			return {};
		}

		static Value GetWorldScale(const FieldContext& context)
		{
			if (context.Owner == nullptr)
				return Value::FromVec3(GetTransform(context).Scale);
			return Value::FromVec3(TransformSystem::GetWorldScale(*context.Owner));
		}

		static Value GetRenderPosition(const FieldContext& context)
		{
			if (context.Owner == nullptr)
				return Value::FromVec3(GetTransform(context).Translation);
			return Value::FromVec3(TransformSystem::GetRenderPosition(*context.Owner));
		}

		static Value GetRenderRotation(const FieldContext& context)
		{
			if (context.Owner == nullptr)
				return Value::FromQuat(glm::normalize(GetTransform(context).Rotation));
			return Value::FromQuat(TransformSystem::GetRenderRotation(*context.Owner));
		}

	}

	void RegisterCoreComponents(TypeRegistry& registry)
	{
		registry.Enum<PrefabOverrideKind>("PrefabOverrideKind", "What a prefab override changes on one prefab entity of an instance.")
			.Entry(PrefabOverrideKind::Field, "Field", "One field of one component; Value is the field's value.")
			.Entry(PrefabOverrideKind::AddComponent, "AddComponent", "A component added to the prefab entity; Value is the whole component.")
			.Entry(PrefabOverrideKind::RemoveComponent, "RemoveComponent", "A component of the prefab entity removed from it; Value is null.")
			.Entry(PrefabOverrideKind::EntityKey, "EntityKey", "An entity key (Name, Active or Tags); Value is the key's value.");

		registry.Struct<PrefabEntityKeys>("PrefabEntityKeys", "The entity keys an EntityKey prefab override can change, with their file spelling.")
			.Field("Name", &PrefabEntityKeys::Name, "The entity's display name (the entity key \"Name\").")
			.Field("Active", &PrefabEntityKeys::Active, "Whether the entity itself is active (the entity key \"Active\").")
			.Field("Tags", &PrefabEntityKeys::Tags, "The entity's gameplay tags, non-empty and unique (the entity key \"Tags\").")
			.Validate(&Utils::ValidateEntityKeys)
			.Generate(&Utils::MakeEntityKeysValid);

		registry.Struct<PrefabOverride>("PrefabOverride", "One recorded difference between a prefab instance and its prefab.")
			.Field("PrefabEntityID", &PrefabOverride::PrefabEntityID,
				"The prefab-local ID of the entity the override applies to (never the derived instance ID).")
			.Field("Kind", &PrefabOverride::Kind, "What the override changes: a field, an added or removed component, or an entity key.")
			.Field("Component", &PrefabOverride::Component,
				"The registry name of the component for Field, AddComponent and RemoveComponent; empty for EntityKey.")
			.Field("Field", &PrefabOverride::Field, "The field name for Field; Name, Active or Tags for EntityKey; empty otherwise.")
			.VariantField("Value", &PrefabOverride::Value,
				"The overriding value, checked against the schema of its target: the field, the whole component, or the entity key.",
				&Utils::ResolvePrefabOverrideValue)
			.Validate(&Utils::ValidatePrefabOverride)
			.Generate(&Utils::MakePrefabOverrideValid);

		RegisterComponent<IDComponent>(registry, "ID", "The entity's persistent identity, written as the entity key \"ID\".")
			.Category("Core")
			.Version(1)
			.Flags(ComponentFlags::Required | ComponentFlags::Hidden | ComponentFlags::EntityLevel)
			.RemoveFlags(ComponentFlags::Removable)
			.Field("ID", &IDComponent::ID, "The entity's UUID; set at creation and never changed.", { .ReadOnly = true });

		RegisterComponent<NameComponent>(registry, "Name", "The entity's display name, written as the entity key \"Name\".")
			.Category("Core")
			.Version(1)
			.Flags(ComponentFlags::Required | ComponentFlags::EntityLevel)
			.RemoveFlags(ComponentFlags::Removable)
			.Field("Name", &NameComponent::Name, "The display name used by entity paths; names need not be unique.");

		RegisterComponent<TagsComponent>(registry, "Tags", "The entity's gameplay tags, written as the entity key \"Tags\".")
			.Category("Core")
			.Version(1)
			.Flags(ComponentFlags::EntityLevel)
			.RemoveFlags(ComponentFlags::Removable)
			.Field("Tags", &TagsComponent::Tags, "Free-form tags such as \"Player\"; non-empty, unique, in insertion order.")
			.Validate(&Utils::ValidateTags)
			.Generate(&Utils::MakeTagsValid);

		RegisterComponent<RelationshipComponent>(registry, "Relationship",
			"The entity's place in the hierarchy; its parent is written as the entity key \"Parent\".")
			.Category("Core")
			.Version(1)
			.Flags(ComponentFlags::Required | ComponentFlags::Hidden | ComponentFlags::EntityLevel)
			.RemoveFlags(ComponentFlags::Removable)
			.Field("Parent", &RelationshipComponent::Parent, "The parent entity; null for a root entity.", { .ReadOnly = true })
			.Field("Children", &RelationshipComponent::Children,
				"The children in sibling order; rebuilt from the children's parents on load, so never written.",
				{ .ReadOnly = true, .Serialized = false });

		RegisterComponent<TransformComponent>(registry, "Transform", "Positions, rotates and scales the entity relative to its parent.")
			.Category("Core")
			.Version(1)
			.Flags(ComponentFlags::Required)
			.RemoveFlags(ComponentFlags::Removable)
			.Field("Translation", &TransformComponent::Translation, "Position relative to the parent, in metres.", { .Unit = "m" })
			.Field("Rotation", &TransformComponent::Rotation, "Rotation relative to the parent: a unit quaternion written [x, y, z, w].")
			.Field("Scale", &TransformComponent::Scale, "Scale along the local axes; each component's magnitude is at least 0.0001.",
				{ .MinMagnitude = MinTransformScaleMagnitude })
			.VirtualField<glm::vec3>("EulerAngles", "Rotation relative to the parent as Euler angles in degrees, applied Z, then X, then Y.",
				&Utils::GetEulerAngles, &Utils::SetEulerAngles, { .Unit = "deg" })
			.VirtualField<glm::vec3>("WorldPosition", "Position in world space, in metres; writing it moves the entity through its parent.",
				&Utils::GetWorldPosition, &Utils::SetWorldPosition, { .Unit = "m" })
			.VirtualField<glm::quat>("WorldRotation", "Rotation in world space; writing it rotates the entity through its parent.",
				&Utils::GetWorldRotation, &Utils::SetWorldRotation)
			.VirtualField<glm::vec3>("WorldScale", "Approximate scale in world space (read-only: a sheared parent chain has no exact one).",
				&Utils::GetWorldScale, nullptr)
			.VirtualField<glm::vec3>("RenderPosition", "The rendered world position, interpolated between fixed steps, in metres (read-only).",
				&Utils::GetRenderPosition, nullptr, { .Unit = "m" })
			.VirtualField<glm::quat>("RenderRotation", "The rendered world rotation, interpolated between fixed steps (read-only).",
				&Utils::GetRenderRotation, nullptr);

		RegisterComponent<PrefabInstanceComponent>(registry, "Prefab", "Marks the root of a prefab instance and records its overrides.")
			.Category("Core")
			.Version(1)
			.Flags(ComponentFlags::Hidden)
			.RemoveFlags(ComponentFlags::EditorVisible | ComponentFlags::Removable)
			.Field("Prefab", &PrefabInstanceComponent::Prefab, "The prefab asset the instance was created from.")
			.Field("Overrides", &PrefabInstanceComponent::Overrides,
				"The instance's differences from the prefab, sorted by prefab entity, component, kind and field.");

		RegisterComponent<PrefabLinkComponent>(registry, "PrefabLink", "Links a prefab instance member, the root included, to its prefab entity.")
			.Category("Core")
			.Version(1)
			.Flags(ComponentFlags::Hidden)
			.RemoveFlags(ComponentFlags::EditorVisible | ComponentFlags::Removable)
			.Field("PrefabEntityID", &PrefabLinkComponent::PrefabEntityID, "The prefab-local ID of the entity this member was created from.")
			.Field("InstanceRoot", &PrefabLinkComponent::InstanceRoot, "The root entity of the instance, which has the Prefab component.");
	}

}
