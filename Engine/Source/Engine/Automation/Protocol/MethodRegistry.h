#pragma once

#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UniqueFunction.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/TypeInfo.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// The method registry (Architecture §13.4, §1.3 principle 2): the single source of truth for automation methods. It drives
// dispatch, parameter validation, rpc.discover, the MCP catalogue (Tools/MCP/catalog.json through --dump-reference) and
// method coverage (gate 5, §15.6).
//
// Param-struct conventions (frozen by the M4 contract; ADR 0008 decision 8). Every method has exactly one reflected
// params struct and one reflected result struct, so parsing, defaults, validation and schemas are generic and no handler
// touches raw JSON plumbing:
//   1. Names. C++ `<Domain><Verb>Params` / `<Domain><Verb>Result` ("entity.create" -> EntityCreateParams,
//      EntityCreateResult), registered in the context's TypeRegistry under the same names, with mandatory descriptions.
//      Nested structs shared between methods (EntitySummary, ...) are registered once, before their users. A name an
//      engine type already takes gets "Method" before its suffix (log.read -> LogReadMethodResult, because Core has
//      LogReadResult).
//   2. Keys. Registered field names are the JSON keys: camelCase ("removeComponents"); C++ members stay PascalCase. Data
//      embedded in a request or result (component values, project settings, asset values) keeps its registry PascalCase.
//   3. Presence. Every member is optional on read and defaults to its member initializer. Members a call cannot do
//      without are listed in MethodSpecification::RequiredParams and checked before parsing (InvalidParams naming each
//      missing one). Where absence means "leave unchanged" (entity.update's name, active, tags) the handler asks
//      MethodContext::HasParam.
//   4. Reserved members. "dryRun", "ifRevision" and "_meta" are never declared: PrepareParams moves them into
//      RequestOptions and RequestInfo (MethodContext.h).
//   5. Strictness. Unknown members are InvalidParams with "did you mean" hints (§13.3 example: "/components/RigidBody/Mas:
//      unknown field 'Mas' on 'RigidBody'"); floats are finite and within their FieldMeta range.
//   6. Enums. Registered enums, parsed case-insensitively anywhere in the params (embedded component data included) and
//      echoed in canonical PascalCase (§6, §13.4): PrepareParams rewrites every recognized spelling to the registry name
//      before strict parsing, so handlers and ComponentAccess only ever see canonical names.
//   7. Entity references. std::string members holding an EntityRef (§13.4: 16 hex digits, a unique prefix of at least 6,
//      or a path such as "/Game/Board"); the host resolves them (EditorMethodContext::ResolveEntity).
//   8. Paths. std::string members holding a project-relative path ("Assets/Scenes/Main.scene") or a project:// path; the
//      host confines them to project:// (§13.2 "Paths"). project.create and project.open take native paths.
//   9. Lists. "limit" (uint32_t, default 100, Min 1, Max 1000) and "cursor" (std::string, opaque, "" for the start);
//      results carry "nextCursor" (std::string; "" when there is nothing more). Log and event cursors are the decimal
//      64-bit sequence numbers of their logs, so they never saturate, and also accept "end" (ObserveMethods.h).
//  10. Component values. std::map<std::string, VariantValue> registered with VariantField and ResolveComponentValue: each
//      key is a component registry name and its value that component's (partial) JSON, so validation, enum spelling and
//      "unknown field" hints work inside it. A value may also set the component's writable virtual fields
//      (Transform.EulerAngles, WorldPosition), each checked against its own field; read-only virtual fields are errors.
//      The handler applies virtual fields through their setters (ComponentAccess::SetFieldValue). Full schemas reference
//      the component $defs, which list the writable virtual fields too; compact (MCP) schemas show a free-form object
//      (§13.8 "Compact tool schemas").
//  11. Polymorphic members ("fix": true | [ids], "components": [...] | "all") are free-form VariantValue members that the
//      handler validates, reporting InvalidArgument located at the member's pointer.
//  12. Results. Canonical ids (16 hex digits) plus readable names and paths (§13.4); every edit-scene mutation reports
//      "undoIndex" (uint32_t: the CommandHistory sequence number saturated with ToAutomationCounter, ADR 0008 decision 8;
//      0 when nothing was recorded, as in a dry run). Revisions in results are EditorContext::GetRevision saturated the
//      same way; "_meta".revision and "ifRevision" carry the full 64 bits and agree with them below 2^32. The Dispatcher
//      adds "_meta" (and "dryRun": true for dry runs), so result structs never declare them.
//  13. Asset references. AssetRef values anywhere in the params (embedded component data included) accept the spellings
//      of §7.1: 16 hex digits (a handle, kept as given), a project path "Assets/..." with an optional "#<sub-asset key>",
//      or an engine path "engine://...". Invoke rewrites every other spelling to the handle it names through the
//      context's IAssetReferenceResolver (MethodContext::GetAssetReferenceResolver), which also checks the asset's type
//      against the field's; a host without one (null) leaves them for the strict reader, which accepts only handles. A
//      handle that names no asset is accepted (the asset may be missing; §7.2's placeholder stands in), a path that names
//      none is an error: NotFound when every unresolved reference names nothing, InvalidParams for a malformed reference
//      or an asset of another type, and any other resolver error (InvalidState for a project path without a project, a
//      refresh's Io) with its own code, each located at its value. A std::string asset param (asset.*, prefab.*) is
//      resolved by the handler with Utils::ResolveAssetParam (EditorCore Automation/Private/AssetMethodSupport.h), the
//      same rule.
//      If a supplied Variant needs external field schemas, Invoke acquires the host's immutable request snapshot after
//      resolving ordinary asset paths, then canonicalizes schema-defined enums/assets and uses that same source for
//      ValidateJson and FromJson. Snapshot acquisition errors keep their host error code; the handler does not run.
//      Registry-only params never request a snapshot (MethodContext::GetFieldSchemaSnapshot).
//      For partial patches the host may complete absent schema owner discriminators from the addressed object first
//      (CompleteParameterOwners); explicit values always win and all completed params still undergo strict validation.

