#pragma once

#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/TypeInfo.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The per-request context method handlers receive (Architecture §13.2, §13.4). The protocol layer cannot know the
// editor (layer rules, §3), so a host derives its own context from MethodContext (EditorCore's EditorMethodContext, from
// M7 the Runtime's), and typed handlers take the derived type or one of its bases. The registry checks the host type
// before it casts, without RTTI (ADR 0008 decision 7):
//   - every context answers IsHostType for its own most-derived key and for MethodContext;
//   - an intermediate base shared by several hosts (from M7, Engine/Automation/Methods' context for the handlers the
//     Editor and the Runtime share, ADR 0008 decision 26) overrides IsHostType to add its own key and calls its base,
//     so a handler typed on it can be registered by every host whose context derives from it.

namespace Engine {

	class IFieldSchemaSource;

	class MethodRegistry;
	class Watchdog;
	struct MethodDescriptor;

	// Who sent a request.
	struct RequestInfo
	{
		ClientId Client = NoClient;
		// session.hello's client.name for TCP clients; "batch", "cli" or "test" for in-process callers. Provenance records it
		// (§13.4 "client").
		std::string ClientName{};
		// The JSON-RPC id (an integer or a string); null for a notification and for the ops of edit.batch.
		Json Id{};
		std::string Method{};
		// params._meta.transcriptLine (§13.8): the request's line in the project's BuildLog.jsonl, stored in provenance.
		std::optional<uint64_t> TranscriptLine{};
	};

	// A request's scene target "scene" (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decision 16): a scene path as a
	// string (project-relative or project://, the path scene.list reports) or an open scene's id as an integer from 1 to
	// 2^32 - 1 (the uint32 id scene.list reports, EditorCore's OpenSceneId; the only way to name a scene that was never
	// saved). Exactly one of the two is set. The protocol layer only checks the JSON type and the id's range (InvalidParams
	// located at /scene otherwise); the host resolves it against its open scenes (EditorMethodContext::ResolveSceneTarget;
	// the Runtime, which has no open scenes, refuses it).
	struct SceneTargetReference
	{
		std::string Path{}; // non-empty when the target is a path
		uint32_t Id = 0;    // non-zero when the target is an id
	};

	// The reserved params any request may carry. MethodRegistry::PrepareParams removes them from the params before the
	// param struct is read, so no param struct declares them (the param-struct conventions in MethodRegistry.h).
	struct RequestOptions
	{
		// "dryRun": true (§13.4). Accepted only by methods with SupportsDryRun; any other method answers Unsupported.
		bool DryRun = false;
		// "ifRevision": N (§13.4, optimistic concurrency). Accepted by every method (a precondition for reads, a guard for
		// calls that write only when asked); the host fails the call with Conflict and data.currentRevision when its
		// revision differs (the editor's
		// EditorContext::GetRevision, which never repeats a value across scenes).
		std::optional<uint64_t> IfRevision{};
		// "scene" (M20, decision 16): accepted only by methods with MethodSpecification::SupportsSceneTarget; nullopt when
		// the request names no scene (it then addresses the active scene). The ops of edit.batch inherit their batch's, and
		// an op that carries its own is InvalidArgument, as one carrying dryRun or ifRevision is.
		std::optional<SceneTargetReference> Scene{};
	};

	// Resolves the readable spellings of an asset reference in params (§7.1, convention 13 of MethodRegistry.h): a project
	// path "Assets/..." (with "#<key>" for a sub-asset) or an engine path "engine://...". The protocol layer cannot know the
	// asset manager (layer rules, §3), so a host that has one implements this (EditorCore's AutomationServer) and returns it
	// from its context's GetAssetReferenceResolver. Main thread only.
	class IAssetReferenceResolver
	{
	public:
		virtual ~IAssetReferenceResolver() = default;

		// The handle `reference` names. `assetTypeName` is the AssetType name the field accepts (TypeInfo::
		// GetAssetTypeName), empty for any. May refresh the host's asset registry first (§7.3: a call that takes a path
		// refreshes). Errors: InvalidArgument for a malformed reference or an asset of another type; NotFound for a
		// reference that names no asset (with "did you mean" suggestions in the hint); any other code is the host's own
		// (InvalidState for a project path while no project is open, a refresh's errors), which MethodRegistry::Invoke
		// returns unchanged.
		[[nodiscard]] virtual Result<UUID> ResolveAssetReference(std::string_view reference, std::string_view assetTypeName) = 0;
	};

