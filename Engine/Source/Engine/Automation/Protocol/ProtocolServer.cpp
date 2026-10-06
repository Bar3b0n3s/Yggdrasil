#include "EnginePCH.h"
#include "Engine/Automation/Protocol/ProtocolServer.h"

#include "Engine/Automation/Protocol/Watchdog.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the transport. Start fails fast, so the stub never
// binds a socket or starts a thread.

namespace Engine {

	struct ProtocolServer::State
	{
		ProtocolServerSpecification Specification{};
		Watchdog* PhaseMarker = nullptr; // documented back-reference: outlives the server
	};

	ProtocolServer::ProtocolServer(ConstructionKey /*key*/, const ProtocolServerSpecification& specification, Watchdog& watchdog)
		: m_State(CreateScope<State>())
	{
		m_State->Specification = specification;
		m_State->PhaseMarker = &watchdog;
	}

	Result<Scope<ProtocolServer>> ProtocolServer::Start(const ProtocolServerSpecification& /*specification*/, Watchdog& /*watchdog*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProtocolServer::Start is an M4 contract stub");
	}

	ProtocolServer::~ProtocolServer() = default;

	void ProtocolServer::Stop()
	{
		ENGINE_CONTRACT_STUB();
	}

	uint16_t ProtocolServer::GetPort() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::vector<ClientEvent> ProtocolServer::TakeClientEvents()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<InboundRequest> ProtocolServer::TakeRequests()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status ProtocolServer::Send(ClientId /*client*/, const Json& /*message*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProtocolServer::Send is an M4 contract stub");
	}

	void ProtocolServer::Disconnect(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t ProtocolServer::GetClientCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