namespace Engine {

	class StructInfo;
	class TypeRegistry;

	// One example call shown by rpc.discover and the generated reference (§13.5 "examples").
	struct MethodExample
	{
		std::string Description{};
		Json Params{}; // an object that passes the method's params schema (the registry tests check this)
	};

	// What a registration declares about a method (§13.4 metadata).
	struct MethodSpecification
	{
		// "domain.verb": a lowercase domain, a dot, a camelCase verb ("project.getSettings"). Unique.
		std::string Name{};
		// Mandatory, one or two sentences; rpc.discover, the MCP tool description and gate 7 use it.
		std::string Description{};
		// camelCase members that must be present (convention 3), in the order they are reported.
		std::vector<std::string> RequiredParams{};
		// Proxied by the MCP bridge as the tool "domain_verb" (§13.8, with the verb converted to snake_case).
		bool ExposeAsTool = false;
		// Changes project files or the open scene's content (an undoable edit or a write). Read-only editors and the
		// deny-mutations switch refuse these with PermissionDenied (Unauthorized) unless the call is a dry run. Methods that
		// change only what the editor shows (scene.open, edit.select) or write only when asked (session.shutdown {save},
		// project.validate {fix}) are not flagged: EditorContext refuses their writes in read-only editors instead. Every
		// method accepts ifRevision, whatever this flag.
		bool Mutates = false;
		// Accepts "dryRun": true (§13.4). Usually a Mutates method; project.validate supports it without being one (it changes
		// something only when asked to fix).
		bool SupportsDryRun = false;
		// Also served by the Runtime's automation subset (§13.5; from M7).
		bool AvailableInRuntime = false;
		// Callable in the launcher state, before a project is open (§12.1: session.*, rpc.discover, docs.get,
		// project.create, project.open); every other method answers InvalidState "no project open" there.
		bool AvailableInLauncher = false;
		// May be an op of edit.batch (§13.4 "Atomic batch"): set only for pure reads and for methods whose every effect goes
		// through EditorContext::Execute (entity.*, project.setSettings, project.validate's fixes), so the batch's
		// transaction rolls all of it back. Methods that replace the open scene, write files directly, record provenance
		// outside a command or end the session (scene.open, scene.new, scene.save, project.save, project.upgrade,
		// session.shutdown) are not, nor is edit.batch itself. Requires !pending (asserted).
		bool AllowedInBatch = false;
		// A test hook (debug.*): registered only with --automation-test-hooks, never a tool, never in the catalogue, and
		// excluded from method coverage (Roadmap M4). Requires !ExposeAsTool (asserted).
		bool TestHook = false;
		// How long the MCP bridge waits for the call (§13.8, default 60); a pending method sets its own bound.
		uint32_t TimeoutSeconds = 60;
		std::vector<MethodExample> Examples{};
	};

