#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/Socket.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

// The host-independent half of Socket, shared by Platform/Windows/SocketWindows.cpp and Platform/Posix/SocketPosix.cpp:
// the timeout arithmetic of every blocking call and the argument checks both hosts make, so that a fix to either lands
// on every host.

namespace Engine {

	namespace Utils {

		// The time left of a timeout that started at construction, in whole milliseconds rounded up (so a wait never ends
		// early and spins) and clamped to what poll(2) and WSAPoll accept. A negative timeout counts as zero.
		class SocketDeadline
		{
		public:
			explicit SocketDeadline(std::chrono::milliseconds timeout);

			[[nodiscard]] int GetRemainingMilliseconds() const;

			[[nodiscard]] std::chrono::milliseconds GetTimeout() const { return m_Timeout; }
		private:
			std::chrono::steady_clock::time_point m_Start{};
			std::chrono::milliseconds m_Timeout{ 0 };
		};

		// Socket::Connect's argument: InvalidArgument for port 0.
		[[nodiscard]] Status CheckConnectPort(uint16_t port);

		// Socket::Receive's argument: InvalidArgument for an empty buffer, which is also asserted.
		[[nodiscard]] Status CheckReceiveBuffer(std::span<const std::byte> destination);

		// Socket::WaitAny's sockets: InvalidArgument for more than MaxSocketWaitCount of them or for the same socket twice,
		// both also asserted. Whether each one is open is the OS half's check.
		[[nodiscard]] Status CheckWaitAnySockets(std::span<const Socket* const> sockets);

	}

}
