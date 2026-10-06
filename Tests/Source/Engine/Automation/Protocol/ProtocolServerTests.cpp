#include "TestsPCH.h"

#include "Engine/Automation/Protocol/ProtocolServer.h"

#include "Engine/Automation/Protocol/Framing.h"
#include "Engine/Automation/Protocol/Handshake.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Platform/Socket.h"

#include <atomic>
#include <thread>

namespace Engine {

	static constexpr std::chrono::milliseconds ProtocolTimeout{ 10000 };

	namespace {

		// A raw client of the server: frames out, frames in.
		class TestConnection
		{
		public:
			// Connects to the server's port; check IsConnected (no doctest assertion here: client threads use it too).
			explicit TestConnection(uint16_t port)
				: m_Socket(Socket::Connect(port, ProtocolTimeout))
			{
			}

			[[nodiscard]] bool IsConnected() const { return m_Socket.has_value(); }

			Status SendRaw(std::string_view bytes)
			{
				if (!m_Socket.has_value())
					return std::unexpected(m_Socket.error());
				return m_Socket->Send(std::as_bytes(std::span(bytes.data(), bytes.size())), ProtocolTimeout);
			}

			Status SendMessage(std::string_view json)
			{
				return SendRaw(EncodeFrame(json));
			}

			// The next message, or an error: Io "closed" when the server closed the connection.
			Result<Json> ReceiveMessage()
			{
				while (true)
				{
					ENGINE_TRY_ASSIGN(std::optional<std::string> payload, m_Decoder.Next());
					if (payload.has_value())
						return JsonReader::Parse(*payload);
					if (!m_Socket.has_value())
						return std::unexpected(m_Socket.error());
					std::array<std::byte, 4096> buffer{};
					ENGINE_TRY_ASSIGN(const size_t received, m_Socket->Receive(buffer, ProtocolTimeout));
					if (received == 0)
						return MakeError(ErrorCode::Io, "closed");
					m_Decoder.Append(std::span(buffer.data(), received));
				}
			}

			// True when the server closed the connection without sending anything more: an orderly close or a reset. A
			// receive timeout means the connection is still open, so it is false (and costs ProtocolTimeout).
			bool IsClosedByServer()
			{
				if (!m_Socket.has_value())
					return m_Socket.error().GetCode() != ErrorCode::Timeout;
				std::array<std::byte, 64> buffer{};
				const Result<size_t> received = m_Socket->Receive(buffer, ProtocolTimeout);
				return received.has_value() ? *received == 0 : received.error().GetCode() != ErrorCode::Timeout;
			}

			// Says hello with `token` and returns the answer the I/O thread sends, if any.
			Result<Json> SayHello(std::string_view token);
		private:
			Result<Socket> m_Socket;
			FrameDecoder m_Decoder;
		};

		// A controllable I/O-thread clock (ProtocolServerSpecification::TimeSource): the steady clock plus an offset the test
		// advances, so timeouts are tested without waiting.
		class TestTimeSource
		{
		public:
			void Advance(std::chrono::milliseconds amount) { m_OffsetMilliseconds += amount.count(); }

			[[nodiscard]] std::function<std::chrono::steady_clock::time_point()> GetFunction()
			{
				return [this]()
				{
					return std::chrono::steady_clock::now() + std::chrono::milliseconds(m_OffsetMilliseconds.load());
				};
			}
		private:
			std::atomic<int64_t> m_OffsetMilliseconds = 0;
		};

	}

	static std::string MakeHello(std::string_view token, std::string_view version = "1.0")
	{
		return std::format(R"({{"jsonrpc":"2.0","id":1,"method":"session.hello","params":{{"token":"{}","protocolVersion":"{}","client":{{"name":"engine-tests","version":"1"}}}}}})",
			token, version);
	}

	Result<Json> TestConnection::SayHello(std::string_view token)
	{
		ENGINE_TRY(SendMessage(MakeHello(token)));
		return ReceiveMessage();
	}

	static Scope<ProtocolServer> StartTestServer(ProtocolServerSpecification specification, Watchdog& watchdog)
	{
		Result<Scope<ProtocolServer>> server = ProtocolServer::Start(specification, watchdog);
		REQUIRE_MESSAGE(server.has_value(), server.error().ToString());
		return std::move(*server);
	}

	static Scope<ProtocolServer> StartTestServer(const std::string& token, Watchdog& watchdog)
	{
		return StartTestServer(ProtocolServerSpecification{ .Port = 0, .Token = token }, watchdog);
	}