	// A registered method: its specification and what the registry derived.
	struct MethodDescriptor
	{
		MethodSpecification Specification{};
		std::string Domain{};               // the part of the name before the dot
		const StructInfo* Params = nullptr; // the registered params struct
		const StructInfo* Result = nullptr; // the registered result struct (what a pending operation resolves to)
		bool Pending = false;               // registered with AddPending
	};

	// How much detail a generated schema carries.
	enum class SchemaStyle : uint8_t
	{
		Full,   // rpc.discover and the reference: $defs per component and per struct (§13.4)
		Compact // the MCP catalogue's inputSchema: component and asset values are free-form objects (§13.8)
	};

	// Prepared params: what PrepareParams leaves for the handler, and the reserved members it took out.
	struct PreparedParams
	{
		Json Params{};
		RequestOptions Options{};
	};

	// The VariantSchemaResolver of params members that map component registry names to component values (convention 10):
	// resolves context.Key through context.Registry->FindComponent to that component's self field (StructInfo::
	// GetSelfField), so the value is read as the component's JSON with every member optional. Errors: NotFound with "did you
	// mean" suggestions for an unknown component name; InvalidArgument for an empty key or a missing registry.
	[[nodiscard]] Result<const FieldInfo*> ResolveComponentValue(const ResolveContext& context);

	// The registry. Lifecycle: built on the main thread with a TypeRegistry that holds every params and result struct
	// (registered before that registry froze, EngineContextSpecification::RegisterTypes), filled by Add/AddPending, then
	// frozen; afterwards it is immutable and every const member is thread-safe. Methods are kept in name order. Not copyable
	// or movable (descriptors are handed out by pointer).
	class MethodRegistry
	{
	public:
		// A type-erased handler: reads nothing itself, receives the parsed params object of the method's params struct.
		using Invoker = UniqueFunction<MethodResult(MethodContext& context, const void* params)>;

		// `types` must be frozen (asserted) and outlive the registry (documented back-reference).
		explicit MethodRegistry(const TypeRegistry& types);
		~MethodRegistry();

		MethodRegistry(const MethodRegistry&) = delete;
		MethodRegistry& operator=(const MethodRegistry&) = delete;
		MethodRegistry(MethodRegistry&&) = delete;
		MethodRegistry& operator=(MethodRegistry&&) = delete;

		// Registers an immediate method. Host is the context type the handler takes: the host's own MethodContext subclass
		// (EditorMethodContext), or a base of it that several hosts share (MethodContext::IsHostType). Params and Result are
		// registered structs (asserted, naming the method). The registration is a programmer error and asserted when the
		// name is malformed or taken, the description is empty, a required param is not a field of Params, the flags
		// contradict each other (TestHook with ExposeAsTool, AllowedInBatch on a pending method, SupportsDryRun on a pending
		// method), or the registry is frozen.
		//     methods.Add<EditorMethodContext, EntityCreateParams, EntityCreateResult>({ .Name = "entity.create", ... },
		//         &Automation::EntityCreate);
		template<typename Host, typename Params, typename ResultType>
		void Add(MethodSpecification specification, Result<ResultType> (*handler)(Host& context, const Params& params))
		{
			static_assert(std::is_base_of_v<MethodContext, Host>, "a method's host context derives from MethodContext");
			ENGINE_CORE_ASSERT(handler != nullptr, "Method '{}' needs a handler", specification.Name);
			AddEntry(std::move(specification), TypeKeyOf<Host>(), TypeKeyOf<Params>(), TypeKeyOf<ResultType>(), false,
				[handler](MethodContext& context, const void* params) -> MethodResult
			{
				Result<ResultType> result = handler(static_cast<Host&>(context), *static_cast<const Params*>(params));
				if (!result)
					return std::move(result).error();
				Result<Json> json = context.SerializeResult(*result);
				if (!json)
					return std::move(json).error();
				return std::move(*json);
			});
		}

