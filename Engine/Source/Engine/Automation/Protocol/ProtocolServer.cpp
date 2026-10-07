#include "EnginePCH.h"
#include "Engine/Automation/Protocol/ProtocolServer.h"

#include "Engine/Automation/Protocol/Handshake.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Socket.h"

#include <array>
#include <atomic>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

// One I/O thread serves the listener and every connection (§4.11): it waits in Socket::WaitAny for readable connections,
// writable connections that have queued output, and new connections, all at once.
//
// Ownership and locks. Only the I/O thread inserts and erases connections, under State::Lock, which also guards the event
// and request queues; the main thread holds State::Lock while it touches a connection (Send, Disconnect), so a connection
// it reaches cannot be erased meanwhile, and the I/O thread uses its connections without the lock. Each connection's
// outbound queue has its own lock, taken after State::Lock when both are needed. Every write to a socket happens under that
// queue lock, so two writes never overlap (Socket.h's threading rule), and frames never interleave.
//
// Sends. Send appends the frame to the queue; when the queue was empty it also writes what fits at once with a
// non-blocking SendAvailable (nothing waits), so a response leaves without waiting for the I/O thread's next turn. Whatever
// does not fit stays queued and the I/O thread drains it as the socket becomes writable.
//
// Closing. The I/O thread closes a connection at the start of its next turn: at once when it is broken (a frame violation,
// a failed write, an overflowing queue, the peer's close), after its queued output is written when it is closing (the
// MaxAuthFailures-th failure, an incompatible version, too many clients, Disconnect), and when its deadline passes (the
// handshake deadline, or a closing connection's time to write its last answer). A closing connection is still read, and
// what it sends is discarded, so the peer never sees a reset before it has read the last answer.

namespace Engine {

	namespace Utils {

		// The receive buffer of the I/O thread. Small enough that erasing returned frames from a decoder's buffer stays cheap.
		constexpr size_t ReceiveChunkBytes = 16 * 1024;
		// Accepts per turn, so a flood of connections cannot starve the established ones.
		constexpr size_t MaxAcceptsPerTurn = 16;

		[[nodiscard]] static bool IsWellFormedToken(std::string_view token)
		{
			if (token.size() != AuthTokenLength)
				return false;
			for (const char character : token)
			{
				if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
					return false;
			}
			return true;
		}

		// The frame of `message`. Strings the engine produces are UTF-8; anything else is replaced rather than thrown on.
		[[nodiscard]] static std::string FrameMessage(const Json& message)
		{
			return EncodeFrame(message.dump(-1, ' ', false, Json::error_handler_t::replace));
		}

	}

	struct ProtocolServer::State
	{
		struct Connection
		{
			ClientId Id = NoClient;
			Socket Link;
			FrameDecoder Decoder;
			// I/O thread only (Authenticated is also written under State::Lock, where the main thread may read it).
			bool Authenticated = false;
			uint32_t Failures = 0;
			std::chrono::steady_clock::time_point Deadline{}; // the handshake deadline, or a closing connection's last moment
			bool Closing = false;                             // stop decoding; close once the queue is written
			std::string Name{};
			std::string Version{};
			// Guarded by OutboxLock.
			std::mutex OutboxLock;
			std::deque<std::string> Outbox{}; // framed messages, oldest first
			size_t OutboxOffset = 0;          // bytes of Outbox.front() already written
			size_t QueuedBytes = 0;           // every byte of Outbox, the written part of the front included
			bool Broken = false;              // close at once, discarding the queue
			bool CloseRequested = false;      // Disconnect: write the queue, then close

			Connection(Socket link, size_t maxHandshakePayloadBytes)
				: Link(std::move(link)), Decoder(maxHandshakePayloadBytes)
			{
			}
		};

		ProtocolServerSpecification Specification{};
		Watchdog* PhaseMarker = nullptr; // documented back-reference: outlives the server
		std::optional<SocketListener> Listener{};
		std::atomic<uint16_t> Port = 0;
		std::atomic<bool> StopRequested = false;
		std::atomic<size_t> AuthenticatedCount = 0;
		std::thread IoThread{};
		bool Stopped = false; // main thread

