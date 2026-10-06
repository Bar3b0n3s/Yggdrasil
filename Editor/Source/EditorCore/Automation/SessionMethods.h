#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

// session.* (Architecture §13.5). Param and result structs follow the conventions of MethodRegistry.h; a member's JSON key is
// its name with the first letter lower-cased unless its comment names another key.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry struct "SessionClientInfo": session.hello's "client".
	struct SessionClientInfo
	{
		std::string Name{};    // 1 to 64 printable ASCII characters ("engine-mcp", "engine-tests")
		std::string Version{}; // free text; may be empty
	};

	// session.hello {token, protocolVersion, client} (§13.2). The I/O thread has already checked the token and the version
	// (Handshake.h) when the handler runs; the token is never logged or echoed.
	struct SessionHelloParams
	{
		std::string Token{};
		std::string Protocol{}; // "protocolVersion": "<major>.<minor>"
		SessionClientInfo Client{};
	};

	// Registry struct "SessionProjectSummary": the open project, or open = false.
	struct SessionProjectSummary
	{
		bool Open = false;
		std::string Name{};        // ProjectSettings::Name
		std::string ProjectFile{}; // absolute path of the .eproj, '/' separators
		bool ReadOnly = false;
	};

	struct SessionHelloResult
	{
		std::string Protocol{};      // "protocolVersion": the server's, "1.0"
		std::string EngineVersion{}; // EngineVersionString
		uint32_t Client = 0;         // "clientId": this connection's ClientId
		// Optional protocol features this server offers: "dryRun", "ifRevision", "batch", "pendingOperations", "offload",
		// and "testHooks" when started with --automation-test-hooks.
		std::vector<std::string> Capabilities{};
		SessionProjectSummary Project{};
	};

	// Registry struct "SessionClientSummary": one connected client.
	struct SessionClientSummary
	{
		uint32_t Id = 0;
		std::string Name{};
		std::string Version{};
		bool InProcess = false; // a batch, CLI or test caller rather than a TCP connection
	};

	// session.info {} -> versions, capabilities, project, play state, renderer, lockstep owner, read-only flag (§13.5).
	struct SessionInfoResult
	{
		std::string Protocol{}; // "protocolVersion"
		std::string EngineVersion{};
		uint32_t ProcessId = 0; // "pid"
		std::vector<std::string> Capabilities{};
		SessionProjectSummary Project{};
		std::string PlayState = "Edit"; // "Edit" until play sessions exist (M7)
		std::string Renderer{};         // "vulkan" or "none" (--renderer)
		std::string LockstepOwner{};    // the client name owning lockstep (M7); empty: none
		bool ReadOnly = false;
		bool Headless = false;
		std::vector<SessionClientSummary> Clients{};
	};

	// session.shutdown {save?, force?}: saves the open scene first when `save`; with a dirty scene and neither flag the call
	// fails with InvalidState (nothing is lost silently); `force` exits discarding unsaved changes. The response is sent,
	// then the editor exits with code 0 at its next frame (EditorContext::RequestShutdown).
	struct SessionShutdownParams
	{
		bool Save = false;
		bool Force = false;
	};

	struct SessionShutdownResult
	{
		bool Saved = false;                    // the open scene was written
		std::vector<std::string> SavedFiles{}; // project-relative
	};

	namespace Automation {

		// session.hello: reports the session; the handshake checks already passed on the I/O thread.
		[[nodiscard]] Result<SessionHelloResult> SessionHello(EditorMethodContext& context, const SessionHelloParams& params);
		// session.info.
		[[nodiscard]] Result<SessionInfoResult> SessionInfo(EditorMethodContext& context, const NoParams& params);
		// session.shutdown. Errors: InvalidState for a dirty scene without save or force; PermissionDenied for save in a
		// read-only editor; the save's errors.
		[[nodiscard]] Result<SessionShutdownResult> SessionShutdown(EditorMethodContext& context, const SessionShutdownParams& params);

	}

	// Registers the structs above (SessionClientInfo, SessionHelloParams, SessionProjectSummary, SessionHelloResult,
	// SessionClientSummary, SessionInfoResult, SessionShutdownParams, SessionShutdownResult).
	void RegisterSessionMethodTypes(TypeRegistry& registry);

	// Registers session.hello, session.info and session.shutdown: available in the launcher state and in the Runtime (M7;
	// their declarations move to Engine/Automation/Methods then, ADR 0008 decision 26); not tools (the bridge's
	// editor_status and editor_shutdown wrap them); none is flagged Mutates (session.shutdown writes only with save, through
	// EditorContext); only session.info is AllowedInBatch.
	void RegisterSessionMethods(MethodRegistry& methods);

}
