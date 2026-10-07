#pragma once

#include "Engine/Automation/Protocol/Dispatcher.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The editor's automation server (Architecture §13.1, §13.2): the protocol (ProtocolServer, Dispatcher, MethodRegistry)
// hosted on the editor state. It runs with --automation or --headless (and, from M10, with the "Allow AI automation"
// preference); it does not exist in Dist builds (EditorCore has no Dist configuration).

namespace Engine {

	class EditorContext;

	struct AutomationServerSpecification
	{
		// Listen on TCP 127.0.0.1 and write the session file. EditorApp sets it from
		// EditorLaunchOptions::ListensForAutomation: --automation, or --headless unless the run is one-shot (--batch,
		// --upgrade, --dump-reference), so no MCP bridge can attach to a batch or upgrade run. Without it only in-process
		// clients exist (--batch, --upgrade, the Tests).
		bool Listen = false;
		// --automation=<port>; 0: OS-assigned.
		uint16_t Port = 0;
		// --automation-test-hooks: register the debug.* methods.
		bool TestHooks = false;
		// Reported by session.info and in the session file.
		bool Headless = false;
		std::string RendererName = "none"; // "vulkan" or "none" (--renderer)
		// <UserData>/<AppName>/Automation/Sessions (§13.2); required when Listen.
		std::filesystem::path SessionsDirectory{};
		// The repository root for docs.get (.claude/skills, Docs/Reference); empty: docs.get is Unsupported. The editor
		// passes ENGINE_REPO_ROOT in development builds.
		std::filesystem::path DocsRoot{};
		// §4.2 step 3: "AutomationServer::Pump(budget 4 ms)".
		std::chrono::microseconds PumpBudget{ 4000 };
		std::chrono::milliseconds WatchdogStallThreshold = DefaultWatchdogStallThreshold;
		// How often Pump tries again to rewrite a session file whose rewrite failed (on Windows, replacing it fails while a
		// client such as engine_client.list_sessions has it open), so the file always comes to name the open project.
		std::chrono::milliseconds SessionFileRetryInterval{ 1000 };
	};

	// One connected client, for session.info and the AutomationPanel (M10).
	struct AutomationClientInfo
	{
		ClientId Id = NoClient;
		std::string Name{};
		std::string Version{};
		bool InProcess = false;
	};

