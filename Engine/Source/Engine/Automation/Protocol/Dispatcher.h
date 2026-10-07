#pragma once

#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Automation/Protocol/MetaBuilder.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Automation/Protocol/ResultOffload.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Request execution (Architecture §13.2 "Threading", "Pending operations", "Disconnect"): requests from one client run in
// order, requests from all clients run on the main thread between input and update (AutomationServer::Pump), and pending
// operations are polled once per frame. Dispatcher.cpp is the protocol's allowlisted try/catch boundary (§4.6 item 5): a
// std::exception escaping a handler, a Poll or a Cancel (std::bad_alloc, an exception of the standard library or nlohmann)
// asserts in Debug builds (ENGINE_DEBUG: it is a bug to find; the message names the method and the exception's what()) and
// becomes an Internal error response naming both otherwise, so a Release editor keeps serving its other clients.

namespace Engine {

	class MethodRegistry;
	class RingBufferSink;
	class Watchdog;
	struct MethodDescriptor;

	// What the protocol needs from the application that hosts it (EditorCore's AutomationServer; the Runtime from M7).
	// Every call is on the main thread. The one-time admission of a request is separate from the per-invocation setup, so
	// a pending operation is admitted once and then polled (and cancelled) without being checked again:
	//     CheckAvailability, PrepareParams, CreateContext, AdmitRequest,
	//     then EnterInvocation / handler, Poll or Cancel / LeaveInvocation (once, or once per Poll and for the Cancel),
	//     then FinishRequest, then GetMetaState for the response.
	class IMethodHost
	{
	public:
		virtual ~IMethodHost() = default;

		// Whether `method` can be called in the host's current state. Checked right after the method is found and before its
		// params are read, so an unavailable method is reported as unavailable whatever its params: the editor answers
		// InvalidState "no project open; call project.create or project.open" for a method without AvailableInLauncher while
		// no project is open (§12.1); from M7 the play-state rules join it. An error is the call's response.
		[[nodiscard]] virtual Status CheckAvailability(const MethodDescriptor& method) const = 0;

		// The host's context for one request (its MethodContext subclass, which the registry checks with IsHostType).
		[[nodiscard]] virtual Scope<MethodContext> CreateContext(MethodRequest request) = 0;

		// Runs once per request, before the handler. It refuses what may not run: a Mutates method in a read-only editor or
		// with deny-mutations, unless it is a dry run (PermissionDenied); a stale ifRevision (Conflict with
		// data.currentRevision). For a dry run it then opens the host's sandbox, which stays open until FinishRequest (dry
		// runs are never pending). An error is the call's response: the handler does not run, and FinishRequest still runs.
		[[nodiscard]] virtual Status AdmitRequest(MethodContext& context) = 0;

		// Run immediately before and after every handler call, every Poll and the Cancel of an admitted request: they set and
		// clear the write attribution (provenance) and the command origin. Infallible, so a Poll or a Cancel is never refused.
		virtual void EnterInvocation(MethodContext& context) = 0;
		virtual void LeaveInvocation(MethodContext& context) = 0;

		// Runs once when the request is over (answered, refused by AdmitRequest, or cancelled), before GetMetaState is asked
		// for its response: closes the dry-run sandbox, so "_meta" reports the real state.
		virtual void FinishRequest(MethodContext& context) = 0;

		// The state "_meta" reports (MetaBuilder).
		[[nodiscard]] virtual MetaState GetMetaState() const = 0;

		// The server tag of this host's offload file names (MakeOffloadServerTag of its process id and start time), fixed
		// for the host's lifetime.
		[[nodiscard]] virtual std::string GetOffloadServerTag() const = 0;

		// Writes an offloaded result's text to `fileName` (MakeOffloadFileName with the host's MakeOffloadServerTag) in the
		// host's offload directory and returns the absolute native path, UTF-8 with '/' separators, that the response names.
		// Errors: those of the write; the Dispatcher then answers with the error instead of the oversized result.
		[[nodiscard]] virtual Result<std::string> WriteOffloadedResult(std::string_view fileName, std::string_view text) = 0;
	};

