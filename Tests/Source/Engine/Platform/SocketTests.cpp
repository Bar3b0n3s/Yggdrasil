#include "TestsPCH.h"

#include "Engine/Platform/Socket.h"

#include <thread>

namespace Engine {

	static constexpr std::chrono::milliseconds SocketTimeout{ 10000 };

	// Receives exactly `size` bytes, or fewer when the peer closes first.
	static Result<std::vector<std::byte>> ReceiveAll(Socket& socket, size_t size)
	{
		std::vector<std::byte> received;
		std::array<std::byte, 4096> buffer{};
		while (received.size() < size)
		{
			ENGINE_TRY_ASSIGN(const size_t count, socket.Receive(buffer, SocketTimeout));
			if (count == 0)
				break;
			received.insert(received.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
		}
		return received;
	}

	// A deterministic payload of `size` bytes.
	static std::vector<std::byte> MakePayload(size_t size)
	{
		std::vector<std::byte> payload(size);
		for (size_t index = 0; index < size; ++index)
			payload[index] = static_cast<std::byte>((index * 31 + 7) % 251);
		return payload;
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("Socket: loopback echo round trip" * doctest::skip(true))
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			const uint16_t port = listener->GetPort();
			REQUIRE(port != 0);

			// The server echoes one message back and then waits for the client to close.
			const std::vector<std::byte> payload = MakePayload(1 << 20); // more than any socket buffer
			Status serverStatus;
			bool serverSawClose = false;
			std::thread server([&listener, &payload, &serverStatus, &serverSawClose]()
			{
				Result<Socket> connection = listener->Accept(SocketTimeout);
				if (!connection.has_value())
				{
					serverStatus = std::unexpected(connection.error());
					return;
				}
				Result<std::vector<std::byte>> received = ReceiveAll(*connection, payload.size());
				if (!received.has_value())
				{
					serverStatus = std::unexpected(received.error());
					return;
				}
				serverStatus = connection->Send(*received, SocketTimeout);
				std::array<std::byte, 16> rest{};
				const Result<size_t> closed = connection->Receive(rest, SocketTimeout);
				serverSawClose = closed.has_value() && *closed == 0;
			});

			// Nothing may leave the scope before the server thread is joined, so the client's results are checked after it.
			Status clientSent;
			Result<std::vector<std::byte>> echoed = std::vector<std::byte>();
			bool wasOpen = false;
			Result<Socket> client = Socket::Connect(port, SocketTimeout);
			if (client.has_value())
			{
				wasOpen = client->IsOpen();
				clientSent = client->Send(payload, SocketTimeout);
				if (clientSent.has_value())
					echoed = ReceiveAll(*client, payload.size());
				client->Close();
			}
			else
			{
				clientSent = std::unexpected(client.error());
			}
			server.join();

			REQUIRE_MESSAGE(clientSent.has_value(), clientSent.error().ToString());
			CHECK(wasOpen);
			CHECK_FALSE(client->IsOpen());
			REQUIRE_MESSAGE(serverStatus.has_value(), serverStatus.error().ToString());
			REQUIRE(echoed.has_value());
			CHECK(*echoed == payload);
			CHECK(serverSawClose);
		}

		TEST_CASE("Socket: one thread sends while another receives on the same connection" * doctest::skip(true))
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			Result<Socket> client = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(client.has_value());
			Result<Socket> accepted = listener->Accept(SocketTimeout);
			REQUIRE(accepted.has_value());

			// Both directions carry more than a socket buffer at the same time: the client sends and then receives on its
			// own thread, a reader thread receives on the server connection while this thread sends on that same connection.
			const std::vector<std::byte> payload = MakePayload(512 * 1024);
			Status clientSent;
			Result<std::vector<std::byte>> clientReceived = std::vector<std::byte>();
			Result<std::vector<std::byte>> serverReceived = std::vector<std::byte>();
			std::thread clientThread([&client, &payload, &clientSent, &clientReceived]()
			{
				clientSent = client->Send(payload, SocketTimeout);
				clientReceived = ReceiveAll(*client, payload.size());
			});
			std::thread reader([&accepted, &payload, &serverReceived]()
			{
				serverReceived = ReceiveAll(*accepted, payload.size());
			});
			const Status serverSent = accepted->Send(payload, SocketTimeout);
			reader.join();
			clientThread.join();

			REQUIRE_MESSAGE(serverSent.has_value(), serverSent.error().ToString());
			REQUIRE_MESSAGE(clientSent.has_value(), clientSent.error().ToString());
			REQUIRE(serverReceived.has_value());
			REQUIRE(clientReceived.has_value());
			CHECK(*serverReceived == payload);
			CHECK(*clientReceived == payload);
		}