		std::mutex Lock; // guards Connections' membership, Events, Requests and every Connection::Authenticated write
		std::map<ClientId, Scope<Connection>> Connections{};
		std::vector<ClientEvent> Events{};
		std::vector<InboundRequest> Requests{};
		ClientId NextClientId = 1; // I/O thread
		// Unauthenticated connections rejected since the last report, the latest reason, and when the next report may be
		// logged (I/O thread).
		size_t RejectedSinceReport = 0;
		std::string LastRejection{};
		std::chrono::steady_clock::time_point NextRejectionReport{};

		[[nodiscard]] std::chrono::steady_clock::time_point Now() const
		{
			return Specification.TimeSource ? Specification.TimeSource() : std::chrono::steady_clock::now();
		}

		// The I/O thread.
		void Run();
		void CloseFinishedConnections(std::chrono::steady_clock::time_point now);
		void CloseConnection(ClientId id);
		void AcceptConnections(std::chrono::steady_clock::time_point now);
		// Counts the rejection of an unauthenticated connection and logs it, or the rejections counted since the last report,
		// at most once per RejectionReportInterval (the class comment of ProtocolServer).
		void ReportRejection(std::string reason, std::chrono::steady_clock::time_point now);
		void FlushRejections(std::chrono::steady_clock::time_point now);
		void ReadFrom(Connection& connection, std::chrono::steady_clock::time_point now);
		void HandlePayload(Connection& connection, std::string_view payload, std::chrono::steady_clock::time_point now);
		void HandleHello(Connection& connection, RpcRequest request, std::chrono::steady_clock::time_point now);
		// An authentication failure answered with `answer` (unless the request was a notification): the MaxAuthFailures-th
		// closes the connection after its answer.
		void FailAuthentication(Connection& connection, const RpcRequest* request, const Json& answer, std::string_view reason);
		// Queues `message` on `connection` and writes what fits at once (I/O thread).
		void Answer(Connection& connection, const Json& message);
		void MarkBroken(Connection& connection, std::string_view reason);
		void MarkClosing(Connection& connection, std::chrono::steady_clock::time_point now);
		// Writes as much of the queue as the socket accepts now; the caller holds connection.OutboxLock.
		static void FlushLocked(Connection& connection);
	};

	void ProtocolServer::State::Run()
	{
		while (!StopRequested.load())
		{
			CloseFinishedConnections(Now());

			// The connections are stable until the next turn: only this thread erases them, at the start of a turn.
			std::vector<Connection*> readers;
			std::vector<const Socket*> readerSockets;
			std::vector<Connection*> writers;
			std::vector<const Socket*> writerSockets;
			for (const auto& [id, connection] : Connections)
			{
				readers.push_back(connection.get());
				readerSockets.push_back(&connection->Link);
				const std::scoped_lock outbox(connection->OutboxLock);
				if (!connection->Outbox.empty() && !connection->Broken)
				{
					writers.push_back(connection.get());
					writerSockets.push_back(&connection->Link);
				}
			}

			const Result<SocketReadiness> ready = Socket::WaitAny(readerSockets, writerSockets, &*Listener, Specification.PollInterval);
			if (!ready)
			{
				// Not expected with open sockets; never spin on it.
				ENGINE_CORE_ERROR("Automation server: waiting on its sockets failed: {}", ready.error().ToString());
				std::this_thread::sleep_for(Specification.PollInterval);
				continue;
			}

			const std::chrono::steady_clock::time_point now = Now();
			if (ready->ListenerReady)
				AcceptConnections(now);
			FlushRejections(now);
			for (const size_t index : ready->ReadableSockets)
				ReadFrom(*readers[index], now);
			for (const size_t index : ready->WritableSockets)
			{
				Connection& connection = *writers[index];
				const std::scoped_lock outbox(connection.OutboxLock);
				FlushLocked(connection);
			}
		}
	}

	void ProtocolServer::State::CloseFinishedConnections(std::chrono::steady_clock::time_point now)
	{
		std::vector<ClientId> finished;
		for (const auto& [id, connection] : Connections)
		{
			bool broken = false;
			bool written = false;
			{
				const std::scoped_lock outbox(connection->OutboxLock);
				broken = connection->Broken;
				written = connection->Outbox.empty();
				if (connection->CloseRequested && !connection->Closing)
				{
					connection->Closing = true;
					connection->Deadline = now + Specification.HandshakeTimeout;
				}
			}
			const bool expired = (!connection->Authenticated || connection->Closing) && now > connection->Deadline;
			if (broken || (connection->Closing && written) || expired)
				finished.push_back(id);
		}
		for (const ClientId id : finished)
			CloseConnection(id);
	}

