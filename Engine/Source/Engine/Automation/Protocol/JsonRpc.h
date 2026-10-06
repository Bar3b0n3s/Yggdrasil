#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/Json.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

// JSON-RPC 2.0 messages of the automation protocol (Architecture §13.1-§13.3). The engine owns a stable, semver'd
// protocol; MCP lives only in the Python bridge (Tools/MCP), and the same messages serve the Python tests, batch files and
// (from M7) the Runtime.
//
// Wire conventions (Docs/Decisions/0008-m4-decisions.md decision 5):
//   - Requests are objects {"jsonrpc": "2.0", "id": <integer or string>, "method": "domain.verb", "params": {...}}; a
//     request without "id" is a notification, which runs but gets no response. Batches (a top-level array) and
//     positional params (an array) are not accepted: every method takes named, camelCase params.
//   - params._meta carries request metadata the engine reads and removes before parsing: "transcriptLine" (the 1-based
//     line of the request in <Project>/Automation/BuildLog.jsonl, written by the MCP bridge, §13.8), which the editor
//     stores in provenance (§13.4).
//   - Success: {"jsonrpc": "2.0", "id": ..., "result": {<result struct>..., "_meta": {...}}}. A dry run's result also
//     carries "dryRun": true.
//   - Failure: {"jsonrpc": "2.0", "id": ..., "error": {"code": <RpcErrorCode>, "message": ..., "data": {"errorCode":
//     <ErrorCode name>, "detail": <Error message>, "hint"?, "contexts"?, "location"?, "issues": [...], <method data>...,
//     "_meta"?}}} (§13.3). Busy answers of the I/O thread carry no "_meta": the main thread is not running.

namespace Engine {

	// The version of the automation protocol (§13.2 "protocolVersion": "1.0"). Minor versions add methods and optional
	// members; a different major version is incompatible and session.hello rejects it, naming both versions. session.hello's
	// own params never gain members within a major version (they are checked strictly before the client knows the server's
	// version); a client sends a newer minor version's optional members elsewhere only after reading the server's
	// protocolVersion from the hello result.
	struct ProtocolVersion
	{
		uint32_t Major = 1;
		uint32_t Minor = 0;

		// "<Major>.<Minor>", for example "1.0".
		[[nodiscard]] std::string ToString() const;

		// Parses "<major>.<minor>": two decimal numbers without sign, leading zeros (except "0" itself) or whitespace, each
		// at most 9 digits; nullopt for anything else.
		[[nodiscard]] static std::optional<ProtocolVersion> Parse(std::string_view text);

		[[nodiscard]] bool operator==(const ProtocolVersion& other) const = default;
	};

	// The protocol this build speaks.
	inline constexpr ProtocolVersion CurrentProtocolVersion{ 1, 0 };

	// The engine version reported by session.hello, session.info and session files (§13.2 "engineVersion"). A constant
	// until the export milestone defines how builds are versioned (ADR 0008 decision 5).
	inline constexpr std::string_view EngineVersionString = "0.1.0";

	// A connection or in-process caller (batch runner, CLI, tests) of one automation server; unique for the server's
	// lifetime and never reused. NoClient is none.
	using ClientId = uint32_t;
	inline constexpr ClientId NoClient = 0;

	// At most 4 TCP clients at a time (§13.2); in-process callers do not count.
	inline constexpr size_t MaxAutomationClients = 4;

	// JSON-RPC error codes: the standard ones and the engine's (§13.3). Values are part of the protocol and never change.
	enum class RpcErrorCode : int32_t
	{
		ParseError = -32700,     // the payload is not JSON
		InvalidRequest = -32600, // JSON, but not a request object
		MethodNotFound = -32601, // no such method (data.issues suggests close names)
		InvalidParams = -32602,  // params fail their schema: missing, unknown, wrong type, out of range
		Internal = -32000,       // an unexpected failure inside the engine (an exception caught by the Dispatcher, an Io error)
		NotFound = -32001,
		InvalidState = -32002,
		ValidationFailed = -32003, // the request is well-formed but the data it names is invalid (a malformed scene file)
		Conflict = -32004,         // ifRevision mismatch, or a name that is taken
		Timeout = -32005,
		ScriptError = -32006,
		Busy = -32007,         // answered by the I/O thread while the main thread has not pumped for the stall threshold
		Unauthorized = -32008, // bad or missing token, or a mutation an editor denies (read-only, deny-mutations switch)
		Unsupported = -32009,  // includes dryRun on a method without supportsDryRun, and test hooks that are not enabled
		Cancelled = -32010
	};

