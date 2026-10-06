#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Dispatcher.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/RingBufferSink.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements request execution. This file is the allowlisted
// try/catch boundary of Architecture §4.6 item 5; the stub needs no catch yet.

namespace Engine {

	struct Dispatcher::State
	{
		const MethodRegistry* Registry = nullptr; // documented back-references
		IMethodHost* Host = nullptr;
		Watchdog* PhaseMarker = nullptr;
		DispatcherSpecification Specification{};
		MetaBuilder Meta;
		uint64_t NextOffloadSequence = 1;

		explicit State(const RingBufferSink& log)
			: Meta(log)
		{
		}
	};

	Dispatcher::Dispatcher(const MethodRegistry& registry, IMethodHost& host, const RingBufferSink& log, Watchdog* watchdog,
		DispatcherSpecification specification)
		: m_State(CreateScope<State>(log))
	{
		ENGINE_CONTRACT_STUB();
		m_State->Registry = &registry;
		m_State->Host = &host;
		m_State->PhaseMarker = watchdog;
		m_State->Specification = specification;
	}

	Dispatcher::~Dispatcher() = default;

	void Dispatcher::AddClient(ClientId /*client*/, std::string /*name*/, bool /*offloadLargeResults*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Dispatcher::RemoveClient(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Dispatcher::Enqueue(ClientId /*client*/, RpcRequest /*request*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<OutboundMessage> Dispatcher::Pump(std::chrono::microseconds /*budget*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	bool Dispatcher::HasWork() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	size_t Dispatcher::GetQueuedCount(ClientId /*client*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	size_t Dispatcher::GetPendingCount(ClientId /*client*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
