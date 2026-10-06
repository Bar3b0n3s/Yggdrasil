#include "EnginePCH.h"
#include "Engine/Platform/Private/SocketCommon.h"

#include "Engine/Core/Assert.h"

#include <climits>

namespace Engine {

	namespace Utils {

		static bool HasDuplicates(std::span<const Socket* const> sockets)
		{
			for (size_t first = 0; first < sockets.size(); ++first)
			{
				for (size_t second = first + 1; second < sockets.size(); ++second)
				{
					if (sockets[first] == sockets[second])
						return true;
				}
			}
			return false;
		}

		SocketDeadline::SocketDeadline(std::chrono::milliseconds timeout)
			: m_Start(std::chrono::steady_clock::now()), m_Timeout(timeout.count() > 0 ? timeout : std::chrono::milliseconds(0))
		{
		}

		int SocketDeadline::GetRemainingMilliseconds() const
		{
			// Truncating the elapsed time rounds the time left up.
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_Start);
			if (elapsed >= m_Timeout)
				return 0;
			const int64_t remaining = (m_Timeout - elapsed).count();
			return remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
		}

		Status CheckConnectPort(uint16_t port)
		{
			if (port == 0)
				return MakeError(ErrorCode::InvalidArgument, "cannot connect to port 0 on 127.0.0.1");
			return {};
		}

		Status CheckReceiveBuffer(std::span<const std::byte> destination)
		{
			ENGINE_CORE_ASSERT(!destination.empty(), "Socket::Receive needs room for at least one byte");
			if (destination.empty())
				return MakeError(ErrorCode::InvalidArgument, "cannot receive into an empty buffer");
			return {};
		}

		Status CheckWaitAnySockets(std::span<const Socket* const> sockets)
		{
			ENGINE_CORE_ASSERT(sockets.size() <= MaxSocketWaitCount, "Socket::WaitAny takes at most {} sockets, got {}", MaxSocketWaitCount,
				sockets.size());
			if (sockets.size() > MaxSocketWaitCount)
				return MakeError(ErrorCode::InvalidArgument, "cannot wait on {} sockets; the limit is {}", sockets.size(), MaxSocketWaitCount);
			const bool hasDuplicates = HasDuplicates(sockets);
			ENGINE_CORE_ASSERT(!hasDuplicates, "Socket::WaitAny got the same socket twice");
			if (hasDuplicates)
				return MakeError(ErrorCode::InvalidArgument, "cannot wait on the same socket twice");
			return {};
		}

	}

}