	void ProtocolServer::State::CloseConnection(ClientId id)
	{
		Scope<Connection> connection;
		{
			const std::scoped_lock lock(Lock);
			const auto found = Connections.find(id);
			if (found == Connections.end())
				return;
			connection = std::move(found->second);
			Connections.erase(found);
			if (connection->Authenticated)
			{
				AuthenticatedCount.fetch_sub(1);
				Events.push_back(ClientEvent{ .Kind = ClientEventKind::Disconnected, .Client = id, .Name = connection->Name, .Version = connection->Version });
			}
		}
		connection->Link.Close();
	}

	void ProtocolServer::State::AcceptConnections(std::chrono::steady_clock::time_point now)
	{
		for (size_t turn = 0; turn < Utils::MaxAcceptsPerTurn; ++turn)
		{
			Result<Socket> accepted = Listener->Accept(std::chrono::milliseconds(0));
			if (!accepted)
			{
				if (accepted.error().GetCode() != ErrorCode::Timeout)
					ENGINE_CORE_WARN("Automation server: accepting a connection failed: {}", accepted.error().ToString());
				return;
			}

			size_t handshaking = 0;
			for (const auto& [id, connection] : Connections)
				handshaking += connection->Authenticated ? 0 : 1;
			if (handshaking >= Specification.MaxPendingHandshakes)
			{
				// Idle sockets from any local process must not pile up (§13.2): closed at once.
				ReportRejection(std::format("refused a connection: {} connections are already in the handshake", handshaking), now);
				accepted->Close();
				continue;
			}

			Scope<Connection> connection = CreateScope<Connection>(std::move(*accepted), Specification.MaxHandshakePayloadBytes);
			connection->Id = NextClientId++;
			connection->Deadline = now + Specification.HandshakeTimeout;
			const std::scoped_lock lock(Lock);
			Connections.emplace(connection->Id, std::move(connection));
		}
	}

	void ProtocolServer::State::ReportRejection(std::string reason, std::chrono::steady_clock::time_point now)
	{
		++RejectedSinceReport;
		LastRejection = std::move(reason);
		FlushRejections(now);
	}

	void ProtocolServer::State::FlushRejections(std::chrono::steady_clock::time_point now)
	{
		if (RejectedSinceReport == 0 || now < NextRejectionReport)
			return;
		if (RejectedSinceReport == 1)
			ENGINE_CORE_WARN("Automation server: {}", LastRejection);
		else
			ENGINE_CORE_WARN("Automation server: rejected {} unauthenticated connections in the last {} s; the latest: {}", RejectedSinceReport,
				RejectionReportInterval.count(), LastRejection);
		RejectedSinceReport = 0;
		LastRejection.clear();
		NextRejectionReport = now + RejectionReportInterval;
	}

	void ProtocolServer::State::ReadFrom(Connection& connection, std::chrono::steady_clock::time_point now)
	{
		{
			const std::scoped_lock outbox(connection.OutboxLock);
			if (connection.Broken)
				return;
		}
		std::array<std::byte, Utils::ReceiveChunkBytes> buffer{};
		const Result<size_t> received = connection.Link.Receive(buffer, std::chrono::milliseconds(0));
		if (!received)
		{
			if (received.error().GetCode() != ErrorCode::Timeout)
				MarkBroken(connection, received.error().GetMessageText());
			return;
		}
		if (*received == 0)
		{
			MarkBroken(connection, "the client closed the connection");
			return;
		}
		if (connection.Closing)
			return; // drained and discarded while the last answer is written

		connection.Decoder.Append(std::span<const std::byte>(buffer.data(), *received));
		while (!connection.Closing)
		{
			Result<std::optional<std::string>> payload = connection.Decoder.Next();
			if (!payload)
			{
				// An HTTP probe, an oversized frame, invalid UTF-8 or nesting over 128: closed without a response (§13.2).
				if (connection.Authenticated)
					ENGINE_CORE_WARN("Automation client '{}' ({}) sent an invalid frame: {}", connection.Name, connection.Id, payload.error().GetMessageText());
				else
					ReportRejection(std::format("closed connection {}: {}", connection.Id, payload.error().GetMessageText()), now);
				MarkBroken(connection, payload.error().GetMessageText());
				return;
			}
			if (!payload->has_value())
				return;
			HandlePayload(connection, **payload, now);
		}
	}

