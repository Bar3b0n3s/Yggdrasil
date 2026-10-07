#include "EnginePCH.h"
#include "Engine/Platform/Socket.h"

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Core/Assert.h"
	#include "Engine/Core/Log.h"
	#include "Engine/Platform/Private/SocketCommon.h"

	#include <arpa/inet.h>
	#include <fcntl.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <poll.h>
	#include <sys/socket.h>
	#include <unistd.h>

	#include <algorithm>
	#include <array>
	#include <cerrno>
	#include <chrono>
	#include <climits>
	#include <system_error>

// Loopback TCP on Linux and macOS (BSD sockets). Every socket is non-blocking and close-on-exec, so a child process
// never inherits a listener or a connection; each blocking call retries its operation and waits in poll(2) for the time
// left of its timeout. A write to a connection the peer closed must fail with EPIPE instead of raising SIGPIPE: Linux
// passes MSG_NOSIGNAL to every send(2), macOS sets SO_NOSIGPIPE on every connection. Connections disable Nagle's
// algorithm (TCP_NODELAY): the automation protocol sends small request and response frames whose latency would
// otherwise depend on the peer's delayed acknowledgements.

namespace Engine {

	namespace {

		// Owns a file descriptor and closes it on destruction.
		class ScopedDescriptor
		{
		public:
			ScopedDescriptor() = default;

			explicit ScopedDescriptor(int descriptor)
				: m_Descriptor(descriptor)
			{
			}

			~ScopedDescriptor()
			{
				Reset();
			}

			ScopedDescriptor(ScopedDescriptor&& other) noexcept
				: m_Descriptor(std::exchange(other.m_Descriptor, -1))
			{
			}

			ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept
			{
				if (this != &other)
				{
					Reset();
					m_Descriptor = std::exchange(other.m_Descriptor, -1);
				}
				return *this;
			}

			ScopedDescriptor(const ScopedDescriptor&) = delete;
			ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

			// Closes the descriptor. After EINTR it is released all the same (Linux) or in an unspecified state (macOS), and
			// closing it again could close a descriptor another thread has just opened, so close(2) is never retried.
			void Reset()
			{
				if (m_Descriptor < 0)
					return;
				if (close(m_Descriptor) != 0)
				{
					const int error = errno;
					if (error != EINTR)
						ENGINE_CORE_WARN("Closing socket descriptor {} failed: {}", m_Descriptor, std::generic_category().message(error));
				}
				m_Descriptor = -1;
			}

			[[nodiscard]] int Get() const { return m_Descriptor; }
			[[nodiscard]] bool IsValid() const { return m_Descriptor >= 0; }
		private:
			int m_Descriptor = -1;
		};

	}

	namespace Utils {

	#if defined(ENGINE_PLATFORM_LINUX)
		static constexpr int SendFlags = MSG_NOSIGNAL;
	#else
		static constexpr int SendFlags = 0; // macOS: SO_NOSIGPIPE is set on every connection instead
	#endif

		// An error whose message ends with the text of `error`, an errno value read right after the failing call (formatting
		// the message may allocate, which may change errno).
		template<typename... Args>
		static std::unexpected<Error> MakeSystemError(ErrorCode code, int error, std::format_string<Args...> format, Args&&... args)
		{
			return MakeError(code, "{}: {}", std::format(format, std::forward<Args>(args)...), std::generic_category().message(error));
		}

		static bool IsWouldBlock(int error)
		{
			return error == EAGAIN || error == EWOULDBLOCK;
		}

		static sockaddr_in MakeLoopbackAddress(uint16_t port)
		{
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			return address;
		}

		static Status SetOption(const ScopedDescriptor& descriptor, int level, int option, std::string_view name)
		{
			const int enable = 1;
			if (setsockopt(descriptor.Get(), level, option, &enable, sizeof(enable)) != 0)
			{
				const int error = errno;
				return MakeSystemError(ErrorCode::Io, error, "setting {} on a socket failed", name);
			}
			return {};
		}

