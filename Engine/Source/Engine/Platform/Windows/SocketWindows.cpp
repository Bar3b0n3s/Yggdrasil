#include "EnginePCH.h"
#include "Engine/Platform/Socket.h"

// M2 contract stub (Roadmap rule 3): stream C (file watcher and socket) implements the loopback sockets on Windows
// (Winsock 2). Until then Connect and Listen fail with Unsupported, so no Socket or SocketListener exists.

#if defined(ENGINE_PLATFORM_WINDOWS)

namespace Engine {

	struct Socket::Impl
	{
	};

	Socket::Socket(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	Socket::~Socket() = default;

	Socket::Socket(Socket&& other) noexcept = default;

	Socket& Socket::operator=(Socket&& other) noexcept = default;

	Result<Socket> Socket::Connect(uint16_t /*port*/, std::chrono::milliseconds /*timeout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Socket::Connect is not implemented yet");
	}

	Status Socket::Send(std::span<const std::byte> /*data*/, std::chrono::milliseconds /*timeout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Socket::Send is not implemented yet");
	}

	Result<size_t> Socket::Receive(std::span<std::byte> /*destination*/, std::chrono::milliseconds /*timeout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Socket::Receive is not implemented yet");
	}

	void Socket::Close()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool Socket::IsOpen() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Result<SocketReadiness> Socket::WaitAny(std::span<const Socket* const> /*sockets*/, const SocketListener* /*listener*/,
		std::chrono::milliseconds /*timeout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Socket::WaitAny is not implemented yet");
	}

	struct SocketListener::Impl
	{
	};

	SocketListener::SocketListener(Scope<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	SocketListener::~SocketListener() = default;

	SocketListener::SocketListener(SocketListener&& other) noexcept = default;

	SocketListener& SocketListener::operator=(SocketListener&& other) noexcept = default;

	Result<SocketListener> SocketListener::Listen(uint16_t /*port*/, uint32_t /*backlog*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SocketListener::Listen is not implemented yet");
	}

	uint16_t SocketListener::GetPort() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Result<Socket> SocketListener::Accept(std::chrono::milliseconds /*timeout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SocketListener::Accept is not implemented yet");
	}

	void SocketListener::Close()
	{
		ENGINE_CONTRACT_STUB();
	}

}

#endif
