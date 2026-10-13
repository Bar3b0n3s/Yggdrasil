#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Testing/TestResults.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The FeatureTest coverage gates (Architecture §15.6), frozen by the M14 contract (Docs/Decisions/0022-m14-contract.md,
// decisions 2 to 6). The per-mode gates 1, 1b, 2, 3, 4 and 9 are evaluated by the FeatureTestRunner at the end of an
// unfiltered run of a project whose Testing.CoverageGates is true, from the counters of TestCoverageReport; the
// mode-independent gates 5 (Scripts/Test.py), 6 (the validation fixtures), 7 (EditorCore/Reference/DescriptionInventory.h)
// and 8 (the settings observation test) are checked outside FeatureTest runs. Everything here is present in Dist, pure
// apart from the registries and scenes it reads, and main-thread only where it reads a Scene or an AssetManager.
//
// Counter ids and kinds (TestCoverageCounter::Id and Kind). Each kind belongs to exactly one gate, and ids never collide
// across gates because each starts with its gate's prefix. Ids are byte-wise sorted in every report.
//
//   Gate  Kind(s)                                        Id
//   1     component                                      component:<Component>
//   1     field                                          field:<Component><pointer>    (pointer: the RFC 6901 JSON pointer,
//                                                        with its leading '/' and ~0/~1 escapes, of a serialized leaf
//                                                        below the component's JSON; every array element and every map
//                                                        value is the single token "*", so map keys never appear and a
//                                                        literal "*" key cannot collide with a field name; e.g.
//                                                        field:Camera/Projection, field:Prefab/Overrides/*/Kind)
//   1b    enum-value                                     enum:<Enum>/<Value>
//   2     function, method, operator, constructor        api:<Owner>.<Member>
//   2     property-read, property-write                  api:<Owner>.<Member>/read, api:<Owner>.<Member>/write
//   2     argument-value                                 api:<Owner>.<Member>/argument<N>/<Enum>/<Value>
//   3     callback                                       callback:<Owner>.<Member>
//   4     proxy-read, proxy-write                        proxy:<Component>.<Field>/read, proxy:<Component>.<Field>/write
//   9     format                                         format:<Importer>/<extension>   (e.g. format:Texture/.jpeg)
//
// Modes: the run modes in which the entry exists (§15.6 "every entry available in that mode"): components, enum values
// and formats exist in all three; fields and proxy fields follow FieldMeta::Modes; script API members and callbacks follow
// their registration's RunModes. Every entry exists in the Editor (RunModes values are EditorOnly or All; nothing is
// registered for Release or Dist alone), so the Editor report lists every counter of every mode, which the cross-mode merge
// relies on. A counter is reported in a mode's run only when that mode is in its Modes, so a Release or Dist report never
// lists an EditorOnly entry.

namespace Engine {

	class AssetManager;
	class Scene;
	class TypeRegistry;
	struct ScriptApiCoverage;

	// The gates of §15.6, in their documented order.
	enum class CoverageGate : uint8_t
	{
		Components,        // "1"
		EnumValues,        // "1b"
		ScriptApi,         // "2"
		Callbacks,         // "3"
		ProxyFields,       // "4"
		AutomationMethods, // "5"
		ValidationCodes,   // "6"
		Descriptions,      // "7"
		ProjectSettings,   // "8"
		Formats            // "9"
	};

	// The per-mode gates, in the order TestCoverageReport::Gates lists them.
	inline constexpr std::array<CoverageGate, 6> PerModeCoverageGates = { CoverageGate::Components, CoverageGate::EnumValues,
		CoverageGate::ScriptApi, CoverageGate::Callbacks, CoverageGate::ProxyFields, CoverageGate::Formats };

