#include "EnginePCH.h"
#include "Engine/Automation/Protocol/MetaBuilder.h"

#include "Engine/Core/RingBufferSink.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the _meta builder.

namespace Engine {

	struct MetaBuilder::State
	{
		const RingBufferSink* Log = nullptr; // documented back-reference: outlives the builder
	};

	MetaBuilder::MetaBuilder(const RingBufferSink& log)
		: m_State(CreateScope<State>())
	{
		ENGINE_CONTRACT_STUB();
		m_State->Log = &log;
	}

	MetaBuilder::~MetaBuilder() = default;

	void MetaBuilder::AddClient(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void MetaBuilder::RemoveClient(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Json MetaBuilder::Build(ClientId /*client*/, const MetaState& /*state*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

}
