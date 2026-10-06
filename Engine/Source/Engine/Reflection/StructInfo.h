#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UniqueFunction.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/ValidationContext.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class IFieldSchemaSource;
	class Random;
	class TypeRegistry;

	// A type-level validation rule (registered with FieldBuilder::Validate), run after the per-field metadata checks.
	using StructValidator = UniqueFunction<void(const void* object, ValidationContext& context)>;

	// A type-level generation hook (registered with FieldBuilder::Generate): repairs an object whose fields
	// RandomValueGenerator has randomized so it also satisfies the type's validators and the constraints field metadata
	// cannot express (array element ranges, uniqueness, cross-field rules). It draws only from `random`.
	using StructGenerator = UniqueFunction<void(void* object, Random& random)>;

	// How reflected reads treat what they do not understand (Architecture §6 "Strict reader").
	struct ReadContext
	{
		// External schemas for Variant resolution (script field schemas, M13; a fixture source in tests). May be null.
		const IFieldSchemaSource* Schemas = nullptr;
		// --strict (CI): unknown members, unresolvable Variant values and Variant values that do not match their schema are
		// errors instead of warnings.
		bool Strict = false;
		// Receives the warnings (REFLECTION_UNKNOWN_FIELD, REFLECTION_VARIANT_UNRESOLVED, REFLECTION_VARIANT_MISMATCH) with
		// absolute JSON pointers. May be null, in which case warnings are dropped.
		std::vector<ValidationIssue>* Diagnostics = nullptr;
	};

	// A reflected struct (Architecture §5.4 "StructInfo"): the ordered fields and type-level validators of a registered C++
	// struct (ProjectSettings and its sections, PrefabOverride, InputActionSettings, every automation param/result struct
	// from M4, every *ImportSettings from M6), and the generic JSON, validation and patch operations on its objects.
	// ComponentInfo extends it with the ECS side.
	//
	// Field order is registration order, which the registration files keep equal to member declaration order. It is the
	// canonical key order of the struct's JSON (§6), the inspector order and the schema property order.
	//
	// Created by TypeRegistry::Struct<T> or TypeRegistry::Component<T>; fields and validators are added only before
	// TypeRegistry::Freeze, on one thread. Afterwards every const member is thread-safe; operations on an object follow
	// that object's threading rule (components: main thread, §4.11).
	class StructInfo
	{
	public:
		StructInfo(std::string name, std::string description, const TypeRegistry& registry);
		~StructInfo();

		StructInfo(const StructInfo&) = delete;
		StructInfo& operator=(const StructInfo&) = delete;

		[[nodiscard]] const std::string& GetName() const { return m_Name; }
		[[nodiscard]] const std::string& GetDescription() const { return m_Description; }
		[[nodiscard]] const TypeRegistry& GetRegistry() const { return *m_Registry; }

		// The struct's own TypeInfo (kind Struct), set by the registry at creation.
		[[nodiscard]] const TypeInfo& GetType() const;

		// The fields in registration order.
		[[nodiscard]] std::span<const Scope<FieldInfo>> GetFields() const { return m_Fields; }
		// The field named exactly `name`, or nullptr.
		[[nodiscard]] const FieldInfo* FindField(std::string_view name) const;
		// Up to `maxResults` field names close to `name`, best first (FuzzySuggest).
		[[nodiscard]] std::vector<std::string> SuggestFieldNames(std::string_view name, size_t maxResults = 3) const;

		// A schema-only field describing a whole object of this struct (kind Struct, named after the struct). It is what a
		// PrefabOverride's Value resolves to for an AddComponent override (§5.4) and what JsonSchema uses for $defs.
		[[nodiscard]] const FieldInfo& GetSelfField() const;

		// A new object of this struct with every field at its default member initializer.
		[[nodiscard]] ObjectPtr CreateDefault() const;

		// The canonical JSON object of `object`: every field with Meta.Serialized, in field order, default values included
		// (explicit beats implicit, §6). Errors: Validation for a non-finite float or an enum value without a name, located
		// at the field's pointer below the root. Never partial: on error nothing is returned.
		[[nodiscard]] Result<Json> ToJson(const void* object) const;

		// Reads the JSON object `reader` into `object`. Every member is optional: a missing field keeps the object's current
		// value (a default-constructed object therefore yields defaults). Each present field must have the field's JSON
		// spelling (ValueFromJson) and satisfy its metadata; Variant values are resolved through the field's resolver with
		// Owner = `object` (the fields declared before it already read), OwnerType = this struct, OwnerJson = `reader` and
		// context.Schemas (ResolveContext), and kept verbatim with a warning when unresolvable or mismatching.
		// Unknown members are REFLECTION_UNKNOWN_FIELD warnings with "did you mean" suggestions. The type-level validators
		// run on the result. Atomic: on error `object` is unchanged. Errors: Validation carrying every problem as an issue
		// (located at absolute pointers), including warnings promoted by context.Strict.
		[[nodiscard]] Status FromJson(void* object, const JsonReader& reader, const ReadContext& context) const;

		// Applies an RFC 7386 merge patch to `object` (§5.4). A member of the patch object replaces the field it names; a
		// null member resets a stored field to its default (the CreateDefault() value) and deletes a key of a Map field; an
		// object member merges recursively into a Struct field and key by key into a Map field; arrays are replaced whole.
		// One exception to RFC 7386 nesting: a member that targets a Variant field, or a value of a Map of Variants, replaces
		// that value whole even when both are objects, because a Variant's shape belongs to its resolved schema. The result
		// equals reading MergePatch(ToJson(object), patch), with Variant values treated as leaves, into a fresh
		// CreateDefault() object (so a key the merged document lacks reads as its default), and replaces `object` only on
		// success. Errors and atomicity as FromJson; a non-object patch is Validation.
		[[nodiscard]] Status ApplyMergePatch(void* object, const JsonReader& patch, const ReadContext& context) const;

		// Runs the field metadata checks and the type-level validators on `object`, adding issues to `context` at its current
		// pointer. `resolve` supplies Registry and Schemas; for each Variant field Validate itself sets Owner (`object`),
		// OwnerType (this struct) and Key, and descends into nested structs the same way. Read-only.
		void Validate(const void* object, const ResolveContext& resolve, ValidationContext& context) const;

		// True when the type has at least one Validate rule, or at least one Generate hook.
		[[nodiscard]] bool HasValidators() const { return !m_Validators.empty(); }
		[[nodiscard]] bool HasGenerators() const { return !m_Generators.empty(); }

		// Runs the Generate hooks on `object` in registration order. RandomValueGenerator::Randomize calls it after it has
		// randomized the object's fields (nested structs first, so an owner's hooks see repaired members); the object then
		// passes Validate. No-op for a type without hooks.
		void Generate(void* object, Random& random) const;

		// ToJson of CreateDefault(); never fails for a registered type (default member initializers are in range by
		// construction, which the registry suite checks).
		[[nodiscard]] Json MakeDefaultJson() const;

		// Registration only (before TypeRegistry::Freeze). AddField asserts a non-empty unique name, a non-empty description
		// and a non-null type, and sets Meta.AssetFilter of AssetRef fields from their type.
		void AddField(FieldInfo::Specification specification);
		void AddValidator(StructValidator validator);
		void AddGenerator(StructGenerator generator);
		// Sets the struct's own TypeInfo; called once by the registry when it creates the struct.
		void SetType(const TypeInfo& type);
	private:
		std::string m_Name;
		std::string m_Description;
		const TypeRegistry* m_Registry = nullptr; // back-reference to the owning registry
		const TypeInfo* m_Type = nullptr;
		std::vector<Scope<FieldInfo>> m_Fields; // Scope: field addresses stay stable (VariantSchemaResolver results)
		std::vector<StructValidator> m_Validators;
		std::vector<StructGenerator> m_Generators;
		Scope<FieldInfo> m_SelfField;
	};

}