	// The counter kinds of the table above.
	inline constexpr std::string_view CoverageKindComponent = "component";
	inline constexpr std::string_view CoverageKindField = "field";
	inline constexpr std::string_view CoverageKindEnumValue = "enum-value";
	inline constexpr std::string_view CoverageKindFunction = "function";
	inline constexpr std::string_view CoverageKindMethod = "method";
	inline constexpr std::string_view CoverageKindOperator = "operator";
	inline constexpr std::string_view CoverageKindConstructor = "constructor";
	inline constexpr std::string_view CoverageKindPropertyRead = "property-read";
	inline constexpr std::string_view CoverageKindPropertyWrite = "property-write";
	inline constexpr std::string_view CoverageKindArgumentValue = "argument-value";
	inline constexpr std::string_view CoverageKindCallback = "callback";
	inline constexpr std::string_view CoverageKindProxyRead = "proxy-read";
	inline constexpr std::string_view CoverageKindProxyWrite = "proxy-write";
	inline constexpr std::string_view CoverageKindFormat = "format";

	// The text TestCoverageGateResult::Gate holds and reports print: "1", "1b", "2", ... "9".
	[[nodiscard]] std::string_view CoverageGateName(CoverageGate gate);
	// The gate CoverageGateName spells `name`; nullopt for any other text (exact, case-sensitive).
	[[nodiscard]] std::optional<CoverageGate> ParseCoverageGateName(std::string_view name);
	// True for 1, 1b, 2, 3, 4 and 9 (§15.6 "Per-mode evaluation").
	[[nodiscard]] bool IsPerModeCoverageGate(CoverageGate gate);
	// The gate a counter kind of the table above belongs to; nullopt for an unknown kind.
	[[nodiscard]] std::optional<CoverageGate> GetCoverageGateOfKind(std::string_view kind);

	// The TestMode values of the set bits of `modes`, in Editor, Release, Dist order; empty for RunModes::None.
	[[nodiscard]] std::vector<TestSuiteSettings::Mode> ToTestModes(RunModes modes);
	// The single RunModes bit of `mode`.
	[[nodiscard]] RunModes ToRunModes(TestSuiteSettings::Mode mode);

	// Evaluates the per-mode gates for a run in `mode` (§15.6). For each gate of PerModeCoverageGates, in that order, the
	// result lists, sorted by id, every counter of the gate's kinds whose Modes contain `mode` and whose Count is 0;
	// Evaluated is true and Passed is Missing.empty(). A gate with no counter available in `mode` at all fails with the
	// single Missing entry "<no counters>", because an absent producer must never pass vacuously. Counters whose Modes do
	// not contain `mode` are ignored (an EditorOnly entry in a Dist evaluation). Errors: Validation naming the counter for
	// an unknown kind, an empty Modes list, Modes without Editor, a component, enum-value or format counter whose Modes are
	// not all three modes, a duplicate id, or an id that does not start with its kind's prefix.
	[[nodiscard]] Result<std::vector<TestCoverageGateResult>> EvaluateCoverageGates(std::span<const TestCoverageCounter> counters,
		TestSuiteSettings::Mode mode);

	// The six per-mode gates with Evaluated and Passed false and no Missing entries: what a filtered, recording or
	// cancelled run of a coverage project reports (ADR 0022 decision 3). Never a pass.
	[[nodiscard]] std::vector<TestCoverageGateResult> MakeUnevaluatedCoverageGates();

	// Gates 2, 3 and 4 from the script API registry: one counter per entry of `current`, with the kind and id of the table
	// above (Function/Method/Operator/Constructor -> calls; Property -> property-read/-write and ProxyField ->
	// proxy-read/-write, each only where RequiresRead/RequiresWrite say, so a read-only field has no write counter; Callback
	// -> callback; each EnumValues entry -> argument-value) and Count the
	// difference from `baseline`, saturated to uint32. Both snapshots must come from the same frozen registry and mode
	// (ScriptApiRegistry::GetCoverage); `baseline` is the snapshot the host takes immediately before FeatureTestRunner::Begin
	// (decision 4). Constants have no counter. Sorted by id. Errors: InvalidArgument when the snapshots differ in Mode or in
	// their member lists, or when a count went backwards.
	[[nodiscard]] Result<std::vector<TestCoverageCounter>> MakeScriptApiCoverageCounters(const ScriptApiCoverage& current,
		const ScriptApiCoverage& baseline);

