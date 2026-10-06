#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/Value.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	class Entity; // Scene/Entity.h. Reflection only passes the pointer through to virtual field accessors (ADR 0006).
	class FieldInfo;
	class StructInfo;
	class TypeRegistry;

	namespace Detail {

		struct ReflectionAccess; // Reflection/Private/ReflectionWalk.h

	}

	// Per-field metadata (Architecture §5.4). Min, Max, Step and MinMagnitude apply to each numeric component of Int32,
	// UInt32, Float, vector and colour fields (IsNumericFieldType), and are enforced on every write path: readers, reflected
	// setters, script proxies and automation, never only in the inspector. Designated initializers name them in this order:
	//     .Field("Mass", &RigidBodyComponent::Mass, "Mass in kilograms (Dynamic only).", { .Min = 0.001, .Unit = "kg" })
	struct FieldMeta
	{
		std::optional<double> Min = std::nullopt;          // inclusive
		std::optional<double> Max = std::nullopt;          // inclusive
		std::optional<double> Step = std::nullopt;         // inspector drag step; not enforced
		std::optional<double> MinMagnitude = std::nullopt; // every component satisfies |v| >= MinMagnitude (Transform.Scale)
		std::string Unit = {};                             // "kg", "m", "deg", "px"; empty for unitless values
		// The accepted asset type of an AssetRef field, by AssetType name ("Mesh"; empty = any type). Set from the field's
		// TypedAssetHandle type by the registry; registrations never write it (ADR 0006).
		std::string AssetFilter = {};
		bool ReadOnly = false;          // readable by scripts, automation and the inspector, never written by them
		bool Hidden = false;            // absent from the inspector and the script API; still serialized when Serialized
		bool Virtual = false;           // a registered getter/setter pair, never serialized (set by FieldBuilder::VirtualField)
		bool Scriptable = true;         // visible to script proxies (§11.4)
		bool Serialized = true;         // written to and read from files (false for virtual fields)
		RunModes Modes = RunModes::All; // All or EditorOnly (§11.4)
	};

	// What a virtual field accessor works on: the object holding the field and, for a component living in a scene, the
	// entity that owns it (TransformComponent.WorldPosition walks the parent chain). Owner is null for an object outside
	// any scene, in which case world-space accessors treat the object as a root.
	struct FieldContext
	{
		void* Object = nullptr;
		const Entity* Owner = nullptr;
	};

	// A virtual field's getter: returns a Value of the field's kind. It must not modify the object or the scene.
	using VirtualFieldGetter = Value (*)(const FieldContext& context);
	// A virtual field's setter: receives a Value already validated against the field (kind, finite, range) and applies it,
	// patching the component through the scene so systems and the change tracker see the write. Errors: whatever the
	// operation can fail with (InvalidState when Owner is required but null).
	using VirtualFieldSetter = Status (*)(const FieldContext& context, const Value& value);

	// A source of field schemas declared outside the registry, which Variant resolvers consult: in M13 the script field
	// schemas extracted by ScriptImporter (`owner` = the script's asset handle); in tests a fixture source. Implementations
	// return schemas that outlive every use of the returned pointer (the source's lifetime). Thread-safety is the
	// implementation's; the engine calls it on the main thread.
	class IFieldSchemaSource
	{
	public:
		virtual ~IFieldSchemaSource() = default;

		// The field `name` declared by `owner`. Errors: NotFound when `owner` is unknown or declares no such field (with
		// suggestions when close names exist).
		[[nodiscard]] virtual Result<const FieldInfo*> FindField(UUID owner, std::string_view name) const = 0;

		// Every field name `owner` declares, in declaration order; empty when `owner` is unknown.
		[[nodiscard]] virtual std::vector<std::string> GetFieldNames(UUID owner) const = 0;
	};

	// What a VariantSchemaResolver receives (Architecture §5.4). The engine fills the owner members whenever it resolves a
	// field of a struct (StructInfo::FromJson, ApplyMergePatch and Validate, FieldInfo::ValidateValue and ValidateJson);
	// a caller resolving by hand sets them the same way.
	//
	// Descending into a resolved schema that is itself a struct (a PrefabOverride's AddComponent value resolves to the
	// whole component, §5.4) replaces Owner, OwnerType and OwnerJson with that nested object (its C++ object when one
	// exists, otherwise null, and its JSON object) and clears Key. A Field override's value resolves to the field itself
	// without its owning component, so a resolver below it (ScriptComponent.Fields inside a PrefabOverride of field
	// "Script.Fields") gets OwnerType = that component and neither Owner nor OwnerJson: it cannot resolve, and the values
	// are kept with REFLECTION_VARIANT_UNRESOLVED until they are applied to a real entity (PrefabInstantiator applies them
	// through ComponentAccess, where the member's ScriptComponent is the owner).
	struct ResolveContext
	{
		const TypeRegistry* Registry = nullptr;
		// The C++ object that holds the Variant field (a ScriptComponent, a PrefabOverride), or null when the value is
		// checked without one (JSON-only validation). Fields declared before the Variant field have already been read into
		// it, so a resolver may read exactly those (ScriptComponent.Script precedes Fields; PrefabOverride's Kind,
		// Component and Field precede Value).
		const void* Owner = nullptr;
		// The registered type of the owner (of Owner, or of the object OwnerJson holds). A resolver checks that it is its
		// own type before it reads Owner or OwnerJson, and returns InvalidArgument otherwise, so it never misreads an owner
		// of another type; null OwnerType is InvalidArgument too.
		const StructInfo* OwnerType = nullptr;
		// The owner's JSON object when the value comes from JSON (FromJson, ApplyMergePatch, ValidateJson); null when a Value
		// is checked on a C++ object. With Owner null a resolver reads the preceding fields from it (the "Script" handle of a
		// Script component object, in the same spelling as the file).
		const JsonReader* OwnerJson = nullptr;
		// The map key when the Variant is a value of a Map field (ScriptComponent.Fields); empty otherwise.
		std::string_view Key;
		// External schemas (script field schemas); may be null, in which case script fields cannot be resolved.
		const IFieldSchemaSource* Schemas = nullptr;
	};

	// Resolves the schema of a Variant value at run time from its owner and key: from Owner when it is set, otherwise from
	// OwnerJson. Errors: InvalidArgument when OwnerType is not the resolver's type or neither Owner nor OwnerJson is set;
	// NotFound (unknown script field, missing prefab target), or any error explaining why no schema applies. On failure
	// the value is preserved verbatim and a diagnostic is raised, so data is never dropped (§5.4). The returned FieldInfo
	// outlives the value's use (it belongs to the registry or to the schema source).
	using VariantSchemaResolver = Result<const FieldInfo*> (*)(const ResolveContext& context);

	// The diagnostic codes of reflected reads (ValidationIssue::Code).
	inline constexpr std::string_view UnknownFieldCode = "REFLECTION_UNKNOWN_FIELD";             // warning, or error with --strict
	inline constexpr std::string_view VariantUnresolvedCode = "REFLECTION_VARIANT_UNRESOLVED";   // warning; value kept verbatim
	inline constexpr std::string_view VariantSchemaMismatchCode = "REFLECTION_VARIANT_MISMATCH"; // warning; value kept verbatim

	// Locates a stored field inside its owner object. MemberFieldAccessor is the only implementation for C++ members.
	class IFieldAccessor
	{
	public:
		virtual ~IFieldAccessor() = default;

		[[nodiscard]] virtual void* GetAddress(void* object) const = 0;
		[[nodiscard]] virtual const void* GetAddress(const void* object) const = 0;
	};

	template<typename Owner, typename Member>
	class MemberFieldAccessor final : public IFieldAccessor
	{
	public:
		explicit MemberFieldAccessor(Member Owner::* member)
			: m_Member(member)
		{
		}

		[[nodiscard]] void* GetAddress(void* object) const override
		{
			return &(static_cast<Owner*>(object)->*m_Member);
		}

		[[nodiscard]] const void* GetAddress(const void* object) const override
		{
			return &(static_cast<const Owner*>(object)->*m_Member);
		}
	private:
		Member Owner::* m_Member = nullptr;
	};

	// One reflected field (Architecture §5.4): name, mandatory description, value type, metadata and access. A field is
	// either stored (a C++ member reached through its accessor), virtual (a getter and an optional setter; read-only
	// without one), or schema-only (no storage at all: a script field schema or a struct's self-description, used to
	// validate and generate JSON). Variant fields may carry a resolver.
	//
	// Fields are created by the registry builders (or by a schema owner) and never change afterwards; their addresses are
	// stable for the owner's lifetime, which is what VariantSchemaResolver results rely on. Every const member is
	// thread-safe; reading or writing an object through a field follows the object's own threading rule.
	class FieldInfo
	{
	public:
		struct Specification
		{
			std::string Name;               // PascalCase, unique within its struct, non-empty
			std::string Description;        // mandatory, non-empty (GenerateDocs.py --check, gate 7)
			const TypeInfo* Type = nullptr; // required
			FieldMeta Meta;
			Scope<IFieldAccessor> Accessor;           // stored fields
			VirtualFieldGetter Getter = nullptr;      // virtual fields
			VirtualFieldSetter Setter = nullptr;      // virtual fields; null = read-only
			VariantSchemaResolver Resolver = nullptr; // fields containing Variant values; null = free-form JSON
		};

		// Takes the specification. A stored field has an accessor and no getter; a virtual field a getter and no accessor
		// (Meta.Virtual set, Meta.Serialized cleared); a schema-only field neither (asserted).
		explicit FieldInfo(Specification specification);

		FieldInfo(FieldInfo&&) noexcept = default;
		FieldInfo& operator=(FieldInfo&&) noexcept = default;
		FieldInfo(const FieldInfo&) = delete;
		FieldInfo& operator=(const FieldInfo&) = delete;

		[[nodiscard]] const std::string& GetName() const { return m_Specification.Name; }
		[[nodiscard]] const std::string& GetDescription() const { return m_Specification.Description; }
		[[nodiscard]] const TypeInfo& GetType() const { return *m_Specification.Type; }
		[[nodiscard]] FieldType GetKind() const { return m_Specification.Type->GetKind(); }
		[[nodiscard]] const FieldMeta& GetMeta() const { return m_Specification.Meta; }
		[[nodiscard]] bool IsStored() const { return m_Specification.Accessor != nullptr; }
		[[nodiscard]] bool IsVirtual() const { return m_Specification.Getter != nullptr; }
		[[nodiscard]] bool IsReadOnly() const { return m_Specification.Meta.ReadOnly || (IsVirtual() && m_Specification.Setter == nullptr); }
		[[nodiscard]] VariantSchemaResolver GetResolver() const { return m_Specification.Resolver; }

		// The address of a stored field inside `object` (asserted stored).
		[[nodiscard]] void* GetAddress(void* object) const;
		[[nodiscard]] const void* GetAddress(const void* object) const;

		// The field's value: a scalar through its type's Read, a composite assembled element by element, a virtual field
		// through its getter. Asserts a stored or virtual field and a non-null context.Object.
		[[nodiscard]] Value GetValue(const FieldContext& context) const;

		// Validates `value` (ValidateValue) and assigns it: a stored field through its type's Write (composites element by
		// element), a virtual field through its setter. Atomic: on error nothing changed. This is the reflected setter of
		// §5.4; Scene's ComponentAccess wraps it in Entity::Patch so the change tracker and systems see the write. Errors:
		// InvalidState for a read-only field; Validation from ValidateValue; the setter's own errors.
		[[nodiscard]] Status SetValue(const FieldContext& context, const Value& value) const;

		// Checks `value` against this field without writing it: kind, finite floats, Min/Max/MinMagnitude per component,
		// enum values that name an enumerator, array and map elements, struct members, and Variant values against their
		// resolved schema (an unresolvable Variant is accepted with a REFLECTION_VARIANT_UNRESOLVED warning). `resolve`
		// describes this field's owner (Owner, OwnerType); nested struct members are checked with the owner replaced as
		// ResolveContext describes. Issues are added to `validation` at its current pointer plus this field's name.
		void ValidateValue(const Value& value, const ResolveContext& resolve, ValidationContext& validation) const;

		// Checks the JSON spelling of a value of this field the same way, without a C++ object (schema-only fields,
		// resolved Variant values, automation parameters): `resolve` describes the enclosing object as far as it is known
		// (OwnerType and OwnerJson, and Owner only when the caller has the C++ object).
		void ValidateJson(const JsonReader& reader, const ResolveContext& resolve, ValidationContext& validation) const;

		// The schema of a Variant value of this field: the resolver's result, or NotFound "no resolver" for a free-form
		// Variant field (callers then treat the value as free-form JSON). Asserts that the field contains Variant values.
		[[nodiscard]] Result<const FieldInfo*> ResolveVariant(const ResolveContext& context) const;
	private:
		// The resolution context of this field's own owner object, for SetValue.
		[[nodiscard]] ResolveContext MakeOwnerResolveContext(const void* owner) const;
	private:
		Specification m_Specification;
		// Back-reference to the struct that declares this field (set by StructInfo::AddField); null for schema-only fields
		// and self fields. SetValue resolves Variant values with it.
		const StructInfo* m_Owner = nullptr;
	private:
		friend struct Detail::ReflectionAccess;
	};

}