	// Drains `server` as the main thread does, answering each session.hello, until `count` clients have connected and been
	// answered (at most a million polls); returns the ids of the clients that connected.
	static std::vector<ClientId> AcceptClients(ProtocolServer& server, size_t count)
	{
		std::vector<ClientId> connected;
		size_t answered = 0;
		for (int attempt = 0; attempt < 1000000 && answered < count; ++attempt)
		{
			for (const ClientEvent& event : server.TakeClientEvents())
			{
				if (event.Kind == ClientEventKind::Connected)
					connected.push_back(event.Client);
			}
			for (const InboundRequest& request : server.TakeRequests())
			{
				CHECK(server.Send(request.Client, Json{ { "jsonrpc", "2.0" }, { "id", request.Request.Id }, { "result", Json::object() } }).has_value());
				answered += request.Request.Method == "session.hello" ? 1 : 0;
			}
			std::this_thread::yield();
		}
		return connected;
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ProtocolServer: a client says hello, the main thread answers and the client disconnects" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			REQUIRE(server->GetPort() != 0);

			std::atomic<bool> done = false;
			Result<Json> answer = Json();
			std::thread client([port = server->GetPort(), &token, &answer, &done]()
			{
				TestConnection connection(port);
				const Status sent = connection.SendMessage(MakeHello(token));
				answer = sent.has_value() ? connection.ReceiveMessage() : Result<Json>(std::unexpected(sent.error()));
				done = true;
			});

			// The main thread drains the queue and answers, as AutomationServer::Pump does.
			std::vector<ClientEvent> events;
			while (!done)
			{
				for (ClientEvent& event : server->TakeClientEvents())
					events.push_back(std::move(event));
				for (const InboundRequest& request : server->TakeRequests())
				{
					CHECK(request.Request.Method == "session.hello");
					CHECK(server->Send(request.Client, Json{ { "jsonrpc", "2.0" }, { "id", request.Request.Id }, { "result", Json{ { "ok", true } } } }).has_value());
				}
				std::this_thread::yield();
			}
			client.join();
			REQUIRE_MESSAGE(answer.has_value(), answer.error().ToString());
			CHECK((*answer)["result"]["ok"] == Json(true));
			REQUIRE_FALSE(events.empty());
			CHECK(events[0].Kind == ClientEventKind::Connected);
			CHECK(events[0].Name == "engine-tests");

			// The client's socket closed when its thread ended.
			bool disconnected = false;
			for (int attempt = 0; attempt < 1000000 && !disconnected; ++attempt)
			{
				for (const ClientEvent& event : server->TakeClientEvents())
					disconnected = disconnected || event.Kind == ClientEventKind::Disconnected;
				std::this_thread::yield();
			}
			CHECK(disconnected);
		}