	// Resolves an EntityRef value inside component data that is not a 16-hex id (a component field, a script's
	// Field.Entity() value reached through Script.Fields, an element of an array or map of them) to the id of the entity it
	// names, so component data takes the spellings an entity param takes (§13.4: a unique id prefix of 6 to 15 hex digits
	// or an entity path; Docs/Decisions/0030-m19-contract.md decision 7, A-EREF). The protocol layer cannot know scenes
	// (layer rules, §3), so a host implements this over the scene it resolves in (Utils::SceneEntityReferenceResolver,
	// Automation/Methods/SharedMethodSupport.h) and returns it from its context's GetEntityReferenceResolver. Which scene
	// is the phase 1 ledger's "Entity paths and scenes" text (Docs/Decisions/0028-backlog-program.md), shared with M20: a
	// path or $ref resolves in the request's (for an op, its batch's) target scene, the active scene when it names none.
	// The editor resolves in the open scene the request's scene target addresses (EditorSceneTargetScope;
	// Docs/Decisions/0031-m20-contract.md decision 16 and "Merge with M19"), or in the play scene for a request whose
	// target is Play (§13.4 "Target"); a host without one returns null from GetEntityReferenceResolver. An entity path is
	// the '/'-separated names from a scene root (Scene::FindEntityByPath) and continues through a prefab instance's members,
	// nested instances included, as the scene holds them. Main thread only.
	class IEntityReferenceResolver
	{
	public:
		virtual ~IEntityReferenceResolver() = default;

		// The id of the entity `reference` names. `pointer` locates the value in errors (its JSON pointer in the params).
		// Errors, located at `pointer`: InvalidArgument for anything that is no id prefix or path, and for an ambiguous prefix
		// or path (every candidate an ErrorIssue with its path); NotFound for a valid reference that names no entity (with
		// "did you mean" suggestions for a path); any other code is the host's own (InvalidState without a scene), which
		// MethodRegistry::Invoke returns unchanged.
		[[nodiscard]] virtual Result<UUID> ResolveEntityReference(std::string_view reference, std::string_view pointer) = 0;
	};

	// Everything one invocation needs, assembled by the Dispatcher (or MethodRegistry::InvokeNested) and handed to the
	// host's CreateContext.
	struct MethodRequest
	{
		RequestInfo Info{};
		RequestOptions Options{};
		const MethodDescriptor* Method = nullptr; // required; a back-reference into the registry, which outlives the request
		// Prepared params: an object, reserved members removed, enums canonical; MethodRegistry::Invoke then resolves the
		// asset references in the context's copy (convention 13).
		Json Params{};
		const MethodRegistry* Registry = nullptr; // required; outlives the request
		Watchdog* PhaseMarker = nullptr;          // the server's watchdog; null for in-process calls without a server
		uint32_t NestingDepth = 0;                // 0 for a request, 1 for an op of edit.batch (batches do not nest)
	};

	// The base of every host's request context. One instance per request (and per nested batch op), created by the host,
	// alive until the request resolves: a pending operation is polled with the context of its request. Main thread only.
	// Not copyable or movable.
	class MethodContext
	{
	public:
		virtual ~MethodContext();

		MethodContext(const MethodContext&) = delete;
		MethodContext& operator=(const MethodContext&) = delete;

		// TypeKeyOf<the most-derived host context>(), as given to the constructor.
		[[nodiscard]] TypeKey GetHostKey() const { return m_HostKey; }

		// True when this context is of the context type `key`: the most-derived host key, TypeKeyOf<MethodContext>(), and,
		// through overrides, the key of every intermediate base (see the file comment). MethodRegistry::Invoke casts to a
		// handler's host type only when this holds. An override returns true for its own class's key and otherwise calls its
		// base class's IsHostType.
		[[nodiscard]] virtual bool IsHostType(TypeKey key) const;

		[[nodiscard]] const RequestInfo& GetRequest() const { return m_Request.Info; }
		[[nodiscard]] const RequestOptions& GetOptions() const { return m_Request.Options; }
		[[nodiscard]] bool IsDryRun() const { return m_Request.Options.DryRun; }
		[[nodiscard]] const MethodDescriptor& GetMethod() const { return *m_Request.Method; }
		[[nodiscard]] const MethodRegistry& GetRegistry() const { return *m_Request.Registry; }
		[[nodiscard]] uint32_t GetNestingDepth() const { return m_Request.NestingDepth; }

		// The prepared params (an object), with the asset references resolved to handles by MethodRegistry::Invoke
		// (convention 13) once it has run. Handlers normally read their typed param struct instead.
		[[nodiscard]] const Json& GetParams() const { return m_Request.Params; }

