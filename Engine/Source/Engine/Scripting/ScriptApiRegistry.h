#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Reflection/Value.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	struct ScriptCall;
	class ScriptModuleBuilder;
	class ScriptProxy;
	class ScriptTypeBuilder;
	class TypeRegistry;

	// Engine callback ABI. ScriptCall is defined only in Scripting/Private/ScriptCall.h; public headers never name VM
	// types. A callback returns its stack result count (or the VM yield sentinel for an explicitly yieldable API).
	// The registry supplies the VM trampoline, error context, access checks and counters. Runtime callbacks are main-thread;
	// isolated pure load-time callbacks run on their VM's owner thread. Callbacks are
	// exception-neutral and run only inside a protected entry; neither the call nor any borrowed stack data may escape.
	using ScriptNativeFunction = int (*)(ScriptCall& call);

	// Availability is independent of RunModes and of read-only evaluation. Runtime includes ordinary play and eval;
	// Test includes test runs. Pure constructors, including Test.Suite, opt into All. Other Test members are Test-only.
	enum class ScriptApiEnvironment : uint8_t
	{
		None = 0,
		LoadTime = 1 << 0,
		Runtime = 1 << 1,
		Test = 1 << 2,
		All = LoadTime | Runtime | Test
	};

	template<>
	inline constexpr bool EnableFlagOperators<ScriptApiEnvironment> = true;

	enum class ScriptApiMemberKind : uint8_t
	{
		Function,
		Method,
		Property,
		Operator,
		Constructor,
		Callback,
		Constant,
		ProxyField
	};

	// ArgumentIndex is the positive VM stack slot: functions start at 1; methods include self in slot 1. The named enum
	// is registered once (or imported from TypeRegistry). Optional nil/absent arguments use DefaultValue when nonempty;
	// their canonical default is counted. Otherwise omitted optional arguments do not count any value. No parallel table.
	struct ScriptEnumParameter
	{
		int ArgumentIndex = 0;
		std::string EnumName{};
		bool Optional = false;
		std::string DefaultValue{};
	};

	struct ScriptMemberOptions
	{
		RunModes Modes = RunModes::All;
		ScriptApiEnvironment Environments = ScriptApiEnvironment::Runtime | ScriptApiEnvironment::Test;
		// Defaults fail closed for functions/methods. Pure registrations explicitly clear Mutates. Properties and value
		// constructors/operators have the different defaults at their builder declarations below. A setter has its own
		// policy: host setters always mutate, whereas Color/Quat/local-generator operations can be local and permitted.
		// These flags gate write permission, not recorder invalidation. Test.Inject* mutates but remains recorded input;
		// actual gameplay-state/shared-random writes notify through ScriptCall's origin-aware private guard after validation.
		bool Mutates = true;
		bool SetterMutates = true;
		std::vector<ScriptEnumParameter> EnumParameters{};
	};

	// Immutable after Freeze. Signature is the canonical Luau function type (generic/intersection overloads allowed),
	// or the value type for a property/constant. Method/operator signatures include self. Function is the callable or
	// property getter; Setter is optional. Generated proxy fields/shortcuts and callbacks use registry-owned dispatch
	// instead of a user callback. ConstantValue exists only for Constant. ComponentTypeIndex identifies generated
	// reflection entries; no metadata is authored a second time for fields, shortcuts, enums, docs or declarations.
	struct ScriptApiMember
	{
		std::string Name{};
		ScriptApiMemberKind Kind = ScriptApiMemberKind::Function;
		std::string Signature{};
		std::string Description{};
		ScriptMemberOptions Options{};
		ScriptNativeFunction Function = nullptr;
		ScriptNativeFunction Setter = nullptr;
		bool Readable = false;
		bool Writable = false;
		std::optional<size_t> ComponentTypeIndex{};
		std::optional<Value> ConstantValue{};
	};

	struct ScriptApiGroup
	{
		std::string Name{};
		std::string Description{};
		std::vector<ScriptApiMember> Members{};
	};

	struct ScriptApiEnum
	{
		std::string Name{};
		std::string Description{};
		std::vector<EnumEntry> Values{};
	};

	// Named structural/value types used by signatures (RaycastHit, Field options, test result records). Registered next
	// to their bindings once; no runtime dispatch slot or call counter. Definition is a Luau type expression, not a
	// second definition file. Enum unions and reflected component fields are generated separately from their owners.
	struct ScriptApiAlias
	{
		std::string Name{};
		std::string Definition{};
		std::string Description{};
	};

	// Per-member, per-argument value coverage for enums with <= 16 values. Larger tables remain in GetEnums and the
	// generated definitions, but get table-driven unit tests instead of impossible FeatureTest value requirements.
	struct ScriptEnumCounter
	{
		int ArgumentIndex = 0;
		std::string EnumName{};
		std::string ValueName{};
		uint64_t Count = 0;
	};

	struct ScriptApiCounter
	{
		std::string Owner{};
		std::string Member{};
		ScriptApiMemberKind Kind = ScriptApiMemberKind::Function;
		RunModes Modes = RunModes::All;
		uint64_t Calls = 0;
		uint64_t Reads = 0;
		uint64_t Writes = 0;
		bool RequiresRead = false;
		bool RequiresWrite = false;
		std::vector<ScriptEnumCounter> EnumValues{};
	};

	struct ScriptApiCoverage
	{
		RunModes Mode = RunModes::None;
		std::vector<ScriptApiCounter> Members{};
	};

	// One registration graph produces dispatch, docs, Engine.d.luau and coverage (§11.4/§11.9). No global registry.
	// Register on a single owning thread (including an isolated import worker), Freeze against an already frozen
	// TypeRegistry, then bind any number of VMs. The
	// TypeRegistry is a borrowed back-reference and must outlive this registry; this registry must outlive every bound
	// VM. Immutable metadata may be read concurrently after Freeze; each VM binds only on its owner thread. Pure
	// load-time binding is safe on isolated workers and never touches counters; runtime counters are main-thread-only.
	// Builders borrow this nonmovable owner and last only for a registration statement. Strings/options are copied.
	class ScriptApiRegistry
	{
	public:
		ScriptApiRegistry();
		~ScriptApiRegistry();
		ScriptApiRegistry(const ScriptApiRegistry&) = delete;
		ScriptApiRegistry& operator=(const ScriptApiRegistry&) = delete;
		ScriptApiRegistry(ScriptApiRegistry&&) = delete;
		ScriptApiRegistry& operator=(ScriptApiRegistry&&) = delete;

		// Reopening a group appends members; a repeated nonempty description must match. Type descriptions may be
		// omitted only for reflected component types: Freeze derives those from TypeRegistry. All other descriptions
		// are mandatory. Registration after Freeze is a programmer error, with no mutation even without asserts.
		[[nodiscard]] ScriptModuleBuilder Module(std::string_view name, std::string_view description);
		[[nodiscard]] ScriptTypeBuilder Type(std::string_view name, std::string_view description = {});
		// Copies a non-reflected enum's single authoritative table. Reflected enums are imported automatically by Freeze.
		// Duplicate names return AlreadyExists; malformed metadata returns Validation; a frozen registry returns InvalidState.
		[[nodiscard]] Status RegisterEnum(const EnumInfo& enumeration);
		// Declares an auxiliary named type from a single binding registration. Same failure rules as RegisterEnum;
		// Freeze resolves referenced names and rejects alias/enum/userdata type collisions and invalid type expressions.
		[[nodiscard]] Status RegisterAlias(std::string_view name, std::string_view definition, std::string_view description);

		// Imports scriptable, nonhidden fields, eligible Entity shortcuts, GetComponent literal overloads and reflected
		// enums. Validates signatures, descriptions, callbacks, enum slots/defaults, modes and mutation policies before
		// publishing anything. Failures return Validation (or InvalidState for an unfrozen types registry). Duplicate
		// members and Entity/shortcut collisions are startup programmer errors (assert plus Validation fallback).
		// Read-only reflected fields never require writes. Field RunModes are inherited. Repeating a successful Freeze
		// with the same TypeRegistry is idempotent; another registry is InvalidState. No partial freeze on failure.
		[[nodiscard]] Status Freeze(const TypeRegistry& types);
		[[nodiscard]] bool IsFrozen() const;
		[[nodiscard]] std::span<const ScriptApiGroup> GetModules() const;
		[[nodiscard]] std::span<const ScriptApiGroup> GetTypes() const;
		[[nodiscard]] std::span<const ScriptApiEnum> GetEnums() const;
		[[nodiscard]] std::span<const ScriptApiAlias> GetAliases() const;
		[[nodiscard]] const ScriptApiEnum* FindEnum(std::string_view name) const;

		// Called inside a protected native setup entry, with live call.State and call.Sandbox. LoadTime permits null
		// call.Engine and never needs an IScriptHost; Test requires call.Engine. Standalone Runtime with neither Engine nor
		// host permits normally eligible members only when their metadata also includes LoadTime. Other APIs reject before
		// callback or host access with a located "requires an active script engine" error. This path retains the Runtime
		// environment and watchdog budget and never updates coverage counters; it does not become a LoadTime VM.
		// Installs tagged userdata, metatables, atoms and registry trampolines before sandboxing.
		// Select exactly one environment and one concrete RunModes bit
		// (Editor/Release/Dist). Disallowed load-time APIs have located rejection stubs, not missing globals. Runtime
		// installs located rejection stubs for Test-only members; Test.Suite remains a pure constructor in every environment.
		// Registry readiness/arguments return InvalidState/InvalidArgument; VM errors unwind
		// to the surrounding protected entry. Read-only checks happen BEFORE native callbacks and again at write helpers.
		// Dispatch never invalidates input recording merely for a Mutates flag; the actual host-write boundary does that.
		[[nodiscard]] Status Bind(ScriptCall& call, ScriptApiEnvironment environment, RunModes mode);

		// Requires Freeze, else InvalidState. Pure, deterministic strings in byte-wise group/member name order (kind is
		// the tie-breaker); enum values preserve their authoritative order. Definitions use Luau 0.741 declare extern
		// type syntax, singleton enum unions, overloads, generic Script.Define and mode/availability annotations.
		[[nodiscard]] Result<std::string> GenerateDefinitions() const;
		[[nodiscard]] Result<std::string> GenerateDocumentation() const;

		// Dispatch updates counters only with nonnull call.Engine and IsTestMode(), in every configuration; load-time pure
		// calls never access an engine or increment coverage. Counters are independent
		// per concrete mode and never increment for rejected availability/read-only checks. Calls count entry; reads and
		// writes count successful access. Dispatch validates every enum-slot registration before entering native code;
		// enum values count successful validation once and use canonical spellings. Typed helpers never double-count. Proxy
		// fields have independent read/write requirements (gate 4); callbacks are gate 3; constants are not call targets.
		// Metadata-derived zero entries are included. No hand-maintained coverage target list is accepted. GetCoverage
		// returns InvalidState before Freeze and InvalidArgument unless mode is exactly Editor, Release or Dist.
		[[nodiscard]] Result<ScriptApiCoverage> GetCoverage(RunModes mode) const;
		void ResetCoverage();
		// ScriptEngine uses the same registered callback entries when invoking optional lifecycle callbacks. Records an
		// actual invocation only in test mode; unknown/noncallback entries are NotFound and a wrong mode is InvalidArgument.
		[[nodiscard]] Status RecordCallback(ScriptCall& call, std::string_view type, std::string_view name, RunModes mode);
	private:
		// Called only by the proxy's successful reflected access path, not additionally by its dispatch trampoline.
		// Uses call.Engine's test flag/run mode; an absent engine is InvalidState. No duplicate field metadata is accepted.
		[[nodiscard]] Status RecordProxyAccess(ScriptCall& call, size_t componentTypeIndex, std::string_view field, bool write);
	private:
		struct Storage;
	private:
		Scope<Storage> m_Storage{};
	private:
		friend class ScriptModuleBuilder;
		friend class ScriptProxy;
		friend class ScriptTypeBuilder;
	};

	class ScriptModuleBuilder
	{
	public:
		ScriptModuleBuilder& Function(std::string_view name, ScriptNativeFunction function, std::string_view signature,
			std::string_view description, ScriptMemberOptions options = {});
		// Numeric/boolean/string constants (Math.Pi, Deg2Rad, Rad2Deg); copied values, never independent declaration text.
		ScriptModuleBuilder& Constant(std::string_view name, const Value& value, std::string_view type,
			std::string_view description, RunModes modes = RunModes::All,
			ScriptApiEnvironment environments = ScriptApiEnvironment::All);
	private:
		ScriptModuleBuilder() = default;
	private:
		friend class ScriptApiRegistry;
	};

	class ScriptTypeBuilder
	{
	public:
		ScriptTypeBuilder& Method(std::string_view name, ScriptNativeFunction function, std::string_view signature,
			std::string_view description, ScriptMemberOptions options = {});
		// Getter required, setter null for read-only; setter receives self at slot 1 and value at slot 2. Options.Mutates
		// controls the getter, SetterMutates the setter. Reflected fields/shortcuts are derived by Freeze, never repeated.
		ScriptTypeBuilder& Property(std::string_view name, ScriptNativeFunction getter, ScriptNativeFunction setter,
			std::string_view type, std::string_view description, ScriptMemberOptions options = { .Mutates = false });
		ScriptTypeBuilder& Operator(std::string_view name, ScriptNativeFunction function, std::string_view signature,
			std::string_view description, ScriptMemberOptions options = { .Mutates = false });
		ScriptTypeBuilder& Constructor(std::string_view name, ScriptNativeFunction function, std::string_view signature,
			std::string_view description, ScriptMemberOptions options = { .Mutates = false });
		// Optional script-defined lifecycle callback, not a C++ native function. OnHotReload is EditorOnly.
		ScriptTypeBuilder& Callback(std::string_view name, std::string_view signature, std::string_view description,
			ScriptMemberOptions options = {});
	private:
		ScriptTypeBuilder() = default;
	private:
		friend class ScriptApiRegistry;
	};

}