		// Makes a new descriptor non-blocking and close-on-exec where the call that created it could not (macOS has
		// neither SOCK_NONBLOCK and SOCK_CLOEXEC nor accept4).
		static Status ConfigureDescriptor([[maybe_unused]] const ScopedDescriptor& descriptor)
		{
	#if defined(ENGINE_PLATFORM_MACOS)
			if (fcntl(descriptor.Get(), F_SETFD, FD_CLOEXEC) != 0)
			{
				const int error = errno;
				return MakeSystemError(ErrorCode::Io, error, "marking a socket close-on-exec failed");
			}
			const int flags = fcntl(descriptor.Get(), F_GETFL);
			if (flags < 0 || fcntl(descriptor.Get(), F_SETFL, flags | O_NONBLOCK) != 0)
			{
				const int error = errno;
				return MakeSystemError(ErrorCode::Io, error, "making a socket non-blocking failed");
			}
	#endif
			return {};
		}

		// The options of every connection, connected or accepted.
		static Status ConfigureConnection(const ScopedDescriptor& descriptor)
		{
			ENGINE_TRY(ConfigureDescriptor(descriptor));
	#if defined(ENGINE_PLATFORM_MACOS)
			ENGINE_TRY(SetOption(descriptor, SOL_SOCKET, SO_NOSIGPIPE, "SO_NOSIGPIPE"));
	#endif
			return SetOption(descriptor, IPPROTO_TCP, TCP_NODELAY, "TCP_NODELAY");
		}

		static Result<ScopedDescriptor> CreateTcpSocket()
		{
	#if defined(ENGINE_PLATFORM_LINUX)
			ScopedDescriptor descriptor(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP));
	#else
			ScopedDescriptor descriptor(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
	#endif
			if (!descriptor.IsValid())
			{
				const int error = errno;
				return MakeSystemError(ErrorCode::Io, error, "creating a TCP socket failed");
			}
			ENGINE_TRY(ConfigureDescriptor(descriptor));
			return descriptor;
		}