		// True when the prepared params hold the member `name` (camelCase). This is how a handler tells an absent optional
		// param from one given with its default value ("name?" of entity.update: absent leaves the name unchanged).
		[[nodiscard]] bool HasParam(std::string_view name) const;

		// Sets the phase marker for the rest of this invocation, for example "Automation:project.upgrade
		// Assets/Scenes/Main.scene". No effect without a watchdog.
		void SetPhase(std::string_view phase) const;

		// Adds the member `name` (replacing an earlier one) to the error response's data if this request fails: edit.batch's
		// "failedOp", Conflict's "currentRevision". Members the protocol writes itself (errorCode, detail, hint, contexts,
		// location, issues, _meta) are reserved (asserted).
		void SetErrorData(std::string_view name, Json value);

		// The members set with SetErrorData: an object, or null when none.
		[[nodiscard]] const Json& GetErrorData() const { return m_ErrorData; }

		// The JSON of `result` through the registry's StructInfo for R, which must be registered (asserted): what an immediate
		// handler returns is serialized the same way, and pending operations call this for their outcome. Errors: Validation
		// from StructInfo::ToJson (a non-finite value).
		template<typename R>
		[[nodiscard]] Result<Json> SerializeResult(const R& result) const
		{
			return SerializeResultObject(TypeKeyOf<R>(), &result);
		}

		// A context of the same host for one op of edit.batch (MethodRegistry::InvokeNested): the host copies what it needs
		// from this context (attribution, dry-run state) into the new one. `request.Options` already holds this context's
		// options (an op never carries its own dryRun, ifRevision or scene).
		[[nodiscard]] virtual Scope<MethodContext> CreateNested(MethodRequest request) const = 0;

		// The host's asset reference resolver, through which MethodRegistry::Invoke rewrites the asset references in the
		// params to handles (convention 13 of MethodRegistry.h) before it reads them; null (the default) for a host without
		// assets, whose AssetRef values must then be handles. The pointer stays valid while the request lives.
		[[nodiscard]] virtual IAssetReferenceResolver* GetAssetReferenceResolver() const { return nullptr; }

		// M19 (Docs/Decisions/0030-m19-contract.md decision 7): the host's entity reference resolver, through which
		// MethodRegistry::Invoke rewrites every EntityRef value in component data that is not a 16-hex id to the id it names
		// (convention 7 of MethodRegistry.h), over the scene the request resolves in; null (the default) for a host without
		// one, whose component data then takes 16-hex ids only (Docs/Decisions/0027-m17-contract.md decision 3, A4's refusal
		// with its hint). The pointer stays valid while the request lives.
		[[nodiscard]] virtual IEntityReferenceResolver* GetEntityReferenceResolver() const { return nullptr; }

		// Immutable external field schemas for this request; null by default for hosts without them. Invoke requests these
		// only when a supplied Variant value cannot resolve from registry metadata alone, after ordinary asset paths have
		// become handles. The returned reference pins the source through canonicalization, validation and the handler.
		// Hosts retain the same snapshot for subsequent calls in this request; nested requests acquire their own snapshot.
		// Errors from acquiring a snapshot propagate unchanged and prevent the handler from running. Main thread only.
		[[nodiscard]] virtual Result<Ref<const IFieldSchemaSource>> GetFieldSchemaSnapshot();

		// Completes missing schema-discriminating owner fields in a partial params object using the addressed host object
		// (for example the existing Script handle for an entity.update Fields-only patch). Called only when a supplied
		// Variant cannot yet resolve, after ordinary asset path resolution and before the request snapshot is acquired.
		// The default does nothing. An override may add only absent owner discriminators, never overwrite supplied values,
		// copy unrelated fields, mutate host state or relax validation. Added fields keep their existing host value when
		// the handler applies the patch. Errors propagate unchanged and prevent invocation. Main thread only.
		[[nodiscard]] virtual Status CompleteParameterOwners(Json& params);
	protected:
		// `request.Method` and `request.Registry` must be set (asserted).
		MethodContext(TypeKey hostKey, MethodRequest request);
	private:
		[[nodiscard]] Result<Json> SerializeResultObject(TypeKey resultType, const void* result) const;
	private:
		TypeKey m_HostKey = nullptr;
		MethodRequest m_Request;
		Json m_ErrorData; // null until SetErrorData
	private:
		friend class MethodRegistry; // Invoke replaces the params with their resolved asset references (convention 13)
	};

}