	void ProtocolServer::State::HandlePayload(Connection& connection, std::string_view payload, std::chrono::steady_clock::time_point now)
	{
		std::expected<RpcRequest, RpcParseFailure> parsed = ParseRpcRequest(payload);
		if (!parsed)
		{
			const Json answer = MakeErrorResponse(parsed.error().Id, parsed.error().Code, parsed.error().Failure, Json(), Json());
			if (!connection.Authenticated)
				FailAuthentication(connection, nullptr, answer, "a payload that is not a request");
			else
				Answer(connection, answer);
			return;
		}

		RpcRequest request = std::move(*parsed);
		if (!connection.Authenticated)
		{
			if (request.Method != "session.hello")
			{
				const Json answer = MakeErrorResponse(request.Id, RpcErrorCode::Unauthorized,
					Error(ErrorCode::PermissionDenied, "session.hello must be the first request"), Json(), Json());
				FailAuthentication(connection, &request, answer, "a request before session.hello");
				return;
			}
			HandleHello(connection, std::move(request), now);
			return;
		}

		if (request.Method == "session.hello")
		{
			if (!request.IsNotification)
			{
				Answer(connection, MakeErrorResponse(request.Id, RpcErrorCode::InvalidState, Error(ErrorCode::InvalidState, "this connection already completed session.hello"), Json(), Json()));
			}
			return;
		}
		if (PhaseMarker != nullptr)
		{
			if (const std::optional<std::chrono::milliseconds> stall = PhaseMarker->GetStall(now))
			{
				if (!request.IsNotification)
					Answer(connection, MakeBusyResponse(request.Id, PhaseMarker->GetPhase(), static_cast<uint64_t>(stall->count())));
				return;
			}
		}
		const std::scoped_lock lock(Lock);
		Requests.push_back(InboundRequest{ .Client = connection.Id, .Request = std::move(request) });
	}

	void ProtocolServer::State::HandleHello(Connection& connection, RpcRequest request, std::chrono::steady_clock::time_point now)
	{
		const Result<HelloRequest> hello = ParseHelloRequest(request.Params);
		if (!hello)
		{
			FailAuthentication(connection, &request, MakeErrorResponse(request.Id, RpcErrorCode::InvalidParams, hello.error(), Json(), Json()),
				"malformed session.hello params");
			return;
		}
		const Status accepted = CheckHello(*hello, Specification.Token, Specification.Version);
		if (!accepted)
		{
			if (accepted.error().GetCode() == ErrorCode::Unsupported)
			{
				// An incompatible major version: answered, then closed (not an authentication failure).
				ReportRejection(std::format("connection {} ('{}') speaks an incompatible protocol: {}", connection.Id, hello->ClientName,
									accepted.error().GetMessageText()),
					now);
				if (!request.IsNotification)
					Answer(connection, MakeErrorResponse(request.Id, RpcErrorCode::Unsupported, accepted.error(), Json(), Json()));
				MarkClosing(connection, now);
				return;
			}
			FailAuthentication(connection, &request, MakeErrorResponse(request.Id, RpcErrorCode::Unauthorized, accepted.error(), Json(), Json()),
				"an invalid token");
			return;
		}

		if (AuthenticatedCount.load() >= Specification.MaxClients)
		{
			ENGINE_CORE_WARN("Automation server: refused client '{}': {} clients are already connected", hello->ClientName, AuthenticatedCount.load());
			if (!request.IsNotification)
			{
				Answer(connection, MakeErrorResponse(request.Id, RpcErrorCode::InvalidState, Error(ErrorCode::InvalidState, std::format("too many automation clients (at most {})", Specification.MaxClients)), Json(), Json()));
			}
			MarkClosing(connection, now);
			return;
		}
		if (PhaseMarker != nullptr)
		{
			if (const std::optional<std::chrono::milliseconds> stall = PhaseMarker->GetStall(now))
			{
				// Busy at once, not a failure: the client retries session.hello and never hangs on a frozen editor (§13.2).
				if (!request.IsNotification)
					Answer(connection, MakeBusyResponse(request.Id, PhaseMarker->GetPhase(), static_cast<uint64_t>(stall->count())));
				connection.Deadline = now + Specification.HandshakeTimeout;
				return;
			}
		}

		connection.Name = hello->ClientName;
		connection.Version = hello->ClientVersion;
		// Authenticated: frames up to the protocol's limit from the next one on (the decoder reads each header afresh).
		connection.Decoder.SetMaxPayloadBytes(MaxFramePayloadBytes);
		AuthenticatedCount.fetch_add(1);
		const std::scoped_lock lock(Lock);
		connection.Authenticated = true;
		Events.push_back(ClientEvent{ .Kind = ClientEventKind::Connected, .Client = connection.Id, .Name = connection.Name, .Version = connection.Version });
		Requests.push_back(InboundRequest{ .Client = connection.Id, .Request = std::move(request) });
	}

