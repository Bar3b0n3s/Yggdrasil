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
		// options (an op never carries its own dryRun or ifRevision).
		[[nodiscard]] virtual Scope<MethodContext> CreateNested(MethodRequest request) const = 0;

		// The host's asset reference resolver, through which MethodRegistry::Invoke rewrites the asset references in the
		// params to handles (convention 13 of MethodRegistry.h) before it reads them; null (the default) for a host without
		// assets, whose AssetRef values must then be handles. The pointer stays valid while the request lives.
		[[nodiscard]] virtual IAssetReferenceResolver* GetAssetReferenceResolver() const { return nullptr; }
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
