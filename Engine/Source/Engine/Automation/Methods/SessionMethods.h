#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

// session.* (Architecture §13.2, §13.5), shared by the Editor and the Runtime (§13.5 "Runtime subset"; moved here from
// EditorCore with M7, unchanged in name and wire format, Docs/Decisions/0012-m7-decisions.md decision 12). The host
// describes itself through AutomationMethodContext::DescribeSession and exits through AutomationMethodContext::Shutdown.
// Param and result structs follow the conventions of MethodRegistry.h; a member's JSON key is its name with the first
// letter lower-cased unless its comment names another key.

namespace Engine {

	class AutomationMethodContext;
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

	// Registry struct "SessionProjectSummary": the open project, or open = false. The Runtime reports its game: open, the
	// manifest's Name, no project file, read-only.
	struct SessionProjectSummary
	{
		bool Open = false;
		std::string Name{};        // ProjectSettings::Name
		std::string ProjectFile{}; // absolute path of the .eproj, '/' separators; empty in the Runtime
		bool ReadOnly = false;
	};

	struct SessionHelloResult
	{
		std::string Protocol{};      // "protocolVersion": the server's, "1.0"
		std::string EngineVersion{}; // EngineVersionString
		uint32_t Client = 0;         // "clientId": this connection's ClientId
		// Optional protocol features this server offers: the editor's "dryRun", "ifRevision", "batch", "pendingOperations",
		// "offload", and "testHooks" when started with --automation-test-hooks; the Runtime's "ifRevision",
		// "pendingOperations" and "offload".
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
		// The names of "_meta".playState (PlayMethods.h PlayRunState): "Edit" without a play session, "Paused" for a paused
		// one, else "Play" or "Simulate" by its mode.
		std::string PlayState = "Edit";
		std::string Renderer{};      // "vulkan" or "none" (--renderer)
		std::string LockstepOwner{}; // the client name owning lockstep; empty: none (or an in-process driver)
		bool ReadOnly = false;
		bool Headless = false;
		std::vector<SessionClientSummary> Clients{};
	};

	// What a host reports about itself in session.hello and session.info (AutomationMethodContext::DescribeSession); the
	// handlers add the versions, the process id, the play state and the lockstep owner.
	struct SessionHostDescription
	{
		std::vector<std::string> Capabilities{}; // as SessionHelloResult::Capabilities
		SessionProjectSummary Project{};
		std::string Renderer{}; // "vulkan" or "none"
		bool ReadOnly = false;  // the host denies mutations: the editor with --read-only, the Runtime always
		bool Headless = false;
		std::vector<SessionClientSummary> Clients{}; // in any order: session.info sorts them by id
	};

	// session.shutdown {save?, force?}: the editor saves the open scene first when `save`; with a dirty scene and neither
	// flag the call fails with InvalidState (nothing is lost silently); `force` exits discarding unsaved changes. The
	// Runtime has no scene file to save, so it saves nothing whatever the flags. The response is sent, then the host exits
	// with code 0 at its next frame (AutomationMethodContext::Shutdown).
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

		// session.hello: reports the session; the handshake checks already passed on the I/O thread. Errors: InvalidArgument
		// at /client/name or /protocolVersion for the param checks in-process callers get here; Unsupported for another
		// major protocol version.
		[[nodiscard]] Result<SessionHelloResult> SessionHello(AutomationMethodContext& context, const SessionHelloParams& params);
		// session.info.
		[[nodiscard]] Result<SessionInfoResult> SessionInfo(AutomationMethodContext& context, const NoParams& params);
		// session.shutdown. Errors: those of AutomationMethodContext::Shutdown (the editor: InvalidState for a dirty scene
		// without save or force; PermissionDenied for save in a read-only editor; the save's errors).
		[[nodiscard]] Result<SessionShutdownResult> SessionShutdown(AutomationMethodContext& context, const SessionShutdownParams& params);

	}

	// Registers the structs above (SessionClientInfo, SessionHelloParams, SessionProjectSummary, SessionHelloResult,
	// SessionClientSummary, SessionInfoResult, SessionShutdownParams, SessionShutdownResult).
	void RegisterSessionMethodTypes(TypeRegistry& registry);

	// Registers session.hello, session.info and session.shutdown: available in the launcher state and in the Runtime; not
	// tools (the bridge's editor_status and editor_shutdown wrap them); none is flagged Mutates (session.shutdown writes
	// only with save, through the editor); only session.info is AllowedInBatch.
	void RegisterSessionMethods(MethodRegistry& methods);

}