	void ProtocolServer::State::FailAuthentication(Connection& connection, const RpcRequest* request, const Json& answer, std::string_view reason)
	{
		++connection.Failures;
		const bool answered = request == nullptr || !request->IsNotification;
		if (answered)
			Answer(connection, answer);
		if (connection.Failures >= MaxAuthFailures)
		{
			const std::chrono::steady_clock::time_point now = Now();
			ReportRejection(std::format("closed connection {} after {} failed authentication attempts (the last: {})", connection.Id, connection.Failures, reason),
				now);
			MarkClosing(connection, now);
		}
	}

	void ProtocolServer::State::Answer(Connection& connection, const Json& message)
	{
		std::string frame = Utils::FrameMessage(message);
		const std::scoped_lock outbox(connection.OutboxLock);
		if (connection.Broken)
			return;
		if (connection.QueuedBytes + frame.size() > Specification.MaxQueuedSendBytes)
		{
			// The bound of Send: a client that keeps sending requests (whose ids Busy and handshake answers echo) without
			// reading the answers is closed instead of growing the queue (ADR 0008 decision 27).
			std::string reason = std::format("closed connection {}: {} unread bytes are queued and a {}-byte answer would pass the limit of {}", connection.Id,
				connection.QueuedBytes, frame.size(), Specification.MaxQueuedSendBytes);
			if (connection.Authenticated)
				ENGINE_CORE_WARN("Automation client '{}': {}", connection.Name, reason);
			else
				ReportRejection(std::move(reason), Now());
			connection.Broken = true;
			return;
		}
		connection.QueuedBytes += frame.size();
		connection.Outbox.push_back(std::move(frame));
		FlushLocked(connection);
	}

	void ProtocolServer::State::MarkBroken(Connection& connection, std::string_view reason)
	{
		const std::scoped_lock outbox(connection.OutboxLock);
		if (!connection.Broken && connection.Authenticated)
			ENGINE_CORE_TRACE("Automation client '{}' ({}) connection ends: {}", connection.Name, connection.Id, reason);
		connection.Broken = true;
	}

	void ProtocolServer::State::MarkClosing(Connection& connection, std::chrono::steady_clock::time_point now)
	{
		connection.Closing = true;
		connection.Deadline = now + Specification.HandshakeTimeout;
	}

	void ProtocolServer::State::FlushLocked(Connection& connection)
	{
		while (!connection.Broken && !connection.Outbox.empty())
		{
			const std::string& front = connection.Outbox.front();
			const std::string_view rest = std::string_view(front).substr(connection.OutboxOffset);
			const Result<size_t> sent = connection.Link.SendAvailable(std::as_bytes(std::span(rest.data(), rest.size())));
			if (!sent)
			{
				connection.Broken = true; // the peer is gone; the I/O thread closes it at its next turn
				return;
			}
			if (*sent < rest.size())
			{
				connection.OutboxOffset += *sent;
				return; // the socket is full: the I/O thread waits for writability
			}
			connection.QueuedBytes -= front.size();
			connection.Outbox.pop_front();
			connection.OutboxOffset = 0;
		}
	}

	ProtocolServer::ProtocolServer(ConstructionKey /*key*/, const ProtocolServerSpecification& specification, Watchdog& watchdog)
		: m_State(CreateScope<State>())
	{
		m_State->Specification = specification;
		m_State->PhaseMarker = &watchdog;
	}

