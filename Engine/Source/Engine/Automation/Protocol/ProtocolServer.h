#pragma once

#include "Engine/Automation/Protocol/Framing.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The automation transport (Architecture §13.2): TCP on 127.0.0.1 only (Platform/Socket.h), port 0 (OS-assigned) or
// --automation=<port>, up to MaxAutomationClients clients, LSP framing (Framing.h) and the session.hello handshake
// (Handshake.h), served by one I/O thread that only frames, parses, authenticates and queues (§4.11). Requests are executed
// on the main thread by whoever drains the queue (EditorCore's AutomationServer through its Dispatcher).

namespace Engine {

	class Watchdog;

	struct ProtocolServerSpecification
	{
		uint16_t Port = 0;   // 0: OS-assigned (GetPort tells which)
		std::string Token{}; // GenerateAuthToken(); required (AuthTokenLength hex digits, asserted)
		ProtocolVersion Version = CurrentProtocolVersion;
		// Authenticated connections at a time (§13.2: 4). Connections still in the handshake do not count.
		size_t MaxClients = MaxAutomationClients;
		// Connections that have not completed session.hello yet, at a time; a connection beyond it is closed at once.
		size_t MaxPendingHandshakes = 2 * MaxAutomationClients;
		// How long a connection may take from accept to a successful session.hello before the I/O thread closes it, so idle
		// or half-finished connections never hold a slot (§13.2 treats the loopback surface as hostile).
		std::chrono::milliseconds HandshakeTimeout{ 10000 };
		// The most bytes a connection's outbound queue may hold (responses Send queued that the client has not read yet);
		// a Send that would exceed it closes the connection instead.
		size_t MaxQueuedSendBytes = MaxFramePayloadBytes;
		// How long the I/O thread waits in Socket::WaitAny before it checks its stop flag and the handshake deadlines;
		// bounds how long Stop takes.
		std::chrono::milliseconds PollInterval{ 20 };
		// The I/O thread's clock for handshake deadlines and Watchdog::GetStall; empty: std::chrono::steady_clock::now.
		// Called on the I/O thread only. Tests inject a controllable one, so timeouts are tested without waiting.
		std::function<std::chrono::steady_clock::time_point()> TimeSource{};
	};

	// One authenticated request, ready for the main thread.
	struct InboundRequest
	{
		ClientId Client = NoClient;
		RpcRequest Request{};
	};

	enum class ClientEventKind : uint8_t
	{
		Connected,   // a client completed session.hello (its hello request follows in TakeRequests)
		Disconnected // a client's connection closed, cleanly or not, after it had connected
	};

	struct ClientEvent
	{
		ClientEventKind Kind = ClientEventKind::Connected;
		ClientId Client = NoClient;
		std::string Name{};    // session.hello client.name
		std::string Version{}; // session.hello client.version
	};

	// The listener, the I/O thread and the connections. Not copyable or movable.
	//
	// The I/O thread, per connection:
	//   - accepts it, unless MaxPendingHandshakes connections are already in the handshake, in which case it is closed at
	//     once; a connection that has not completed session.hello within HandshakeTimeout is closed without a response;
	//   - frames the byte stream; a FrameDecoder failure (an HTTP probe's first line, an oversized frame, invalid UTF-8,
	//     nesting over 128) closes the socket at once, without a response (§13.2);
	//   - parses each payload (ParseRpcRequest); a ParseError or InvalidRequest is answered and the connection stays open;
	//   - until the connection's session.hello succeeds: any other method is answered Unauthorized "session.hello must be the
	//     first request", a bad token Unauthorized, malformed hello params (ParseHelloRequest, unknown members included)
	//     InvalidParams; each counts as an authentication failure and the MaxAuthFailures-th closes the connection after its
	//     answer; an incompatible major version is answered Unsupported (listing both versions) and closes it. A good hello:
	//       - when MaxClients connections are already authenticated, is answered InvalidState "too many automation clients"
	//         and the connection is closed;
	//       - while the watchdog reports a stall (Watchdog::GetStall), is answered at once with MakeBusyResponse and leaves
	//         the connection unauthenticated, without counting as a failure and with its handshake deadline restarted, so
	//         the client retries session.hello and never hangs on a frozen editor (§13.2);
	//       - otherwise marks the connection authenticated and queues a Connected event and then the hello itself, which
	//         the main thread answers (session.hello handler);
	//   - after authentication: a second session.hello is answered InvalidState; while the watchdog reports a stall, every
	//     new request is answered at once with MakeBusyResponse (§13.2) and dropped; otherwise it is queued.
	// When an authenticated connection closes, a Disconnected event is queued; unauthenticated ones vanish silently.
	//
	// Sends never block the caller. Each connection has an outbound queue of framed messages: Send (main thread) and the I/O
	// thread's own answers (Busy, handshake errors) append to it under the connection's short lock, and the I/O thread
	// drains it with non-blocking writes (Socket::SendAvailable, waiting for writability in Socket::WaitAny), so frames
	// never interleave (ADR 0005 decision 16), the main thread never waits on a socket, and a client that stops reading
	// delays only itself. A connection whose queue would exceed MaxQueuedSendBytes is closed (its Disconnected queued).
	class ProtocolServer
	{
	public:
		// Restricts construction to Start; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ProtocolServer;
		};

		// Use Start.
		ProtocolServer(ConstructionKey key, const ProtocolServerSpecification& specification, Watchdog& watchdog);

		// Binds 127.0.0.1:specification.Port and starts the I/O thread. `watchdog` is a documented back-reference that
		// outlives the server. Errors: AlreadyExists when the port is in use; Io; InvalidArgument for a malformed token.
		[[nodiscard]] static Result<Scope<ProtocolServer>> Start(const ProtocolServerSpecification& specification, Watchdog& watchdog);

		// Stop().
		~ProtocolServer();

		ProtocolServer(const ProtocolServer&) = delete;
		ProtocolServer& operator=(const ProtocolServer&) = delete;

		// Stops the I/O thread (within PollInterval) and closes the listener and every connection, discarding unsent
		// output; every authenticated connection is reported once more as Disconnected by the next TakeClientEvents.
		// Idempotent.
		void Stop();

		// The bound port; 0 after Stop.
		[[nodiscard]] uint16_t GetPort() const;

		// Main thread: the client events since the last call, in order. A client's Connected precedes its requests in
		// TakeRequests, and its Disconnected follows them.
		[[nodiscard]] std::vector<ClientEvent> TakeClientEvents();

		// Main thread: the queued requests since the last call, in arrival order (per client, in the order sent).
		[[nodiscard]] std::vector<InboundRequest> TakeRequests();

		// Main thread: frames `message` and appends it to `client`'s outbound queue; returns at once, never waiting on the
		// socket (see the class comment). Errors: NotFound for a client that is gone (its Disconnected event is or was
		// queued); Io when the queue would exceed MaxQueuedSendBytes, after which the connection is closed and its
		// Disconnected queued.
		[[nodiscard]] Status Send(ClientId client, const Json& message);

		// Main thread: closes the client's connection (session.shutdown answers first, then the editor closes everything).
		void Disconnect(ClientId client);

		// The authenticated connections right now.
		[[nodiscard]] size_t GetClientCount() const;
	private:
		// The specification, the watchdog back-reference, the listener, the connections with their decoders, outbound
		// queues and handshake state, the event and request queues with their lock, the stop flag and the I/O thread
		// (ProtocolServer.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
