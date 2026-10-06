#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/Value.h"

#include <string_view>

namespace Engine {

	class ConstEntity;
	class Entity;
	class IFieldSchemaSource;

	// By-name component access through the registry: the "reflected setters" of Architecture §5.4, shared by automation
	// (component.* methods, M4), script proxies (M13), the inspector (M10) and prefab override application (stream E).
	// Every write validates against the registry first (finite floats, Min/Max/MinMagnitude, enum names, Variant schemas,
	// type-level validators), is atomic (on error nothing changes), goes through ComponentHostOps::Patch (EnTT on_update,
	// change tracker, revision) and therefore never lets an invalid value reach a component, Jolt or the renderer.
	//
	// Unknown names fail with NotFound and "did you mean" suggestions (FuzzySuggest) in the error's hint and issues.
	// Entity-level components (ID, Name, Tags, Relationship) are reached through Entity members and the hierarchy
	// operations, not here: naming one is InvalidArgument. Hidden components are maintained by the engine (Prefab and
	// PrefabLink by PrefabInstantiator, which keeps instance roots and links consistent, §6): they can be read here, but
	// every write (add, remove, set, patch, field set) is InvalidArgument, so no validated write path can produce a
	// structural defect. Main thread, like the scene. Static functions only.
	class ComponentAccess
	{
	public:
		// The canonical JSON of the component (StructInfo::ToJson). Read-only, so it takes a ConstEntity (an Entity
		// converts). Errors: NotFound for an unknown component name or a component the entity does not have;
		// InvalidArgument for an entity-level component.
		[[nodiscard]] static Result<Json> GetComponentJson(ConstEntity entity, std::string_view component);

		// Adds the component, default-initialized and then read from `initial` when it is not null (an object; missing fields
		// keep their defaults). Errors: NotFound (unknown name); InvalidArgument (entity-level or Hidden component);
		// InvalidState when the entity already has it, when a Requires component is missing or an Excludes component
		// present (naming it), or when a UniquePerScene component already exists elsewhere in the scene; Validation for
		// invalid `initial` values.
		[[nodiscard]] static Status AddComponent(Entity entity, std::string_view component, const Json* initial,
			const IFieldSchemaSource* schemas = nullptr);

		// Removes the component. Errors: NotFound (unknown or absent); InvalidArgument (entity-level or Hidden component);
		// InvalidState when it is not Removable or another present component Requires it (naming it).
		[[nodiscard]] static Status RemoveComponent(Entity entity, std::string_view component);

		// Replaces every serialized field of the component with `value` (an object; missing fields reset to defaults).
		// Errors: as GetComponentJson, plus InvalidArgument for a Hidden component and Validation.
		[[nodiscard]] static Status SetComponentJson(Entity entity, std::string_view component, const Json& value,
			const IFieldSchemaSource* schemas = nullptr);

		// Applies an RFC 7386 merge patch to the component (StructInfo::ApplyMergePatch: null resets a field or deletes a Map
		// key, nested objects merge, Variant values are replaced whole). Errors: as SetComponentJson.
		[[nodiscard]] static Status PatchComponentJson(Entity entity, std::string_view component, const Json& patch,
			const IFieldSchemaSource* schemas = nullptr);

		// The value of one field, virtual fields included (Transform.WorldPosition). Errors: NotFound for an unknown
		// component, field or absent component; InvalidArgument for an entity-level component.
		[[nodiscard]] static Result<Value> GetFieldValue(Entity entity, std::string_view component, std::string_view field);

		// Validates and writes one field, virtual fields included (FieldInfo::SetValue inside a patch). Errors: as
		// GetFieldValue, plus InvalidArgument for a Hidden component, InvalidState for a read-only field and Validation for
		// an invalid value.
		[[nodiscard]] static Status SetFieldValue(Entity entity, std::string_view component, std::string_view field,
			const Value& value, const IFieldSchemaSource* schemas = nullptr);
	};

}