		// The local port of a bound socket.
		static Result<uint16_t> GetLocalPort(const ScopedDescriptor& descriptor)
		{
			sockaddr_in address{};
			socklen_t length = sizeof(address);
			if (getsockname(descriptor.Get(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
			{
				const int error = errno;
				return MakeSystemError(ErrorCode::Io, error, "reading the address of a socket failed");
			}
			return ntohs(address.sin_port);
		}

		// Waits until `descriptor` reports one of `events`, or an error or hang-up, and returns true; false when the deadline
		// passes first. A zero time left only checks.
		static Result<bool> WaitFor(const ScopedDescriptor& descriptor, short events, const SocketDeadline& deadline)
		{
			for (;;)
			{
				pollfd entry{};
				entry.fd = descriptor.Get();
				entry.events = events;
				const int result = poll(&entry, 1, deadline.GetRemainingMilliseconds());
				if (result > 0)
					return true;
				if (result == 0)
					return false;
				const int error = errno;
				if (error != EINTR)
					return MakeSystemError(ErrorCode::Io, error, "waiting on a socket failed");
			}
		}

	}

	struct Socket::Impl
	{
		ScopedDescriptor Descriptor;
	};

	struct SocketListener::Impl
	{
		ScopedDescriptor Descriptor;
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
		ENGINE_TRY_ASSIGN(ScopedDescriptor descriptor, Utils::CreateTcpSocket());
		const sockaddr_in address = Utils::MakeLoopbackAddress(port);
		if (connect(descriptor.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			// A non-blocking connect completes in the background, also after EINTR; its result arrives as SO_ERROR.
			const int connectError = errno;
			if (connectError != EINPROGRESS && connectError != EINTR)
				return Utils::MakeSystemError(ErrorCode::Io, connectError, "connecting to 127.0.0.1:{} failed", port);
			ENGINE_TRY_ASSIGN(const bool isConnected, Utils::WaitFor(descriptor, POLLOUT, deadline));
			if (!isConnected)
			{
				return MakeError(ErrorCode::Timeout, "connecting to 127.0.0.1:{} timed out after {} ms", port,
					deadline.GetTimeout().count());
			}

			int socketError = 0;
			socklen_t length = sizeof(socketError);
			if (getsockopt(descriptor.Get(), SOL_SOCKET, SO_ERROR, &socketError, &length) != 0)
				socketError = errno;
			if (socketError != 0)
				return Utils::MakeSystemError(ErrorCode::Io, socketError, "connecting to 127.0.0.1:{} failed", port);
		}

		// With nothing listening, a connection from an ephemeral port that happens to equal `port` connects to itself
		// (TCP simultaneous open). That is a refused connection, not a peer.
		ENGINE_TRY_ASSIGN(const uint16_t localPort, Utils::GetLocalPort(descriptor));
		if (localPort == port)
		{
			return MakeError(ErrorCode::Io, "connecting to 127.0.0.1:{} failed: nothing listens there (the socket connected to itself)",
				port);
		}

		ENGINE_TRY(Utils::ConfigureConnection(descriptor));
		Scope<Impl> impl = CreateScope<Impl>();
		impl->Descriptor = std::move(descriptor);
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
			const ssize_t count = send(m_Impl->Descriptor.Get(), data.data() + sent, data.size() - sent, Utils::SendFlags);
			if (count > 0)
			{
				sent += static_cast<size_t>(count);
				continue;
			}
			if (count < 0)
			{
				const int error = errno;
				if (error == EINTR)
					continue;
				if (!Utils::IsWouldBlock(error))
					return Utils::MakeSystemError(ErrorCode::Io, error, "sending failed after {} of {} bytes", sent, data.size());
			}

			ENGINE_TRY_ASSIGN(const bool isWritable, Utils::WaitFor(m_Impl->Descriptor, POLLOUT, deadline));
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
			const ssize_t count = recv(m_Impl->Descriptor.Get(), destination.data(), destination.size(), 0);
			if (count >= 0)
				return static_cast<size_t>(count);
			const int error = errno;
			if (error == EINTR)
				continue;
			if (!Utils::IsWouldBlock(error))
				return Utils::MakeSystemError(ErrorCode::Io, error, "receiving failed");

			ENGINE_TRY_ASSIGN(const bool isReadable, Utils::WaitFor(m_Impl->Descriptor, POLLIN, deadline));
			if (!isReadable)
				return MakeError(ErrorCode::Timeout, "nothing was received within {} ms", deadline.GetTimeout().count());
		}
	}

	void Socket::Close()
	{
		if (!IsOpen())
			return;
		if (shutdown(m_Impl->Descriptor.Get(), SHUT_RDWR) != 0)
		{
			// ENOTCONN: the peer already reset the connection, so there is nothing left to shut down.
			const int error = errno;
			if (error != ENOTCONN)
				ENGINE_CORE_WARN("Shutting down a connection failed: {}", std::generic_category().message(error));
		}
		m_Impl->Descriptor.Reset();
	}

	bool Socket::IsOpen() const
	{
		return m_Impl != nullptr && m_Impl->Descriptor.IsValid();
	}

	Result<SocketReadiness> Socket::WaitAny(std::span<const Socket* const> sockets, const SocketListener* listener,
		std::chrono::milliseconds timeout)
	{
		return WaitAny(sockets, {}, listener, timeout);
	}

	Result<SocketReadiness> Socket::WaitAny(std::span<const Socket* const> sockets, std::span<const Socket* const> writers,
		const SocketListener* listener, std::chrono::milliseconds timeout)
	{
		ENGINE_TRY(Utils::CheckWaitAnySockets(sockets));
		ENGINE_TRY(Utils::CheckWaitAnySockets(writers));

		// One entry per distinct socket: a socket in both lists waits for both events in one entry.
		std::array<pollfd, 2 * MaxSocketWaitCount + 1> entries{};
		std::array<size_t, MaxSocketWaitCount> readerEntries{};
		std::array<size_t, MaxSocketWaitCount> writerEntries{};
		size_t entryCount = 0;
		if (listener != nullptr)
		{
			ENGINE_CORE_ASSERT(listener->m_Impl != nullptr && listener->m_Impl->Descriptor.IsValid(),
				"Socket::WaitAny got a closed listener");
			if (listener->m_Impl == nullptr || !listener->m_Impl->Descriptor.IsValid())
				return MakeError(ErrorCode::Io, "cannot wait on a closed listener");
			entries[entryCount++] = pollfd{ .fd = listener->m_Impl->Descriptor.Get(), .events = POLLIN, .revents = 0 };
		}
		for (size_t index = 0; index < sockets.size(); ++index)
		{
			const Socket* socket = sockets[index];
			ENGINE_CORE_ASSERT(socket != nullptr && socket->IsOpen(), "Socket::WaitAny got a closed socket");
			if (socket == nullptr || !socket->IsOpen())
				return MakeError(ErrorCode::Io, "cannot wait on a closed socket");
			readerEntries[index] = entryCount;
			entries[entryCount++] = pollfd{ .fd = socket->m_Impl->Descriptor.Get(), .events = POLLIN, .revents = 0 };
		}
		for (size_t index = 0; index < writers.size(); ++index)
		{
			const Socket* socket = writers[index];
			ENGINE_CORE_ASSERT(socket != nullptr && socket->IsOpen(), "Socket::WaitAny got a closed socket");
			if (socket == nullptr || !socket->IsOpen())
				return MakeError(ErrorCode::Io, "cannot wait on a closed socket");
			const auto reader = std::find(sockets.begin(), sockets.end(), socket);
			if (reader != sockets.end())
			{
				writerEntries[index] = readerEntries[static_cast<size_t>(reader - sockets.begin())];
				entries[writerEntries[index]].events = static_cast<short>(entries[writerEntries[index]].events | POLLOUT);
				continue;
			}
			writerEntries[index] = entryCount;
			entries[entryCount++] = pollfd{ .fd = socket->m_Impl->Descriptor.Get(), .events = POLLOUT, .revents = 0 };
		}

		const Utils::SocketDeadline deadline(timeout);
		for (;;)
		{
			if (poll(entries.data(), static_cast<nfds_t>(entryCount), deadline.GetRemainingMilliseconds()) >= 0)
				break;
			const int error = errno;
			if (error != EINTR)
				return Utils::MakeSystemError(ErrorCode::Io, error, "waiting on sockets failed");
		}

		// A hang-up or an error counts as ready: Accept, Receive or SendAvailable then returns at once, with the end of the
		// stream or Io.
		constexpr short FailureEvents = POLLHUP | POLLERR;
		SocketReadiness readiness;
		for (size_t index = 0; index < entryCount; ++index)
		{
			if ((entries[index].revents & POLLNVAL) != 0)
				return MakeError(ErrorCode::Io, "waiting on sockets failed: descriptor {} is not open", entries[index].fd);
		}
		if (listener != nullptr)
			readiness.ListenerReady = (entries[0].revents & (POLLIN | FailureEvents)) != 0;
		for (size_t index = 0; index < sockets.size(); ++index)
		{
			if ((entries[readerEntries[index]].revents & (POLLIN | FailureEvents)) != 0)
				readiness.ReadableSockets.push_back(index);
		}
		for (size_t index = 0; index < writers.size(); ++index)
		{
			if ((entries[writerEntries[index]].revents & (POLLOUT | FailureEvents)) != 0)
				readiness.WritableSockets.push_back(index);
		}
		return readiness;
	}

	Result<size_t> Socket::SendAvailable(std::span<const std::byte> data)
	{
		if (!IsOpen())
			return MakeError(ErrorCode::Io, "cannot send on a closed socket");

		size_t sent = 0;
		while (sent < data.size())
		{
			const ssize_t count = send(m_Impl->Descriptor.Get(), data.data() + sent, data.size() - sent, Utils::SendFlags);
			if (count > 0)
			{
				sent += static_cast<size_t>(count);
				continue;
			}
			if (count < 0)
			{
				const int error = errno;
				if (error == EINTR)
					continue;
				if (!Utils::IsWouldBlock(error))
					return Utils::MakeSystemError(ErrorCode::Io, error, "sending failed after {} of {} bytes", sent, data.size());
			}
			break; // the send buffer is full: the caller waits for writability
		}
		return sent;
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

		ENGINE_TRY_ASSIGN(ScopedDescriptor descriptor, Utils::CreateTcpSocket());
		// SO_REUSEADDR only lets a new listener bind a port whose old connections linger in TIME_WAIT; on Linux and macOS
		// it never lets two sockets listen on the same address and port.
		ENGINE_TRY(Utils::SetOption(descriptor, SOL_SOCKET, SO_REUSEADDR, "SO_REUSEADDR"));

		const sockaddr_in address = Utils::MakeLoopbackAddress(port);
		if (bind(descriptor.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			const int error = errno;
			const ErrorCode code = error == EADDRINUSE ? ErrorCode::AlreadyExists : ErrorCode::Io;
			return Utils::MakeSystemError(code, error, "binding 127.0.0.1:{} failed", port);
		}
		// Two listeners may both bind with SO_REUSEADDR before either listens; the second listen then fails.
		const int queueLength = static_cast<int>(std::clamp(backlog, uint32_t{ 1 }, static_cast<uint32_t>(INT_MAX)));
		if (listen(descriptor.Get(), queueLength) != 0)
		{
			const int error = errno;
			const ErrorCode code = error == EADDRINUSE ? ErrorCode::AlreadyExists : ErrorCode::Io;
			return Utils::MakeSystemError(code, error, "listening on 127.0.0.1:{} failed", port);
		}

		ENGINE_TRY_ASSIGN(const uint16_t boundPort, Utils::GetLocalPort(descriptor));
		Scope<Impl> impl = CreateScope<Impl>();
		impl->Descriptor = std::move(descriptor);
		impl->Port = boundPort;
		return SocketListener(std::move(impl));
	}

	uint16_t SocketListener::GetPort() const
	{
		return m_Impl != nullptr && m_Impl->Descriptor.IsValid() ? m_Impl->Port : 0;
	}

	Result<Socket> SocketListener::Accept(std::chrono::milliseconds timeout)
	{
		if (m_Impl == nullptr || !m_Impl->Descriptor.IsValid())
			return MakeError(ErrorCode::Io, "cannot accept on a closed listener");

		const Utils::SocketDeadline deadline(timeout);
		for (;;)
		{
	#if defined(ENGINE_PLATFORM_LINUX)
			ScopedDescriptor descriptor(accept4(m_Impl->Descriptor.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
	#else
			ScopedDescriptor descriptor(accept(m_Impl->Descriptor.Get(), nullptr, nullptr));
	#endif
			if (descriptor.IsValid())
			{
				ENGINE_TRY(Utils::ConfigureConnection(descriptor));
				Scope<Socket::Impl> impl = CreateScope<Socket::Impl>();
				impl->Descriptor = std::move(descriptor);
				return Socket(std::move(impl));
			}

			// ECONNABORTED and EPROTO: a pending connection was reset before it was accepted; wait for the next one.
			const int error = errno;
			if (error == EINTR || error == ECONNABORTED || error == EPROTO)
				continue;
			if (!Utils::IsWouldBlock(error))
				return Utils::MakeSystemError(ErrorCode::Io, error, "accepting a connection on 127.0.0.1:{} failed", m_Impl->Port);

			ENGINE_TRY_ASSIGN(const bool isPending, Utils::WaitFor(m_Impl->Descriptor, POLLIN, deadline));
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
			m_Impl->Descriptor.Reset();
	}

}

#endif
