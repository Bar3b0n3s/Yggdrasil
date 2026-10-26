#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Native/NativeApi.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The engine's side of a native gameplay module (Docs/Decisions/0030-m19-contract.md decisions 26 and 27): the module's
// registration, validated and copied into an immutable NativeModuleTypes, and the conversions of a native value between its
// bytes and its canonical JSON, which scene files, automation, the native block of the state hash and the editor's
// migrations all use. Native (layer 4, like Scripting) holds no state of its own and includes Core, Reflection and Scene.

namespace Engine {

	class XXH64Hasher;

	// The limits a registration is checked against (decision 26).
	inline constexpr size_t MaxNativeNameBytes = 64;         // a type, field, system or enum entry name
	inline constexpr uint32_t MaxNativeTypeSize = 64 * 1024; // sizeof of a native type
	inline constexpr uint32_t MaxNativeTypeAlignment = 16;   // alignof of a native type
	inline constexpr size_t MaxNativeTypes = 256;            // components and resources together
	inline constexpr size_t MaxNativeFieldsPerType = 64;
	inline constexpr size_t MaxNativeSystems = 256;
	inline constexpr size_t MaxNativeEnumEntries = 256;

	// The version every native component is written with in a document's "ComponentVersions" (decision 27): a native type
	// has no version of its own, since its values migrate by field name.
	inline constexpr uint32_t NativeComponentVersion = 1;

	// One enum entry of a native field, copied from the module's NativeEnumEntry.
	struct NativeEnumValue
	{
		std::string Name{};
		int32_t Value = 0;
		std::string Description{};
	};

	// One reflected field of a native type, copied from the module's NativeFieldDescriptor.
	struct NativeFieldInfo
	{
		std::string Name{};
		std::string Description{};
		NativeFieldKind Kind = NativeFieldKind::Float;
		uint32_t Offset = 0;
		std::optional<double> Min{};
		std::optional<double> Max{};
		std::string Unit{};
		std::string RenamedFrom{};
		std::vector<NativeEnumValue> EnumValues{}; // Enum: in declaration order
	};

	// One native type, copied from the module's NativeTypeDescriptor and validated (BuildNativeModuleTypes).
	struct NativeTypeInfo
	{
		NativeTypeRole Role = NativeTypeRole::Component;
		uint32_t Index = 0;         // among the module's types of its role, in registration order
		std::string Name{};         // the module's name ("Spinner")
		std::string RegistryName{}; // a component's name in files and automation ("Native.Spinner"); the Name of a resource
		std::string Description{};
		uint32_t Size = 0;
		uint32_t Alignment = 0;
		std::vector<std::byte> Defaults{}; // Size bytes
		std::vector<NativeFieldInfo> Fields{};
	};

	// One system, copied from the module's NativeSystemDescriptor. Run points into the module, which NativeModuleTypes keeps
	// loaded (GetKeepalive).
	struct NativeSystemInfo
	{
		std::string Name{};
		std::string Description{};
		NativeSystemFunction Run = nullptr;
	};

	// A loaded module's registration (decisions 26 and 27): immutable, shared by the editor's host, every play session that
	// runs the module and every scene load that canonicalizes its components; thread-safe to read. It keeps the module's
	// code loaded (the keepalive given to BuildNativeModuleTypes, the editor's SharedLibrary) for as long as anything holds it,
	// so a system's Run never outlives its library.
	class NativeModuleTypes
	{
	public:
		// Restricts construction to BuildNativeModuleTypes; CreateRef still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend Result<Ref<const NativeModuleTypes>> BuildNativeModuleTypes(const NativeModuleEntryPoint& entry, Ref<const void> keepalive);
		};

		NativeModuleTypes(ConstructionKey key, std::vector<NativeTypeInfo> components, std::vector<NativeTypeInfo> resources,
			std::vector<NativeSystemInfo> systems, Ref<const void> keepalive);

		NativeModuleTypes(const NativeModuleTypes&) = delete;
		NativeModuleTypes& operator=(const NativeModuleTypes&) = delete;

		// In registration order.
		[[nodiscard]] std::span<const NativeTypeInfo> GetComponents() const { return m_Components; }
		[[nodiscard]] std::span<const NativeTypeInfo> GetResources() const { return m_Resources; }
		[[nodiscard]] std::span<const NativeSystemInfo> GetSystems() const { return m_Systems; }

		// The component whose registry name is `registryName` ("Native.Spinner"); null when the module registers none.
		[[nodiscard]] const NativeTypeInfo* FindComponent(std::string_view registryName) const;
		// The resource named `name`; null when the module registers none.
		[[nodiscard]] const NativeTypeInfo* FindResource(std::string_view name) const;