	// A response (or nothing for a notification) for one client.
	struct OutboundMessage
	{
		ClientId Client = NoClient;
		Json Message{};
	};

	struct DispatcherSpecification
	{
		size_t OffloadThresholdBytes = DefaultOffloadThresholdBytes;
	};

	// Per-client request queues, the pending operations and response building (results, errors, "_meta", offloading).
	// Main thread only; not copyable or movable.
	//
	// One request, in order: MethodRegistry::Find (MethodNotFound with suggestions), IMethodHost::CheckAvailability,
	// PrepareParams (InvalidParams or Unsupported), CreateContext and AdmitRequest; then EnterInvocation,
	// MethodRegistry::Invoke inside the catch boundary with the phase marker set to "Automation:<method>", and
	// LeaveInvocation; then, unless the result is pending, FinishRequest and the response: the result (plus "dryRun": true
	// for a dry run, and offloaded when over the threshold for clients that allow it) or the error (with the context's error
	// data), each with "_meta" from MetaBuilder. A pending result is kept with its context and polled on later pumps, each
	// Poll between EnterInvocation and LeaveInvocation and never admitted again (an ifRevision is checked once, when the
	// request starts); when it resolves, FinishRequest runs and the response is sent. The client's next request waits until
	// then (requests from one client run in order). Notifications run the same way but produce no message.
	class Dispatcher
	{
	public:
		// `registry` (frozen), `host`, `log` and `watchdog` (may be null) are documented back-references that outlive the
		// dispatcher.
		Dispatcher(const MethodRegistry& registry, IMethodHost& host, const RingBufferSink& log, Watchdog* watchdog,
			DispatcherSpecification specification = {});
		~Dispatcher();

		Dispatcher(const Dispatcher&) = delete;
		Dispatcher& operator=(const Dispatcher&) = delete;

		// A new client (TCP after its session.hello, or in-process); asserts an unused id. `name` goes into RequestInfo.
		// With `offloadLargeResults` false every result stays inline whatever its size: in-process clients that consume the
		// results themselves (BatchRunner's "$ref" substitution) pass false; TCP clients always offload (§13.4).
		void AddClient(ClientId client, std::string name, bool offloadLargeResults = true);

		// The client is gone (§13.2 "Disconnect"). Each of its pending operations gets PendingOperation::Cancel between
		// EnterInvocation and LeaveInvocation, then FinishRequest, and is destroyed without a response; Cancel always runs and
		// cannot be refused (lockstep and recordings depend on it, §13.2). Its queued requests, which were never admitted,
		// are dropped, and MetaBuilder forgets it. Logs one Info line naming the client and the number of cancelled
		// operations. Unknown clients are ignored. The destructor does the same for every remaining client.
		void RemoveClient(ClientId client);

		// Queues a request of `client` (asserted known).
		void Enqueue(ClientId client, RpcRequest request);

		// Does the frame's automation work and returns the messages to send, in completion order: first every pending
		// operation is polled once (in the order they started), then queued requests run, clients in round-robin order, until
		// none is runnable or `budget` (wall time, measured with the steady clock) is spent; at least one request runs when
		// one is runnable, so a slow method still progresses. Heartbeats the watchdog between requests.
		[[nodiscard]] std::vector<OutboundMessage> Pump(std::chrono::microseconds budget);

		// True when a request is queued or an operation pending, for any client.
		[[nodiscard]] bool HasWork() const;
		[[nodiscard]] size_t GetQueuedCount(ClientId client) const;
		[[nodiscard]] size_t GetPendingCount(ClientId client) const;
	private:
		// The back-references, the MetaBuilder, the clients with their queues and pending operations, and the offload
		// sequence (Dispatcher.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