		// Registers a method that resolves later (§13.2): the handler validates, starts the work and returns the operation,
		// whose outcome is the JSON of ResultType. Same rules as Add; a pending method cannot support dry runs (asserted).
		template<typename Host, typename Params, typename ResultType>
		void AddPending(MethodSpecification specification, Result<Scope<PendingOperation>> (*handler)(Host& context, const Params& params))
		{
			static_assert(std::is_base_of_v<MethodContext, Host>, "a method's host context derives from MethodContext");
			ENGINE_CORE_ASSERT(handler != nullptr, "Method '{}' needs a handler", specification.Name);
			AddEntry(std::move(specification), TypeKeyOf<Host>(), TypeKeyOf<Params>(), TypeKeyOf<ResultType>(), true,
				[handler](MethodContext& context, const void* params) -> MethodResult
			{
				Result<Scope<PendingOperation>> operation = handler(static_cast<Host&>(context), *static_cast<const Params*>(params));
				if (!operation)
					return std::move(operation).error();
				return std::move(*operation);
			});
		}

		// Ends registration (asserts that every method has an example whose params pass PrepareParams, so rpc.discover never
		// shows an invalid example). Idempotent.
		void Freeze();
		[[nodiscard]] bool IsFrozen() const { return m_IsFrozen; }

		// The method named exactly `name`, or nullptr.
		[[nodiscard]] const MethodDescriptor* Find(std::string_view name) const;
		// Up to `maxResults` method names close to `name`, best first (FuzzySuggest), for MethodNotFound hints.
		[[nodiscard]] std::vector<std::string> SuggestMethodNames(std::string_view name, size_t maxResults = 3) const;
		// Every method, in name order.
		[[nodiscard]] std::span<const MethodDescriptor* const> GetMethods() const { return m_Methods; }
		// The distinct domains, in name order.
		[[nodiscard]] std::vector<std::string> GetDomains() const;

		[[nodiscard]] const TypeRegistry& GetTypes() const { return *m_Types; }

		// Prepares `params` (null reads as {}) for `method`, in this order: params must be an object; the reserved members
		// are taken out (dryRun must be a boolean and requires SupportsDryRun, ifRevision a non-negative integer, accepted by
		// every method); every RequiredParams member must be present; every enum spelling recognized by the params struct's schema
		// (resolved Variant values included) is rewritten to its canonical name. Errors: InvalidArgument (InvalidParams) with
		// one issue per problem, located at its pointer; Unsupported (dryRun on a method without SupportsDryRun). Pure.
		[[nodiscard]] Result<PreparedParams> PrepareParams(const MethodDescriptor& method, const Json& params) const;

		// Runs `context`'s method: checks context.IsHostType(the handler's Host key) (asserted: a host registers only handlers
		// typed on its own context or on one of its bases); when context.GetAssetReferenceResolver() is non-null, rewrites
		// every AssetRef value that is not a handle to the handle it names (convention 13) and stores the result as the
		// context's params; reads context.GetParams() into a new params object (StructInfo::FromJson, strict: unknown members
		// are errors), and calls the handler. Errors, the handler not running: a resolver error other than NotFound and
		// InvalidArgument (InvalidState without a project, a refresh's Io) unchanged, located at its value; NotFound when
		// every other unresolved reference names nothing; otherwise InvalidArgument (reported as InvalidParams) with one
		// issue per reference, located at its value; a parse failure as InvalidArgument carrying every issue. Does not catch
		// exceptions: the Dispatcher's allowlisted boundary does (§4.6 item 5).
		[[nodiscard]] MethodResult Invoke(MethodContext& context) const;