		TEST_CASE("Socket: WaitAny reports a pending connection and the readable connections" * doctest::skip(true))
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());

			// Nothing pending: an empty readiness once the timeout expires.
			const Result<SocketReadiness> idle = Socket::WaitAny({}, &*listener, std::chrono::milliseconds(20));
			REQUIRE(idle.has_value());
			CHECK(idle->IsEmpty());

			Result<Socket> first = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(first.has_value());
			const Result<SocketReadiness> pending = Socket::WaitAny({}, &*listener, SocketTimeout);
			REQUIRE(pending.has_value());
			CHECK(pending->ListenerReady);
			CHECK(pending->ReadableSockets.empty());
			Result<Socket> firstServer = listener->Accept(SocketTimeout);
			REQUIRE(firstServer.has_value());
			Result<Socket> second = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(second.has_value());
			Result<Socket> secondServer = listener->Accept(SocketTimeout);
			REQUIRE(secondServer.has_value());

			// Only the second connection has data.
			const std::array<std::byte, 3> message = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };
			REQUIRE(second->Send(message, SocketTimeout).has_value());
			const std::array<const Socket*, 2> servers = { &*firstServer, &*secondServer };
			const Result<SocketReadiness> readable = Socket::WaitAny(servers, &*listener, SocketTimeout);
			REQUIRE(readable.has_value());
			CHECK_FALSE(readable->ListenerReady);
			CHECK(readable->ReadableSockets == std::vector<size_t>{ 1 });
			std::array<std::byte, 8> buffer{};
			const Result<size_t> received = secondServer->Receive(buffer, SocketTimeout);
			REQUIRE(received.has_value());
			CHECK(*received == message.size());

			// A peer that closed makes its connection readable too, and Receive then returns 0.
			first->Close();
			const Result<SocketReadiness> closed = Socket::WaitAny(servers, nullptr, SocketTimeout);
			REQUIRE(closed.has_value());
			CHECK(closed->ReadableSockets == std::vector<size_t>{ 0 });
			const Result<size_t> end = firstServer->Receive(buffer, SocketTimeout);
			REQUIRE(end.has_value());
			CHECK(*end == 0);
		}

		TEST_CASE("Socket: Accept and Receive time out when nothing arrives" * doctest::skip(true))
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			const Result<Socket> none = listener->Accept(std::chrono::milliseconds(50));
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::Timeout);

			Result<Socket> client = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(client.has_value());
			Result<Socket> accepted = listener->Accept(SocketTimeout);
			REQUIRE(accepted.has_value());
			std::array<std::byte, 8> buffer{};
			const Result<size_t> silent = accepted->Receive(buffer, std::chrono::milliseconds(50));
			REQUIRE_FALSE(silent.has_value());
			CHECK(silent.error().GetCode() == ErrorCode::Timeout);
		}

		TEST_CASE("Socket: a port in use, a closed port and port 0 are reported" * doctest::skip(true))
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			const uint16_t port = listener->GetPort();

			const Result<SocketListener> second = SocketListener::Listen(port);
			REQUIRE_FALSE(second.has_value());
			CHECK(second.error().GetCode() == ErrorCode::AlreadyExists);

			listener->Close();
			const Result<Socket> refused = Socket::Connect(port, SocketTimeout);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Io);

			const Result<Socket> zero = Socket::Connect(0, SocketTimeout);
			REQUIRE_FALSE(zero.has_value());
			CHECK(zero.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Socket: sending to a closed peer fails with Io" * doctest::skip(true))
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			Result<Socket> client = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(client.has_value());
			{
				Result<Socket> accepted = listener->Accept(SocketTimeout);
				REQUIRE(accepted.has_value());
			} // the server side closes

			// The first sends may still be buffered; within a few the closed peer must surface as Io, never SIGPIPE.
			const std::vector<std::byte> payload = MakePayload(64 * 1024);
			Status sent;
			for (int attempt = 0; attempt < 64 && sent.has_value(); ++attempt)
				sent = client->Send(payload, SocketTimeout);
			REQUIRE_FALSE(sent.has_value());
			CHECK(sent.error().GetCode() == ErrorCode::Io);
		}
	}

}
