#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// TCP sockets on the loopback interface, for the automation transport (Architecture §13.2: TCP bound to 127.0.0.1 only,
// port 0 for an OS-assigned port). There is deliberately no way to bind or connect to another address. The OS code
// lives in Platform/Windows/SocketWindows.cpp (Winsock 2) and Platform/Posix/SocketPosix.cpp (BSD sockets).
//
// Both classes are movable values; a moved-from object may only be destroyed or assigned to. Winsock is initialized per
// object (WSAStartup in the factory, WSACleanup in the destructor; the OS reference-counts it), so there is no
// process-level socket state. Writing to a connection the peer closed fails with Io and never raises SIGPIPE.
//
// Threads (§13.2: one I/O thread serves the listener and up to 4 clients, while requests run on the main thread):
//   - A connection is full duplex: one Send and one receiving call (Receive or Socket::WaitAny) may run at the same time
//     on two threads, and IsOpen alongside them. The I/O thread waits and receives while the main thread sends its
//     responses directly, so the I/O thread never has to be woken for outgoing data.
//   - Close, moves and destruction need exclusive access: no other call may be running on that object. Two Sends or two
//     receiving calls on one Socket never overlap. A SocketListener is used by one thread at a time.
//   - Different objects may be used from different threads freely.
//   - Socket::WaitAny waits on the listener and every connection at once (poll, WSAPoll), so one thread serves them all
//     without polling each in turn. Every blocking call takes a timeout, which lets the thread check its stop flag between
//     calls instead of being woken from another thread.

namespace Engine {

	class SocketListener;

	// The most sockets one Socket::WaitAny call accepts (asserted): the automation server's 4 clients with room to spare.
	inline constexpr size_t MaxSocketWaitCount = 64;

	// Which of the objects passed to Socket::WaitAny can proceed without waiting.
	struct SocketReadiness
	{
		// The listener has a pending connection: Accept returns at once.
		bool ListenerReady = false;
		// Indexes into WaitAny's `sockets`, ascending, of the sockets whose Receive returns at once: data arrived, the peer
		// closed the connection (Receive returns 0) or the connection failed (Receive reports Io).
		std::vector<size_t> ReadableSockets{};

		// Nothing is ready (WaitAny's timeout expired).
		[[nodiscard]] bool IsEmpty() const { return !ListenerReady && ReadableSockets.empty(); }
	};

	// A connected TCP stream on 127.0.0.1.
	class Socket
	{
	public:
		// Connects to 127.0.0.1:`port` within `timeout`. Errors: InvalidArgument for port 0; Timeout; Io when the
		// connection is refused or fails (the message carries the OS error).
		[[nodiscard]] static Result<Socket> Connect(uint16_t port, std::chrono::milliseconds timeout);

		~Socket();

		Socket(Socket&& other) noexcept;
		Socket& operator=(Socket&& other) noexcept;
		Socket(const Socket&) = delete;
		Socket& operator=(const Socket&) = delete;

		// Sends every byte of `data`, waiting as needed, within `timeout` overall. Errors: Timeout (part of the data may have
		// been sent; the connection should then be closed); Io when the connection fails or the peer closed it.
		[[nodiscard]] Status Send(std::span<const std::byte> data, std::chrono::milliseconds timeout);

		// Waits up to `timeout` for data and receives up to destination.size() bytes (> 0, asserted). Returns the number of
		// bytes received, at least 1, or 0 when the peer closed the connection in an orderly way. Errors: Timeout when
		// nothing arrived; Io when the connection fails.
		[[nodiscard]] Result<size_t> Receive(std::span<std::byte> destination, std::chrono::milliseconds timeout);

		// Shuts both directions down and closes the socket; the peer's Receive then returns 0. Idempotent; the destructor
		// does it too.
		void Close();

		[[nodiscard]] bool IsOpen() const;

		// Waits up to `timeout` until `listener` (null: none) has a pending connection or one of `sockets` can be received
		// from without waiting, and reports everything ready at that moment. It returns as soon as anything is ready; after
		// the timeout it returns an empty readiness, which is not an error. A zero timeout only checks. Every object must
		// be open and `sockets` may hold at most MaxSocketWaitCount entries, without duplicates (asserted). For each socket
		// the call counts as its receiving call (see the threading rules above). Errors: Io (the message carries the OS
		// error).
		[[nodiscard]] static Result<SocketReadiness> WaitAny(std::span<const Socket* const> sockets, const SocketListener* listener,
			std::chrono::milliseconds timeout);
	private:
		struct Impl;

		explicit Socket(Scope<Impl> impl);
	private:
		Scope<Impl> m_Impl;
	private:
		friend class SocketListener;
	};

	// A listening TCP socket on 127.0.0.1.
	class SocketListener
	{
	public:
		// Binds 127.0.0.1:`port`, or an OS-assigned port when `port` is 0 (GetPort tells which), and listens with a backlog
		// of `backlog` pending connections (>= 1, asserted). The address cannot be reused while another socket is bound to
		// it. Errors: AlreadyExists when the port is in use; Io.
		[[nodiscard]] static Result<SocketListener> Listen(uint16_t port, uint32_t backlog = 4);

		~SocketListener();

		SocketListener(SocketListener&& other) noexcept;
		SocketListener& operator=(SocketListener&& other) noexcept;
		SocketListener(const SocketListener&) = delete;
		SocketListener& operator=(const SocketListener&) = delete;

		// The bound port (the OS-assigned one when Listen got 0).
		[[nodiscard]] uint16_t GetPort() const;

		// Waits up to `timeout` for a connection and accepts it. Errors: Timeout when none arrived; Io.
		[[nodiscard]] Result<Socket> Accept(std::chrono::milliseconds timeout);

		// Stops listening and closes the socket; pending connections are refused. Idempotent; the destructor does it too.
		void Close();
	private:
		struct Impl;

		explicit SocketListener(Scope<Impl> impl);
	private:
		Scope<Impl> m_Impl;
	private:
		friend class Socket; // Socket::WaitAny waits on the listening socket
	};

}