		// The XXH64 (seed 0) of the layout of every type: role, name and each field's name, kind, offset, bounds, renamed-from
		// name and enum entries, in registration order, and the size, alignment and defaults of each type, the defaults
		// through their canonical JSON minified (NativeValueToJson), never their raw bytes, so neither padding nor a member
		// that is not registered reaches it. Equal hashes mean equal canonical JSON for every value, so a reload whose hash is
		// unchanged rewrites no component (decision 31); two registrations of an unchanged module, in any process and build,
		// give the same hash.
		[[nodiscard]] uint64_t GetLayoutHash() const { return m_LayoutHash; }
	private:
		std::vector<NativeTypeInfo> m_Components{};
		std::vector<NativeTypeInfo> m_Resources{};
		std::vector<NativeSystemInfo> m_Systems{};
		Ref<const void> m_Keepalive{};
		uint64_t m_LayoutHash = 0;
	};

	// Runs the module's registration and validates it (decision 26). `keepalive` keeps the module's code loaded for the
	// result's lifetime (the editor's library; null for a module linked into the executable). Errors: UnsupportedVersion when
	// entry.ApiVersion is not NativeApiVersion (naming both); InvalidArgument for an entry without a Register function;
	// Validation for the first rule the registration breaks, naming the type, field or system: a name that is not PascalCase
	// ASCII ([A-Z][A-Za-z0-9]*) of at most MaxNativeNameBytes or that repeats among its kind (types of one role, fields of one
	// type, systems, the entries of one enum); an empty description; a size of 0 or above MaxNativeTypeSize, an alignment that
	// is not a power of two up to MaxNativeTypeAlignment or does not divide the size; a field outside the type, misaligned for
	// its kind or overlapping another; bounds on a kind without numbers, non-finite or with Min > Max; a RenamedFrom that is not
	// a valid name or names another field; an Enum field without entries or with more than MaxNativeEnumEntries; a default
	// value its field rejects (NativeValueFromJson's rules, an Entity default other than none); a system without Run; more
	// types, fields or systems than the limits; and a registration with neither a type nor a system. The registration is the
	// module's code, so it runs inside a try/catch (NativeModule.cpp is a §4.6 boundary file, Scripts/ModuleRules.json's
	// TryCatchFiles): an exception that escapes it refuses the module with Validation "the registration of the native module
	// raised an exception: <what>" (a std::exception's what(), else "an exception of an unknown type"), and nothing it
	// registered is kept. So a throwing registration fails native.load (the editor keeps its previous module) and the
	// Runtime's startup with an error, never terminating the process; a crash is not caught (decision 28).
	[[nodiscard]] Result<Ref<const NativeModuleTypes>> BuildNativeModuleTypes(const NativeModuleEntryPoint& entry, Ref<const void> keepalive);

	// The canonical JSON object of `value` (`type.Size` bytes of `type`): every field in registration order, each in its kind's
	// form (NativeFieldKind), floats as float like every authored number (§6). Pure.
	[[nodiscard]] Json NativeValueToJson(const NativeTypeInfo& type, std::span<const std::byte> value);

	// A note on a value that canonicalization changed (decision 27), located by its field.
	struct NativeValueNote
	{
		std::string Code{};    // NativeFieldDroppedCode or NativeFieldResetCode
		std::string Field{};   // the member of the JSON object it concerns
		std::string Message{}; // "dropped 'Spped': Native.Spinner has no such field"
	};

	// What reading a value found besides its result.
	struct NativeValueReadReport
	{
		std::vector<NativeValueNote> Notes{}; // in the order of the JSON object's members
		bool Changed = false;                 // the canonical JSON of the result differs from the JSON read
	};

	// Reads `json` into `value` (`type.Size` bytes), starting from the type's defaults, by field name (decision 27): a member
	// named like a field, or like a field's RenamedFrom while the field's own name is absent, sets that field; a member of the
	// right JSON type but another kind's form that converts exactly (an integer for a Float, a non-negative Int32 for a UInt32,
	// ...) is converted; a member that does not convert leaves the field at its default with a NATIVE_FIELD_RESET note; any
	// other member is dropped with a NATIVE_FIELD_DROPPED note. Errors (Validation, each issue located below `pointer`): `json`
	// is not an object; a value of a field's kind outside its bounds, non-finite, an Entity that is not 16 lowercase hex digits
	// or null, an enum name the field does not list, a quaternion that is not unit length within 1e-3. Pure.
	[[nodiscard]] Result<NativeValueReadReport> NativeValueFromJson(const NativeTypeInfo& type, const Json& json, std::string_view pointer,
		std::span<std::byte> value);

	// NativeValueFromJson then NativeValueToJson: the canonical form of a stored value, with its notes.
	struct NativeCanonicalValue
	{
		VariantValue Value{};
		NativeValueReadReport Report{};
	};
	[[nodiscard]] Result<NativeCanonicalValue> CanonicalizeNativeValue(const NativeTypeInfo& type, const Json& json, std::string_view pointer);

	// The JSON Schema (2020-12) of a native type's canonical JSON, with each field's description, kind, bounds, unit and enum
	// names, as component.schema gives a built-in component's (§13.4). Pure.
	[[nodiscard]] Json GetNativeTypeSchema(const NativeTypeInfo& type);

	// Appends `value` to the state hash (decision 28's native block): its registry name, then its canonical JSON minified by
	// JsonWriter, the bytes the scene part of the hash would hold for it. Pure.
	void AppendNativeValueHash(const NativeTypeInfo& type, std::span<const std::byte> value, XXH64Hasher& hasher);

	// The diagnostic codes of native values and components (LoadDiagnostic::Code, NativeValueNote::Code; decision 27).
	// Warning (an error with LoadOptions::StrictUnknowns): a "Native." component the loaded module does not register, or any
	// "Native." component while no module is loaded; it is kept as written.
	inline constexpr std::string_view NativeComponentUnregisteredCode = "NATIVE_COMPONENT_UNREGISTERED";
	// Error: a native component whose value its type rejects (NativeValueFromJson's errors); it is kept as written, and a
	// play session of its scene does not start.
	inline constexpr std::string_view NativeComponentInvalidCode = "NATIVE_COMPONENT_INVALID";
	// Warning (an error with StrictUnknowns): a member no field of the type is named after, dropped.
	inline constexpr std::string_view NativeFieldDroppedCode = "NATIVE_FIELD_DROPPED";
	// Warning (an error with StrictUnknowns): a member whose value does not convert to its field's kind, reset to the default.
	inline constexpr std::string_view NativeFieldResetCode = "NATIVE_FIELD_RESET";

}
