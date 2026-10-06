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
		TEST_CASE("Socket: loopback echo round trip")
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

		TEST_CASE("Socket: one thread sends while another receives on the same connection")
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

		TEST_CASE("Socket: WaitAny reports a pending connection and the readable connections")
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

		TEST_CASE("Socket: Accept and Receive time out when nothing arrives")
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

		TEST_CASE("Socket: a port in use, a closed port and port 0 are reported")
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

		TEST_CASE("Socket: sending to a closed peer fails with Io")
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

		TEST_CASE("Socket: Send times out when the peer does not read")
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			Result<Socket> client = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(client.has_value());
			Result<Socket> accepted = listener->Accept(SocketTimeout);
			REQUIRE(accepted.has_value());

			// Sends until the connection's buffers are full, which takes far less than 256 MB on any host (the content is
			// irrelevant). Several sends, because Windows accepts a whole send of any size while its send backlog is below
			// SO_SNDBUF, so only a later send has to wait.
			const std::vector<std::byte> chunk(1024 * 1024);
			Status sent;
			for (int attempt = 0; attempt < 256 && sent.has_value(); ++attempt)
				sent = client->Send(chunk, std::chrono::milliseconds(100));
			REQUIRE_FALSE(sent.has_value());
			CHECK(sent.error().GetCode() == ErrorCode::Timeout);
		}

		TEST_CASE("Socket: SendAvailable sends what fits without waiting and WaitAny reports writability" * doctest::skip(true))
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			Result<Socket> client = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(client.has_value());
			Result<Socket> accepted = listener->Accept(SocketTimeout);
			REQUIRE(accepted.has_value());
			const std::array<const Socket*, 1> writers = { &*client };

			// An empty send buffer is writable at once.
			const Result<SocketReadiness> idle = Socket::WaitAny({}, writers, nullptr, std::chrono::milliseconds(0));
			REQUIRE(idle.has_value());
			CHECK(idle->WritableSockets == std::vector<size_t>{ 0 });

			// The peer never reads, so the buffers fill: SendAvailable returns fewer bytes than offered (eventually 0) and
			// never waits, which a 256 MB budget of 1 MB sends reaches on any host.
			const std::vector<std::byte> chunk(1024 * 1024);
			bool filled = false;
			for (int attempt = 0; attempt < 256 && !filled; ++attempt)
			{
				const Result<size_t> sent = client->SendAvailable(chunk);
				REQUIRE(sent.has_value());
				filled = *sent < chunk.size();
			}
			REQUIRE(filled);
			const Result<SocketReadiness> full = Socket::WaitAny({}, writers, nullptr, std::chrono::milliseconds(0));
			REQUIRE(full.has_value());
			CHECK(full->WritableSockets.empty());

			// Once the peer reads, the connection becomes writable again.
			std::array<std::byte, 64 * 1024> buffer{};
			for (int read = 0; read < 64; ++read)
				CHECK(accepted->Receive(buffer, SocketTimeout).has_value());
			const Result<SocketReadiness> drained = Socket::WaitAny({}, writers, nullptr, SocketTimeout);
			REQUIRE(drained.has_value());
			CHECK(drained->WritableSockets == std::vector<size_t>{ 0 });
		}

		TEST_CASE("Socket: SendAvailable to a closed peer fails with Io" * doctest::skip(true))
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			Result<Socket> client = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(client.has_value());
			Result<Socket> accepted = listener->Accept(SocketTimeout);
			REQUIRE(accepted.has_value());
			accepted->Close();

			// The first sends may still be buffered locally; a reset peer makes a later one fail.
			const std::vector<std::byte> chunk(64 * 1024);
			Result<size_t> sent = size_t{ 0 };
			for (int attempt = 0; attempt < 256 && sent.has_value(); ++attempt)
				sent = client->SendAvailable(chunk);
			REQUIRE_FALSE(sent.has_value());
			CHECK(sent.error().GetCode() == ErrorCode::Io);
		}

		TEST_CASE("Socket: sending nothing succeeds at once")
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			Result<Socket> client = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(client.has_value());
			CHECK(client->Send({}, std::chrono::milliseconds(0)).has_value());
		}

		TEST_CASE("Socket: Close is idempotent and closed objects report Io")
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			const uint16_t port = listener->GetPort();
			Result<Socket> client = Socket::Connect(port, SocketTimeout);
			REQUIRE(client.has_value());
			Result<Socket> accepted = listener->Accept(SocketTimeout);
			REQUIRE(accepted.has_value());

			client->Close();
			client->Close();
			CHECK_FALSE(client->IsOpen());
			const std::array<std::byte, 1> byte = { std::byte{ 42 } };
			const Status sent = client->Send(byte, SocketTimeout);
			REQUIRE_FALSE(sent.has_value());
			CHECK(sent.error().GetCode() == ErrorCode::Io);
			std::array<std::byte, 8> buffer{};
			const Result<size_t> received = client->Receive(buffer, SocketTimeout);
			REQUIRE_FALSE(received.has_value());
			CHECK(received.error().GetCode() == ErrorCode::Io);

			// The peer sees the end of the stream.
			const Result<size_t> end = accepted->Receive(buffer, SocketTimeout);
			REQUIRE(end.has_value());
			CHECK(*end == 0);

			listener->Close();
			listener->Close();
			CHECK(listener->GetPort() == 0);
			const Result<Socket> none = listener->Accept(std::chrono::milliseconds(0));
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::Io);
		}

		TEST_CASE("Socket: a moved connection keeps working and move assignment closes the replaced one")
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			Result<Socket> first = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(first.has_value());
			Result<Socket> firstServer = listener->Accept(SocketTimeout);
			REQUIRE(firstServer.has_value());
			Result<Socket> second = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(second.has_value());
			Result<Socket> secondServer = listener->Accept(SocketTimeout);
			REQUIRE(secondServer.has_value());

			Socket moved(std::move(*first));
			CHECK(moved.IsOpen());
			const std::array<std::byte, 2> message = { std::byte{ 7 }, std::byte{ 9 } };
			REQUIRE(moved.Send(message, SocketTimeout).has_value());
			std::array<std::byte, 8> buffer{};
			const Result<size_t> received = firstServer->Receive(buffer, SocketTimeout);
			REQUIRE(received.has_value());
			CHECK(*received == message.size());

			// Assigning the second connection closes the first: its peer sees the end of the stream.
			moved = std::move(*second);
			CHECK(moved.IsOpen());
			const Result<size_t> end = firstServer->Receive(buffer, SocketTimeout);
			REQUIRE(end.has_value());
			CHECK(*end == 0);
			REQUIRE(moved.Send(message, SocketTimeout).has_value());
			const Result<size_t> again = secondServer->Receive(buffer, SocketTimeout);
			REQUIRE(again.has_value());
			CHECK(*again == message.size());

			SocketListener movedListener(std::move(*listener));
			CHECK(movedListener.GetPort() != 0);
		}

		TEST_CASE("Socket: WaitAny with a zero timeout only checks and reports unread data again")
		{
			Result<SocketListener> listener = SocketListener::Listen(0);
			REQUIRE(listener.has_value());
			Result<Socket> client = Socket::Connect(listener->GetPort(), SocketTimeout);
			REQUIRE(client.has_value());
			Result<Socket> accepted = listener->Accept(SocketTimeout);
			REQUIRE(accepted.has_value());

			const std::array<const Socket*, 1> sockets = { &*accepted };
			const Result<SocketReadiness> idle = Socket::WaitAny(sockets, &*listener, std::chrono::milliseconds(0));
			REQUIRE(idle.has_value());
			CHECK(idle->IsEmpty());

			const std::array<std::byte, 1> byte = { std::byte{ 1 } };
			REQUIRE(client->Send(byte, SocketTimeout).has_value());
			const Result<SocketReadiness> arrived = Socket::WaitAny(sockets, nullptr, SocketTimeout);
			REQUIRE(arrived.has_value());
			CHECK(arrived->ReadableSockets == std::vector<size_t>{ 0 });
			const Result<SocketReadiness> stillThere = Socket::WaitAny(sockets, nullptr, std::chrono::milliseconds(0));
			REQUIRE(stillThere.has_value());
			CHECK(stillThere->ReadableSockets == std::vector<size_t>{ 0 });

			// Nothing to wait on: the timeout simply expires.
			const Result<SocketReadiness> nothing = Socket::WaitAny({}, nullptr, std::chrono::milliseconds(0));
			REQUIRE(nothing.has_value());
			CHECK(nothing->IsEmpty());
		}
	}

}
