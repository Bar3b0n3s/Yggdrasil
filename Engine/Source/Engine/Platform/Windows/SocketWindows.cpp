#include "EnginePCH.h"
#include "Engine/Platform/Socket.h"

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Assert.h"
	#include "Engine/Core/Log.h"
	#include "Engine/Platform/Private/SocketCommon.h"

	#include <winsock2.h>
	#include <ws2tcpip.h>

	#include <chrono>
	#include <climits>
	#include <system_error>
	#include <thread>

// Loopback TCP on Windows (Winsock 2). Every Socket and SocketListener holds its own Winsock reference (WSAStartup when
// it is created, WSACleanup when it is destroyed), so there is no process-level socket state. Sockets are non-blocking
// and not inheritable, so a child process never inherits a listener or a connection; each blocking call retries its
// operation and waits in WSAPoll for the time left of its timeout. Connect waits with select instead: WSAPoll does not
// report a failed connection attempt before Windows 10 version 2004. Listeners take their address exclusively
// (SO_EXCLUSIVEADDRUSE), so no other socket can bind the automation port while the engine listens on it. Connections
// disable Nagle's algorithm (TCP_NODELAY): the automation protocol sends small request and response frames whose latency
// would otherwise depend on the peer's delayed acknowledgements.
//
// <winsock2.h> defines min and max as macros (NOMINMAX is not set, Dependencies.lua), so this file uses neither
// std::min nor std::max.

namespace Engine {

	namespace {

		// One reference on Winsock, released on destruction.
		class WinsockReference
		{
		public:
			WinsockReference() = default;

			~WinsockReference()
			{
				if (m_IsStarted && WSACleanup() != 0)
					ENGINE_CORE_WARN("WSACleanup failed: {}", std::system_category().message(WSAGetLastError()));
			}

			WinsockReference(const WinsockReference&) = delete;
			WinsockReference& operator=(const WinsockReference&) = delete;
			WinsockReference(WinsockReference&&) = delete;
			WinsockReference& operator=(WinsockReference&&) = delete;

			[[nodiscard]] Status Start()
			{
				ENGINE_CORE_ASSERT(!m_IsStarted, "WinsockReference::Start called twice");
				WSADATA data{};
				const int result = WSAStartup(MAKEWORD(2, 2), &data);
				if (result != 0)
					return MakeError(ErrorCode::Io, "starting Winsock 2.2 failed: {}", std::system_category().message(result));
				m_IsStarted = true;
				return {};
			}
		private:
			bool m_IsStarted = false;
		};

		// Owns a socket handle and closes it on destruction.
		class ScopedSocket
		{
		public:
			ScopedSocket() = default;

			~ScopedSocket()
			{
				Reset();
			}

			ScopedSocket(const ScopedSocket&) = delete;
			ScopedSocket& operator=(const ScopedSocket&) = delete;
			ScopedSocket(ScopedSocket&&) = delete;
			ScopedSocket& operator=(ScopedSocket&&) = delete;

			void Reset(SOCKET handle = INVALID_SOCKET)
			{
				if (m_Handle != INVALID_SOCKET && closesocket(m_Handle) != 0)
					ENGINE_CORE_WARN("Closing a socket failed: {}", std::system_category().message(WSAGetLastError()));
				m_Handle = handle;
			}

			[[nodiscard]] SOCKET Get() const { return m_Handle; }
			[[nodiscard]] bool IsValid() const { return m_Handle != INVALID_SOCKET; }
		private:
			SOCKET m_Handle = INVALID_SOCKET;
		};

	}

	namespace Utils {

		// An error whose message ends with the text of `error`, a Winsock error code read with WSAGetLastError right after
		// the failing call.
		template<typename... Args>
		static std::unexpected<Error> MakeSystemError(ErrorCode code, int error, std::format_string<Args...> format, Args&&... args)
		{
			return MakeError(code, "{}: {}", std::format(format, std::forward<Args>(args)...), std::system_category().message(error));
		}

		static sockaddr_in MakeLoopbackAddress(uint16_t port)
		{
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			return address;
		}

