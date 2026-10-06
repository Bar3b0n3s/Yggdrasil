#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Watchdog.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the watchdog and its phase marker.

namespace Engine {

	struct Watchdog::State
	{
		mutable std::mutex Lock;
		std::chrono::steady_clock::time_point LastHeartbeat{};
		std::string Phase{};
	};

	Watchdog::Watchdog(std::chrono::milliseconds stallThreshold, std::chrono::steady_clock::time_point now)
		: m_StallThreshold(stallThreshold), m_State(CreateScope<State>())
	{
		ENGINE_CONTRACT_STUB();
		m_State->LastHeartbeat = now;
	}

	Watchdog::~Watchdog() = default;

	void Watchdog::Heartbeat(std::chrono::steady_clock::time_point /*now*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Watchdog::SetPhase(std::string_view /*phase*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::string Watchdog::GetPhase() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<std::chrono::milliseconds> Watchdog::GetStall(std::chrono::steady_clock::time_point /*now*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	WatchdogPhaseScope::WatchdogPhaseScope(Watchdog* watchdog, std::string_view /*phase*/)
		: m_Watchdog(watchdog)
	{
		ENGINE_CONTRACT_STUB();
		if (m_Watchdog != nullptr)
			m_PreviousPhase = m_Watchdog->GetPhase();
	}

	WatchdogPhaseScope::~WatchdogPhaseScope()
	{
		ENGINE_CONTRACT_STUB();
	}

}