	// The server. Main thread only (its ProtocolServer's I/O thread is internal); not copyable or movable.
	//
	// As the protocol's IMethodHost (Dispatcher.h) it:
	//   - in CheckAvailability refuses every method but those AvailableInLauncher while no project is open (InvalidState
	//     "no project open; call project.create or project.open", §12.1), before the params are read;
	//   - creates EditorMethodContexts;
	//   - in AdmitRequest refuses, in this order: Mutates methods in a read-only editor unless the call is a dry run
	//     (PermissionDenied "the editor is read-only (--read-only)"); a stale ifRevision, compared with
	//     EditorContext::GetRevision (Conflict, data.currentRevision); then, for a dry run, opens an EditorDryRunScope that
	//     the context holds;
	//   - in EnterInvocation sets the write attribution {method, request id, client name, transcript line} on the
	//     EditorContext, and in LeaveInvocation clears it;
	//   - in FinishRequest closes the request's dry run;
	//   - reports MetaState from EditorContext::GetRevision, the dirty flag and the history;
	//   - writes offloaded results to project://Library/Automation/Out/ (user://Automation/Out/ without a project or when
	//     read-only), named with its server tag (its process id and start time).
	// On a client's disconnect it removes the client from the Dispatcher (cancelling its pending operations) and appends one
	// AutomationClientDisconnected event (§13.2).
	class AutomationServer final : private IMethodHost
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class AutomationServer;
		};

		// Use Create.
		AutomationServer(ConstructionKey key, EditorContext& editor, const AutomationServerSpecification& specification);
		// Stops the transport, cancels every pending operation and removes the session file.
		~AutomationServer() override;

		AutomationServer(const AutomationServer&) = delete;
		AutomationServer& operator=(const AutomationServer&) = delete;

		// Builds the method registry (RegisterEditorMethods, then Freeze) over the editor's type registry and the Dispatcher;
		// with Listen, generates the token, starts the ProtocolServer and writes the session file. `editor` is a documented
		// back-reference that outlives the server. Errors: those of GenerateAuthToken, ProtocolServer::Start (AlreadyExists for
		// a port in use) and SessionFile::Write, with nothing left running.
		[[nodiscard]] static Result<Scope<AutomationServer>> Create(EditorContext& editor, const AutomationServerSpecification& specification);

		// The frame's automation work at the safe point (§4.2 step 3), on the main thread: heartbeat and phase "Pump" on the
		// watchdog; client events (Connected: Dispatcher::AddClient; Disconnected: RemoveClient and the event); queued TCP
		// requests into the Dispatcher; Dispatcher::Pump(PumpBudget); responses to their clients (TCP sends, in-process queues);
		// and a rewrite of the session file when the open project changed since the last write.
		void Pump();

		// The bound TCP port; 0 without Listen.
		[[nodiscard]] uint16_t GetPort() const;
		[[nodiscard]] const MethodRegistry& GetMethods() const { return m_Methods; }
		[[nodiscard]] const AutomationServerSpecification& GetSpecification() const { return m_Specification; }
		[[nodiscard]] EditorContext& GetEditor() const { return *m_Editor; }
		[[nodiscard]] Watchdog& GetWatchdog() { return m_Watchdog; }

		// The connected clients (TCP after session.hello, and in-process ones), by id.
		[[nodiscard]] std::vector<AutomationClientInfo> GetClients() const;

		// In-process clients (--batch, --upgrade, the Tests): a new client named `name` ("batch", "cli", "test") whose requests
		// run through the same Dispatcher as TCP requests, so in-process tests exercise the real dispatch path (§15.2
		// "an in-process round trip for every method"). No handshake. With `offloadLargeResults` false its results stay
		// inline whatever their size (BatchRunner, whose "$ref" substitution reads them; Dispatcher::AddClient).
		[[nodiscard]] ClientId ConnectInProcess(std::string name, bool offloadLargeResults = true);
		// Removes the client like a TCP disconnect (pending operations cancelled, AutomationClientDisconnected appended).
		void DisconnectInProcess(ClientId client);
		// Queues a request of an in-process client (asserted); it runs at the next Pump.
		void SubmitInProcess(ClientId client, RpcRequest request);
		// The responses for an in-process client since the last call, in completion order.
		[[nodiscard]] std::vector<Json> TakeInProcessResponses(ClientId client);
	private:
		[[nodiscard]] Status CheckAvailability(const MethodDescriptor& method) const override;
		[[nodiscard]] Scope<MethodContext> CreateContext(MethodRequest request) override;
		[[nodiscard]] Status AdmitRequest(MethodContext& context) override;
		void EnterInvocation(MethodContext& context) override;
		void LeaveInvocation(MethodContext& context) override;
		void FinishRequest(MethodContext& context) override;
		[[nodiscard]] MetaState GetMetaState() const override;
		[[nodiscard]] std::string GetOffloadServerTag() const override;
		[[nodiscard]] Result<std::string> WriteOffloadedResult(std::string_view fileName, std::string_view text) override;
	private:
		// The Dispatcher, the ProtocolServer, the token, the session file path, the server tag, the clients with their
		// names and versions, the in-process response queues and the project the session file last named
		// (AutomationServer.cpp).
		struct State;
	private:
		EditorContext* m_Editor = nullptr; // documented back-reference: outlives the server
		AutomationServerSpecification m_Specification;
		Watchdog m_Watchdog;
		MethodRegistry m_Methods; // over the editor's type registry; filled and frozen by Create
		Scope<State> m_State;
	};

}