	// The enumerator name ("InvalidParams", "Busy"); "Internal" for a value outside the enumeration.
	[[nodiscard]] std::string_view RpcErrorCodeToString(RpcErrorCode code);

	// The JSON-RPC code a handler error is reported with (ADR 0008 decision 5):
	//   InvalidArgument -> InvalidParams; NotFound -> NotFound; InvalidState -> InvalidState; Validation, Parse,
	//   UnsupportedVersion, ImportFailed -> ValidationFailed; AlreadyExists, Conflict -> Conflict; Timeout -> Timeout;
	//   Script, CompileFailed -> ScriptError; PermissionDenied -> Unauthorized; Unsupported -> Unsupported;
	//   Cancelled -> Cancelled; Unknown, Io, Gpu -> Internal.
	// Parameter parsing failures are always InvalidParams, whatever their ErrorCode (the registry reports them so).
	// PermissionDenied is reserved for the protocol's own refusals (a bad token, a read-only editor, deny-mutations). An
	// operating-system access failure (EACCES, EPERM, a Windows sharing violation) that FileSystem reports as
	// PermissionDenied is converted to Io by the host's file paths before it reaches a response (EditorContext's file
	// writes and reads), so agents never mistake a locked file for an authorization failure.
	[[nodiscard]] RpcErrorCode ToRpcErrorCode(ErrorCode code);

	// One parsed request.
	struct RpcRequest
	{
		Json Id; // integer or string; null for a notification
		bool IsNotification = false;
		std::string Method;
		Json Params; // always an object ({} when absent); params._meta removed
		// params._meta.transcriptLine: the request's line in the project's BuildLog.jsonl (§13.8), >= 1.
		std::optional<uint64_t> TranscriptLine;
	};

	// Why a payload is not a request, and the id to answer with (null when it could not be recovered).
	struct RpcParseFailure
	{
		RpcErrorCode Code = RpcErrorCode::InvalidRequest; // ParseError or InvalidRequest
		Error Failure;
		Json Id;
	};

	// Parses one frame payload (UTF-8 JSON, nesting already bounded by the FrameDecoder). Failures: ParseError for text
	// that is not JSON; InvalidRequest for a top-level array (batch), a non-object, "jsonrpc" other than "2.0", a missing or
	// non-string "method", "params" that is present but not an object, an "id" that is neither an integer, a string nor
	// absent (null ids are rejected: a notification omits "id"), or a params._meta that is not an object or whose
	// transcriptLine is not an integer >= 1. Pure; thread-safe.
	[[nodiscard]] std::expected<RpcRequest, RpcParseFailure> ParseRpcRequest(std::string_view payload);

	// {"jsonrpc": "2.0", "id": id, "result": result} with `meta` stored as result["_meta"] unless it is null. `result`
	// must be an object (asserted).
	[[nodiscard]] Json MakeResultResponse(const Json& id, Json result, const Json& meta);

	// The error response of `error` with `code`: "message" is the standard JSON-RPC text for the standard codes ("Parse
	// error", "Invalid Request", "Method not found", "Invalid params") and Error::GetMessageText() otherwise; "data" holds
	// errorCode (ErrorCodeToString), detail (Error::GetMessageText()), hint (when set), contexts (when any), location
	// ({file, line, column, pointer, entity}, the members that are set), issues (each {pointer, message, hint?,
	// suggestions?}; always present, possibly empty), then every member of `extraData` (an object or null, such as
	// edit.batch's failedOp), then "_meta" unless `meta` is null.
	[[nodiscard]] Json MakeErrorResponse(const Json& id, RpcErrorCode code, const Error& error, const Json& extraData, const Json& meta);

	// MakeErrorResponse with ToRpcErrorCode(error.GetCode()).
	[[nodiscard]] Json MakeErrorResponse(const Json& id, const Error& error, const Json& extraData, const Json& meta);

	// The watchdog's answer (§13.2): code Busy, data {"errorCode": "Busy", "detail": ..., "phase": <the main thread's
	// phase marker>, "stalledMilliseconds": <how long it has not pumped>, "issues": []}. No "_meta".
	[[nodiscard]] Json MakeBusyResponse(const Json& id, std::string_view phase, uint64_t stalledMilliseconds);

}