	Result<Scope<ProtocolServer>> ProtocolServer::Start(const ProtocolServerSpecification& specification, Watchdog& watchdog)
	{
		ENGINE_CORE_ASSERT(Utils::IsWellFormedToken(specification.Token), "The automation token must be {} lowercase hex digits", AuthTokenLength);
		if (!Utils::IsWellFormedToken(specification.Token))
			return MakeError(ErrorCode::InvalidArgument, "the automation token must be {} lowercase hexadecimal digits", AuthTokenLength);

		const uint32_t backlog = static_cast<uint32_t>(specification.MaxClients + specification.MaxPendingHandshakes + 1);
		ENGINE_TRY_ASSIGN(SocketListener listener, SocketListener::Listen(specification.Port, backlog));
		Scope<ProtocolServer> server = CreateScope<ProtocolServer>(ConstructionKey(), specification, watchdog);
		State& state = *server->m_State;
		state.Port = listener.GetPort();
		state.Listener.emplace(std::move(listener));
		state.IoThread = std::thread([&state]()
		{
			state.Run();
		});
		ENGINE_CORE_INFO("Automation server listening on 127.0.0.1:{}", state.Port.load());
		return server;
	}

	ProtocolServer::~ProtocolServer()
	{
		Stop();
	}

	void ProtocolServer::Stop()
	{
		State& state = *m_State;
		if (state.Stopped)
			return;
		state.Stopped = true;
		state.StopRequested = true;
		if (state.IoThread.joinable())
			state.IoThread.join();

		std::map<ClientId, Scope<State::Connection>> connections;
		{
			const std::scoped_lock lock(state.Lock);
			connections.swap(state.Connections);
			for (const auto& [id, connection] : connections)
			{
				if (connection->Authenticated)
				{
					state.Events.push_back(
						ClientEvent{ .Kind = ClientEventKind::Disconnected, .Client = id, .Name = connection->Name, .Version = connection->Version });
				}
			}
			state.AuthenticatedCount = 0;
		}
		for (auto& [id, connection] : connections)
			connection->Link.Close();
		connections.clear();
		state.Listener.reset();
		state.Port = 0;
	}

	uint16_t ProtocolServer::GetPort() const
	{
		return m_State->Port.load();
	}

	std::vector<ClientEvent> ProtocolServer::TakeClientEvents()
	{
		const std::scoped_lock lock(m_State->Lock);
		std::vector<ClientEvent> events;
		events.swap(m_State->Events);
		return events;
	}

	std::vector<InboundRequest> ProtocolServer::TakeRequests()
	{
		const std::scoped_lock lock(m_State->Lock);
		std::vector<InboundRequest> requests;
		requests.swap(m_State->Requests);
		return requests;
	}

	Status ProtocolServer::Send(ClientId client, const Json& message)
	{
		std::string frame = Utils::FrameMessage(message);
		const std::scoped_lock lock(m_State->Lock);
		const auto found = m_State->Connections.find(client);
		if (found == m_State->Connections.end() || !found->second->Authenticated)
			return MakeError(ErrorCode::NotFound, "automation client {} is not connected", client);

		State::Connection& connection = *found->second;
		const std::scoped_lock outbox(connection.OutboxLock);
		if (connection.Broken || connection.CloseRequested)
			return MakeError(ErrorCode::NotFound, "automation client {} is disconnecting", client);
		if (connection.QueuedBytes + frame.size() > m_State->Specification.MaxQueuedSendBytes)
		{
			// A client that stopped reading delays only itself (ADR 0008 decision 27): it is closed instead.
			connection.Broken = true;
			return MakeError(ErrorCode::Io, "automation client {} has {} unread bytes queued; a {}-byte message would pass the limit of {}, so its "
											"connection is closed",
				client, connection.QueuedBytes, frame.size(), m_State->Specification.MaxQueuedSendBytes);
		}
		connection.QueuedBytes += frame.size();
		connection.Outbox.push_back(std::move(frame));
		if (connection.Outbox.size() == 1)
			State::FlushLocked(connection);
		return {};
	}

	void ProtocolServer::Disconnect(ClientId client)
	{
		const std::scoped_lock lock(m_State->Lock);
		const auto found = m_State->Connections.find(client);
		if (found == m_State->Connections.end())
			return;
		const std::scoped_lock outbox(found->second->OutboxLock);
		found->second->CloseRequested = true;
	}

	size_t ProtocolServer::GetClientCount() const
	{
		return m_State->AuthenticatedCount.load();
	}

}
