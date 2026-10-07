#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Watchdog.h"

#include "Engine/Core/Assert.h"

#include <mutex>

namespace Engine {

	struct Watchdog::State
	{
		mutable std::mutex Lock; // guards both members; held only to copy them
		std::chrono::steady_clock::time_point LastHeartbeat{};
		std::string Phase{}; // empty: "Idle"
	};

	Watchdog::Watchdog(std::chrono::milliseconds stallThreshold, std::chrono::steady_clock::time_point now)
		: m_StallThreshold(stallThreshold), m_State(CreateScope<State>())
	{
		ENGINE_CORE_ASSERT(stallThreshold.count() > 0, "The watchdog's stall threshold must be positive");
		m_State->LastHeartbeat = now;
	}

	Watchdog::~Watchdog() = default;

	void Watchdog::Heartbeat(std::chrono::steady_clock::time_point now)
	{
		const std::scoped_lock lock(m_State->Lock);
		m_State->LastHeartbeat = now;
	}

	void Watchdog::SetPhase(std::string_view phase)
	{
		// Cut at a character boundary: never in the middle of a UTF-8 sequence, so the phase stays valid text.
		size_t length = phase.size();
		if (length > MaxPhaseLength)
		{
			length = MaxPhaseLength;
			while (length > 0 && (static_cast<uint8_t>(phase[length]) & 0xc0u) == 0x80u)
				--length;
		}
		std::string copy(phase.substr(0, length));
		const std::scoped_lock lock(m_State->Lock);
		m_State->Phase = std::move(copy);
	}

	std::string Watchdog::GetPhase() const
	{
		const std::scoped_lock lock(m_State->Lock);
		return m_State->Phase.empty() ? std::string("Idle") : m_State->Phase;
	}

	std::optional<std::chrono::milliseconds> Watchdog::GetStall(std::chrono::steady_clock::time_point now) const
	{
		std::chrono::steady_clock::time_point last;
		{
			const std::scoped_lock lock(m_State->Lock);
			last = m_State->LastHeartbeat;
		}
		if (now <= last)
			return std::nullopt;
		const auto stalled = std::chrono::duration_cast<std::chrono::milliseconds>(now - last);
		if (stalled <= m_StallThreshold)
			return std::nullopt;
		return stalled;
	}

	WatchdogPhaseScope::WatchdogPhaseScope(Watchdog* watchdog, std::string_view phase)
		: m_Watchdog(watchdog)
	{
		if (m_Watchdog == nullptr)
			return;
		m_PreviousPhase = m_Watchdog->GetPhase();
		m_Watchdog->SetPhase(phase);
	}

	WatchdogPhaseScope::~WatchdogPhaseScope()
	{
		if (m_Watchdog != nullptr)
			m_Watchdog->SetPhase(m_PreviousPhase == "Idle" ? std::string_view() : std::string_view(m_PreviousPhase));
	}

}