		static Status SetOption(const ScopedSocket& socket, int level, int option, std::string_view name)
		{
			const BOOL enable = TRUE;
			if (setsockopt(socket.Get(), level, option, reinterpret_cast<const char*>(&enable), sizeof(enable)) != 0)
			{
				const int error = WSAGetLastError();
				return MakeSystemError(ErrorCode::Io, error, "setting {} on a socket failed", name);
			}
			return {};
		}

		static Status MakeNonBlocking(const ScopedSocket& socket)
		{
			u_long isNonBlocking = 1;
			if (ioctlsocket(socket.Get(), FIONBIO, &isNonBlocking) != 0)
			{
				const int error = WSAGetLastError();
				return MakeSystemError(ErrorCode::Io, error, "making a socket non-blocking failed");
			}
			return {};
		}

		// The options of every connection, connected or accepted.
		static Status ConfigureConnection(const ScopedSocket& socket)
		{
			// An accepted socket copies the listener's properties, but whether that includes the inheritance flag is not
			// documented, so it is cleared explicitly.
			if (!SetHandleInformation(reinterpret_cast<HANDLE>(socket.Get()), HANDLE_FLAG_INHERIT, 0))
			{
				const int error = static_cast<int>(GetLastError());
				return MakeSystemError(ErrorCode::Io, error, "making a socket non-inheritable failed");
			}
			ENGINE_TRY(MakeNonBlocking(socket));
			return SetOption(socket, IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY");
		}

		static Status CreateTcpSocket(ScopedSocket& socket)
		{
			socket.Reset(WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
			if (!socket.IsValid())
			{
				const int error = WSAGetLastError();
				return MakeSystemError(ErrorCode::Io, error, "creating a TCP socket failed");
			}
			return MakeNonBlocking(socket);
		}

		// The local port of a bound socket.
		static Result<uint16_t> GetLocalPort(const ScopedSocket& socket)
		{
			sockaddr_in address{};
			int length = sizeof(address);
			if (getsockname(socket.Get(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
			{
				const int error = WSAGetLastError();
				return MakeSystemError(ErrorCode::Io, error, "reading the address of a socket failed");
			}
			return ntohs(address.sin_port);
		}

		// Waits until `socket` reports one of `events`, or an error or hang-up, and returns true; false when the deadline
		// passes first. A zero time left only checks.
		static Result<bool> WaitFor(const ScopedSocket& socket, SHORT events, const SocketDeadline& deadline)
		{
			WSAPOLLFD entry{};
			entry.fd = socket.Get();
			entry.events = events;
			const int result = WSAPoll(&entry, 1, deadline.GetRemainingMilliseconds());
			if (result == SOCKET_ERROR)
			{
				const int error = WSAGetLastError();
				return MakeSystemError(ErrorCode::Io, error, "waiting on a socket failed");
			}
			return result > 0;
		}

		// send and recv take an int length.
		static int ClampLength(size_t length)
		{
			return length > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(length);
		}

	}

	struct Socket::Impl
	{
		WinsockReference Winsock; // declared first, so it is released after the socket is closed
		ScopedSocket Handle;
	};

	struct SocketListener::Impl
	{
		WinsockReference Winsock; // declared first, so it is released after the socket is closed
		ScopedSocket Handle;
		uint16_t Port = 0;
	};

	Socket::Socket(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	Socket::~Socket()
	{
		Close();
	}

	Socket::Socket(Socket&& other) noexcept = default;

	Socket& Socket::operator=(Socket&& other) noexcept
	{
		if (this != &other)
		{
			Close();
			m_Impl = std::move(other.m_Impl);
		}
		return *this;
	}

	Result<Socket> Socket::Connect(uint16_t port, std::chrono::milliseconds timeout)
	{
		ENGINE_TRY(Utils::CheckConnectPort(port));

		const Utils::SocketDeadline deadline(timeout);
		Scope<Impl> impl = CreateScope<Impl>();
		ENGINE_TRY(impl->Winsock.Start());
		ENGINE_TRY(Utils::CreateTcpSocket(impl->Handle));

		const sockaddr_in address = Utils::MakeLoopbackAddress(port);
		if (connect(impl->Handle.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			const int connectError = WSAGetLastError();
			if (connectError != WSAEWOULDBLOCK)
				return Utils::MakeSystemError(ErrorCode::Io, connectError, "connecting to 127.0.0.1:{} failed", port);

			// Writable: connected. In the exception set: the attempt failed, and SO_ERROR says why.
			fd_set writable{};
			fd_set failed{};
			FD_ZERO(&writable);
			FD_ZERO(&failed);
			FD_SET(impl->Handle.Get(), &writable);
			FD_SET(impl->Handle.Get(), &failed);
			const int remaining = deadline.GetRemainingMilliseconds();
			const timeval wait{ .tv_sec = remaining / 1000, .tv_usec = (remaining % 1000) * 1000 };
			const int result = select(0, nullptr, &writable, &failed, &wait);
			if (result == SOCKET_ERROR)
			{
				const int error = WSAGetLastError();
				return Utils::MakeSystemError(ErrorCode::Io, error, "connecting to 127.0.0.1:{} failed", port);
			}
			if (result == 0)
			{
				return MakeError(ErrorCode::Timeout, "connecting to 127.0.0.1:{} timed out after {} ms", port,
					deadline.GetTimeout().count());
			}
			if (FD_ISSET(impl->Handle.Get(), &failed))
			{
				int socketError = 0;
				int length = sizeof(socketError);
				if (getsockopt(impl->Handle.Get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &length) != 0)
					socketError = WSAGetLastError();
				return Utils::MakeSystemError(ErrorCode::Io, socketError, "connecting to 127.0.0.1:{} failed", port);
			}
		}

		// With nothing listening, a connection from an ephemeral port that happens to equal `port` would connect to itself
		// (TCP simultaneous open). That is a refused connection, not a peer.
		ENGINE_TRY_ASSIGN(const uint16_t localPort, Utils::GetLocalPort(impl->Handle));
		if (localPort == port)
		{
			return MakeError(ErrorCode::Io, "connecting to 127.0.0.1:{} failed: nothing listens there (the socket connected to itself)",
				port);
		}

		ENGINE_TRY(Utils::ConfigureConnection(impl->Handle));
		return Socket(std::move(impl));
	}

	Status Socket::Send(std::span<const std::byte> data, std::chrono::milliseconds timeout)
	{
		if (!IsOpen())
			return MakeError(ErrorCode::Io, "cannot send on a closed socket");

		const Utils::SocketDeadline deadline(timeout);
		size_t sent = 0;
		while (sent < data.size())
		{
			const int count = send(m_Impl->Handle.Get(), reinterpret_cast<const char*>(data.data() + sent),
				Utils::ClampLength(data.size() - sent), 0);
			if (count > 0)
			{
				sent += static_cast<size_t>(count);
				continue;
			}
			if (count == SOCKET_ERROR)
			{
				const int error = WSAGetLastError();
				if (error != WSAEWOULDBLOCK)
					return Utils::MakeSystemError(ErrorCode::Io, error, "sending failed after {} of {} bytes", sent, data.size());
			}

			ENGINE_TRY_ASSIGN(const bool isWritable, Utils::WaitFor(m_Impl->Handle, POLLWRNORM, deadline));
			if (!isWritable)
			{
				return MakeError(ErrorCode::Timeout, "sending timed out after {} ms with {} of {} bytes sent",
					deadline.GetTimeout().count(), sent, data.size());
			}
		}
		return {};
	}

	Result<size_t> Socket::Receive(std::span<std::byte> destination, std::chrono::milliseconds timeout)
	{
		ENGINE_TRY(Utils::CheckReceiveBuffer(destination));
		if (!IsOpen())
			return MakeError(ErrorCode::Io, "cannot receive on a closed socket");

		const Utils::SocketDeadline deadline(timeout);
		for (;;)
		{
			const int count = recv(m_Impl->Handle.Get(), reinterpret_cast<char*>(destination.data()),
				Utils::ClampLength(destination.size()), 0);
			if (count >= 0)
				return static_cast<size_t>(count);
			const int error = WSAGetLastError();
			if (error != WSAEWOULDBLOCK)
				return Utils::MakeSystemError(ErrorCode::Io, error, "receiving failed");

			ENGINE_TRY_ASSIGN(const bool isReadable, Utils::WaitFor(m_Impl->Handle, POLLRDNORM, deadline));
			if (!isReadable)
				return MakeError(ErrorCode::Timeout, "nothing was received within {} ms", deadline.GetTimeout().count());
		}
	}

	void Socket::Close()
	{
		if (!IsOpen())
			return;
		if (shutdown(m_Impl->Handle.Get(), SD_BOTH) != 0)
		{
			// WSAENOTCONN: the peer already reset the connection, so there is nothing left to shut down.
			const int error = WSAGetLastError();
			if (error != WSAENOTCONN)
				ENGINE_CORE_WARN("Shutting down a connection failed: {}", std::system_category().message(error));
		}
		m_Impl->Handle.Reset();
	}

	bool Socket::IsOpen() const
	{
		return m_Impl != nullptr && m_Impl->Handle.IsValid();
	}

	Result<SocketReadiness> Socket::WaitAny(std::span<const Socket* const> sockets, const SocketListener* listener,
		std::chrono::milliseconds timeout)
	{
		ENGINE_TRY(Utils::CheckWaitAnySockets(sockets));

		std::array<WSAPOLLFD, MaxSocketWaitCount + 1> entries{};
		size_t entryCount = 0;
		if (listener != nullptr)
		{
			ENGINE_CORE_ASSERT(listener->m_Impl != nullptr && listener->m_Impl->Handle.IsValid(), "Socket::WaitAny got a closed listener");
			if (listener->m_Impl == nullptr || !listener->m_Impl->Handle.IsValid())
				return MakeError(ErrorCode::Io, "cannot wait on a closed listener");
			entries[entryCount].fd = listener->m_Impl->Handle.Get();
			entries[entryCount].events = POLLRDNORM;
			++entryCount;
		}
		const size_t firstSocket = entryCount;
		for (const Socket* socket : sockets)
		{
			ENGINE_CORE_ASSERT(socket != nullptr && socket->IsOpen(), "Socket::WaitAny got a closed socket");
			if (socket == nullptr || !socket->IsOpen())
				return MakeError(ErrorCode::Io, "cannot wait on a closed socket");
			entries[entryCount].fd = socket->m_Impl->Handle.Get();
			entries[entryCount].events = POLLRDNORM;
			++entryCount;
		}

		SocketReadiness readiness;
		if (entryCount == 0)
		{
			// WSAPoll rejects an empty set; with nothing to wait on, the timeout simply expires.
			std::this_thread::sleep_for(timeout.count() > 0 ? timeout : std::chrono::milliseconds(0));
			return readiness;
		}

		const Utils::SocketDeadline deadline(timeout);
		if (WSAPoll(entries.data(), static_cast<ULONG>(entryCount), deadline.GetRemainingMilliseconds()) == SOCKET_ERROR)
		{
			const int error = WSAGetLastError();
			return Utils::MakeSystemError(ErrorCode::Io, error, "waiting on sockets failed");
		}

		// A hang-up or an error counts as ready: Accept or Receive then returns at once, with the end of the stream or Io.
		constexpr SHORT ReadyEvents = POLLRDNORM | POLLHUP | POLLERR;
		for (size_t index = 0; index < entryCount; ++index)
		{
			if ((entries[index].revents & POLLNVAL) != 0)
				return MakeError(ErrorCode::Io, "waiting on sockets failed: socket {} is not open", entries[index].fd);
		}
		if (listener != nullptr)
			readiness.ListenerReady = (entries[0].revents & ReadyEvents) != 0;
		for (size_t index = firstSocket; index < entryCount; ++index)
		{
			if ((entries[index].revents & ReadyEvents) != 0)
				readiness.ReadableSockets.push_back(index - firstSocket);
		}
		return readiness;
	}

	Result<size_t> Socket::SendAvailable(std::span<const std::byte> /*data*/)
	{
		// M4 contract stub (Roadmap rule 3): stream B (protocol, transport) implements non-blocking sends.
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Socket::SendAvailable is an M4 contract stub");
	}

	Result<SocketReadiness> Socket::WaitAny(std::span<const Socket* const> /*sockets*/, std::span<const Socket* const> /*writers*/,
		const SocketListener* /*listener*/, std::chrono::milliseconds /*timeout*/)
	{
		// M4 contract stub (Roadmap rule 3): stream B (protocol, transport) implements waiting for writability.
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Socket::WaitAny with writers is an M4 contract stub");
	}

	SocketListener::SocketListener(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	SocketListener::~SocketListener() = default;

	SocketListener::SocketListener(SocketListener&& other) noexcept = default;

	SocketListener& SocketListener::operator=(SocketListener&& other) noexcept = default;

	Result<SocketListener> SocketListener::Listen(uint16_t port, uint32_t backlog)
	{
		ENGINE_CORE_ASSERT(backlog >= 1, "SocketListener::Listen needs a backlog of at least 1");

		Scope<Impl> impl = CreateScope<Impl>();
		ENGINE_TRY(impl->Winsock.Start());
		ENGINE_TRY(Utils::CreateTcpSocket(impl->Handle));
		ENGINE_TRY(Utils::SetOption(impl->Handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, "SO_EXCLUSIVEADDRUSE"));

		const sockaddr_in address = Utils::MakeLoopbackAddress(port);
		if (bind(impl->Handle.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			const int error = WSAGetLastError();
			const ErrorCode code = error == WSAEADDRINUSE ? ErrorCode::AlreadyExists : ErrorCode::Io;
			return Utils::MakeSystemError(code, error, "binding 127.0.0.1:{} failed", port);
		}
		const int queueLength = static_cast<int>(std::clamp(backlog, uint32_t{ 1 }, static_cast<uint32_t>(INT_MAX)));
		if (listen(impl->Handle.Get(), queueLength) != 0)
		{
			const int error = WSAGetLastError();
			const ErrorCode code = error == WSAEADDRINUSE ? ErrorCode::AlreadyExists : ErrorCode::Io;
			return Utils::MakeSystemError(code, error, "listening on 127.0.0.1:{} failed", port);
		}

		ENGINE_TRY_ASSIGN(impl->Port, Utils::GetLocalPort(impl->Handle));
		return SocketListener(std::move(impl));
	}

	uint16_t SocketListener::GetPort() const
	{
		return m_Impl != nullptr && m_Impl->Handle.IsValid() ? m_Impl->Port : 0;
	}

	Result<Socket> SocketListener::Accept(std::chrono::milliseconds timeout)
	{
		if (m_Impl == nullptr || !m_Impl->Handle.IsValid())
			return MakeError(ErrorCode::Io, "cannot accept on a closed listener");

		const Utils::SocketDeadline deadline(timeout);
		Scope<Socket::Impl> impl = CreateScope<Socket::Impl>();
		ENGINE_TRY(impl->Winsock.Start());
		for (;;)
		{
			impl->Handle.Reset(accept(m_Impl->Handle.Get(), nullptr, nullptr));
			if (impl->Handle.IsValid())
			{
				ENGINE_TRY(Utils::ConfigureConnection(impl->Handle));
				return Socket(std::move(impl));
			}

			// WSAECONNRESET: a pending connection was reset before it was accepted; wait for the next one.
			const int error = WSAGetLastError();
			if (error == WSAECONNRESET)
				continue;
			if (error != WSAEWOULDBLOCK)
				return Utils::MakeSystemError(ErrorCode::Io, error, "accepting a connection on 127.0.0.1:{} failed", m_Impl->Port);

			ENGINE_TRY_ASSIGN(const bool isPending, Utils::WaitFor(m_Impl->Handle, POLLRDNORM, deadline));
			if (!isPending)
			{
				return MakeError(ErrorCode::Timeout, "no connection arrived on 127.0.0.1:{} within {} ms", m_Impl->Port,
					deadline.GetTimeout().count());
			}
		}
	}

	void SocketListener::Close()
	{
		if (m_Impl != nullptr)
			m_Impl->Handle.Reset();
	}

}

#endif