		TEST_CASE("ProtocolServer: three bad tokens close the connection" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			TestConnection connection(server->GetPort());
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				REQUIRE(connection.SendMessage(MakeHello(std::string(AuthTokenLength, 'f'))).has_value());
				const Result<Json> answer = connection.ReceiveMessage();
				REQUIRE(answer.has_value());
				CHECK((*answer)["error"]["code"] == Json(-32008));
			}
			CHECK(connection.IsClosedByServer());
			CHECK(server->TakeRequests().empty());
		}

		TEST_CASE("ProtocolServer: a request before session.hello is Unauthorized" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			TestConnection connection(server->GetPort());
			REQUIRE(connection.SendMessage(R"({"jsonrpc":"2.0","id":5,"method":"scene.get","params":{}})").has_value());
			const Result<Json> answer = connection.ReceiveMessage();
			REQUIRE(answer.has_value());
			CHECK((*answer)["id"] == Json(5));
			CHECK((*answer)["error"]["code"] == Json(-32008));
		}

		TEST_CASE("ProtocolServer: an incompatible protocol version is rejected and closes the connection" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			TestConnection connection(server->GetPort());
			REQUIRE(connection.SendMessage(MakeHello(token, "2.0")).has_value());
			const Result<Json> answer = connection.ReceiveMessage();
			REQUIRE(answer.has_value());
			CHECK((*answer)["error"]["code"] == Json(-32009));
			CHECK(connection.IsClosedByServer());
		}

		TEST_CASE("ProtocolServer: an HTTP probe closes the socket without a response" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			TestConnection connection(server->GetPort());
			REQUIRE(connection.SendRaw("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").has_value());
			CHECK(connection.IsClosedByServer());
		}

		TEST_CASE("ProtocolServer: an oversized frame closes the connection" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			TestConnection connection(server->GetPort());
			REQUIRE(connection.SendRaw(std::format("Content-Length: {}\r\n\r\n", MaxFramePayloadBytes + 1)).has_value());
			CHECK(connection.IsClosedByServer());
		}

		TEST_CASE("ProtocolServer: a hello during a stall is answered Busy naming the phase and can be retried" * doctest::skip(true))
		{
			// A heartbeat an hour old: the watchdog reports a stall at once, without the test waiting for one.
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now() - std::chrono::hours(1));
			watchdog.SetPhase("Automation:debug.stall");
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			TestConnection connection(server->GetPort());
			const Result<Json> busy = connection.SayHello(token);
			REQUIRE(busy.has_value());
			CHECK((*busy)["error"]["code"] == Json(-32007));
			CHECK((*busy)["error"]["data"]["phase"] == Json("Automation:debug.stall"));
			CHECK(server->TakeClientEvents().empty()); // still unauthenticated, and not a failure

			// The main thread recovers; the same hello now authenticates (a fourth Busy hello would not have closed it).
			watchdog.Heartbeat(std::chrono::steady_clock::now());
			REQUIRE(connection.SendMessage(MakeHello(token)).has_value());
			CHECK(AcceptClients(*server, 1).size() == 1);
			const Result<Json> answer = connection.ReceiveMessage();
			REQUIRE(answer.has_value());
			CHECK(answer->contains("result"));
		}

		TEST_CASE("ProtocolServer: a fifth authenticated client is refused and closed" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			std::vector<Scope<TestConnection>> connections;
			for (size_t index = 0; index < MaxAutomationClients; ++index)
			{
				connections.push_back(CreateScope<TestConnection>(server->GetPort()));
				REQUIRE(connections.back()->SendMessage(MakeHello(token)).has_value());
			}
			REQUIRE(AcceptClients(*server, MaxAutomationClients).size() == MaxAutomationClients);
			CHECK(server->GetClientCount() == MaxAutomationClients);

			TestConnection fifth(server->GetPort());
			const Result<Json> refused = fifth.SayHello(token);
			REQUIRE(refused.has_value());
			CHECK((*refused)["error"]["code"] == Json(-32002));
			CHECK(fifth.IsClosedByServer());
			CHECK(server->GetClientCount() == MaxAutomationClients);
		}

		TEST_CASE("ProtocolServer: connections still in the handshake have their own cap and do not take client slots" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			const ProtocolServerSpecification specification{ .Port = 0, .Token = token, .MaxPendingHandshakes = 2 };
			Scope<ProtocolServer> server = StartTestServer(specification, watchdog);
			TestConnection first(server->GetPort());
			TestConnection second(server->GetPort());
			REQUIRE(first.IsConnected());
			REQUIRE(second.IsConnected());
			TestConnection third(server->GetPort()); // beyond MaxPendingHandshakes
			CHECK(third.IsClosedByServer());

			// The idle ones never counted against MaxClients: a client that says hello is served.
			REQUIRE(first.SendMessage(MakeHello(token)).has_value());
			CHECK(AcceptClients(*server, 1).size() == 1);
		}

		TEST_CASE("ProtocolServer: a connection that does not complete session.hello in time is closed" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			TestTimeSource clock;
			ProtocolServerSpecification specification{ .Port = 0, .Token = token };
			specification.TimeSource = clock.GetFunction();
			Scope<ProtocolServer> server = StartTestServer(specification, watchdog);
			TestConnection idle(server->GetPort());
			REQUIRE(idle.IsConnected());
			clock.Advance(specification.HandshakeTimeout + std::chrono::milliseconds(1));
			CHECK(idle.IsClosedByServer());
			CHECK(server->TakeClientEvents().empty()); // unauthenticated connections vanish silently
		}

		TEST_CASE("ProtocolServer: Send never waits for a client that stops reading and closes it past the queue cap" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			const ProtocolServerSpecification specification{ .Port = 0, .Token = token, .MaxQueuedSendBytes = 1024 * 1024 };
			Scope<ProtocolServer> server = StartTestServer(specification, watchdog);
			TestConnection reader(server->GetPort());
			REQUIRE(reader.SendMessage(MakeHello(token)).has_value());
			const std::vector<ClientId> clients = AcceptClients(*server, 1);
			REQUIRE(clients.size() == 1);

			// The client never reads again: Sends keep returning at once until the queue passes its cap, then the connection
			// is closed and reported. 64 KB messages exceed any host's socket buffers plus the cap within 4096 Sends.
			const Json large{ { "jsonrpc", "2.0" }, { "method", "test.noise" }, { "params", Json{ { "text", std::string(64 * 1024, 'x') } } } };
			Status sent;
			for (int attempt = 0; attempt < 4096 && sent.has_value(); ++attempt)
				sent = server->Send(clients[0], large);
			REQUIRE_FALSE(sent.has_value());
			CHECK(sent.error().GetCode() == ErrorCode::Io);
			bool disconnected = false;
			for (int attempt = 0; attempt < 1000000 && !disconnected; ++attempt)
			{
				for (const ClientEvent& event : server->TakeClientEvents())
					disconnected = disconnected || event.Kind == ClientEventKind::Disconnected;
				std::this_thread::yield();
			}
			CHECK(disconnected);
		}

		TEST_CASE("ProtocolServer: Stop closes every connection and the port" * doctest::skip(true))
		{
			const std::string token(AuthTokenLength, 'e');
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			Scope<ProtocolServer> server = StartTestServer(token, watchdog);
			TestConnection connection(server->GetPort());
			server->Stop();
			CHECK(server->GetPort() == 0);
			CHECK(connection.IsClosedByServer());
			server->Stop();
		}
	}

}