		// One op of edit.batch (§13.4). Before anything runs it checks, in order: `method` exists (NotFound with
		// suggestions); it is AllowedInBatch (InvalidArgument "<method> cannot be an op of edit.batch", which also covers
		// edit.batch itself and every pending method); `params` holds none of the reserved members dryRun, ifRevision and
		// _meta (InvalidArgument with one issue per member, located at "/params/<member>" relative to the op, which
		// edit.batch prefixes with "/ops/<k>"); in a dry-run batch, the method has SupportsDryRun (Unsupported; edit.batch
		// checks every op this way before running the first). It then prepares `params` like PrepareParams, creates the
		// nested context with parent.CreateNested from a request that carries the parent's RequestOptions (so the batch's
		// dryRun and the attribution apply to every op, and IsDryRun agrees with the host's sandbox), invokes it and returns
		// the op's result JSON. Errors: those above; the op's own error otherwise.
		[[nodiscard]] Result<Json> InvokeNested(MethodContext& parent, std::string_view method, const Json& params) const;

		// The JSON Schema (2020-12) of `method`'s params: the params struct's object schema with "required" set from
		// RequiredParams, the reserved members dryRun (SupportsDryRun only) and ifRevision (every method but the session.hello
		// handshake) added, and, in Full
		// style, "$defs" for every referenced struct and component. Compact style replaces component maps and Variant members
		// with {"type": "object"} or {} plus a description pointing at component_schema, and drops "$defs" (§13.8).
		[[nodiscard]] Json GetParamsSchema(const MethodDescriptor& method, SchemaStyle style) const;
		// The JSON Schema of the result (the result struct; "_meta" and "dryRun" documented as optional members).
		[[nodiscard]] Json GetResultSchema(const MethodDescriptor& method, SchemaStyle style) const;

		// The rpc.discover entry of `method` (§13.5): {name, domain, description, exposeAsTool, mutates, supportsDryRun,
		// availableInRuntime, availableInLauncher, pending, timeoutSeconds, requiredParams, params (Full schema), result
		// (Full schema), examples}.
		[[nodiscard]] Json Describe(const MethodDescriptor& method) const;

		// The method catalogue that --dump-reference writes (ADR 0008 decision 9): {"Format": "MethodCatalog", "Version": 1,
		// "ProtocolVersion": "1.0", "Methods": [Describe(m) for every method except test hooks, in name order]}.
		[[nodiscard]] Json BuildMethodCatalog() const;

		// The MCP tool catalogue (Tools/MCP/catalog.json, §13.8): {"Format": "McpCatalog", "Version": 1, "ProtocolVersion":
		// "1.0", "Tools": [{"name": "domain_verb", "method": "domain.verb", "description", "inputSchema" (Compact params
		// schema), "mutates", "supportsDryRun", "timeoutSeconds"} for every ExposeAsTool method, in tool-name order]}.
		[[nodiscard]] Json BuildToolCatalog() const;
	private:
		// Registration internals of Add and AddPending (see their rules).
		void AddEntry(MethodSpecification specification, TypeKey host, TypeKey params, TypeKey result, bool pending, Invoker invoker);
	private:
		struct Storage; // descriptors and invokers, owned (MethodRegistry.cpp)
	private:
		const TypeRegistry* m_Types = nullptr; // documented back-reference: outlives the registry
		Scope<Storage> m_Storage;
		std::vector<const MethodDescriptor*> m_Methods; // name order
		bool m_IsFrozen = false;
	};

	// "entity.create" -> "entity_create", "project.getSettings" -> "project_get_settings" (§13.8 tool names).
	[[nodiscard]] std::string MethodNameToToolName(std::string_view method);

}