	// The producer of gates 1 and 1b. The FeatureTestRunner records every scene it simulates: each suite and replay session's
	// scene after its setup, again after every scene replacement (a PlaySession::GetSceneGeneration change) and before its
	// teardown. It creates the recorder at the first recorded scene, from that scene's Scene::GetTypeRegistry() (the host
	// exposes no registry; every session of one host shares it, and a scene with another registry fails the run with
	// InvalidState). Golden scenes are rendered only by the doctest goldens and never count. Not copyable; main thread.
	class SceneCoverageRecorder
	{
	public:
		// Every component of `types` (TypeRegistry::GetComponents, hidden and entity-level ones included), every serialized
		// field leaf of each (FieldMeta::Serialized; recursing into structs and into the elements of arrays and maps of
		// structs, other values compared whole) and every value of every registry enum such a field holds becomes a counter
		// with Count 0. `types` is frozen and is a documented back-reference that outlives the recorder.
		explicit SceneCoverageRecorder(const TypeRegistry& types);
		~SceneCoverageRecorder();

		SceneCoverageRecorder(const SceneCoverageRecorder&) = delete;
		SceneCoverageRecorder& operator=(const SceneCoverageRecorder&) = delete;

		// Counts, over every entity of `scene` in canonical order: each component it has; each field leaf whose value differs
		// from its default, compared per leaf as above (a top-level leaf against the component's StructInfo::MakeDefaultJson,
		// a leaf inside an array or map element against the element struct's MakeDefaultJson, since the component's default
		// has no elements); and each enum value a serialized field holds. Errors: those of serializing a component to JSON,
		// naming the entity and component.
		[[nodiscard]] Status Record(const Scene& scene);

		// One counter per entry of the constructor's list, sorted by id, with the kinds component, field and enum-value.
		[[nodiscard]] std::vector<TestCoverageCounter> GetCounters() const;

		// Sets every count back to 0 (FeatureTestRunner::Begin).
		void Reset();
	private:
		struct State;
		Scope<State> m_State{};
	};

	// The producer of gate 9. The FeatureTestRunner installs AssetManager::SetLoadObserver for the run and forwards each
	// load here. Not copyable; main thread.
	class FormatCoverageRecorder
	{
	public:
		// One counter with Count 0 per importable source format (GetSourceFormats, Importable true), kind format.
		FormatCoverageRecorder();
		~FormatCoverageRecorder();

		FormatCoverageRecorder(const FormatCoverageRecorder&) = delete;
		FormatCoverageRecorder& operator=(const FormatCoverageRecorder&) = delete;

		// Counts the load of `handle` by `assets` once per handle: a handle seen since the last Reset returns at once, so the
		// per-frame loads of a render path cost one set lookup and counts never depend on how many frames ran. For a new
		// handle, the extension of AssetManager::GetReferencePath (a const query, which the load observer may make) before any
		// '#' sub-asset suffix, ASCII lower-cased, selects the source format (FindSourceFormat). Engine assets (engine://),
		// procedural built-ins, unknown handles and extensions without a source format are ignored, so the call never fails.
		// Any project path counts: the foreign formats live in Assets/Formats/ by convention (decision 5), the native ones
		// anywhere.
		void RecordLoad(const AssetManager& assets, AssetHandle handle);

		// One counter per importable source format, sorted by id; Count is the number of distinct handles of that format
		// loaded since the last Reset.
		[[nodiscard]] std::vector<TestCoverageCounter> GetCounters() const;

		// Sets every count back to 0 and forgets the handles seen.
		void Reset();
	private:
		struct State;
		Scope<State> m_State{};
	};

}
