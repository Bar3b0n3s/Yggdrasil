#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Watchdog.h"

#include "Engine/Core/Assert.h"

#include <mutex>

namespace Engine {

	namespace Utils {

		// `phase` cut at a character boundary: never in the middle of a UTF-8 sequence, so the phase stays valid text.
		static std::string_view TruncatePhase(std::string_view phase)
		{
			size_t length = phase.size();
			if (length > Watchdog::MaxPhaseLength)
			{
				length = Watchdog::MaxPhaseLength;
				while (length > 0 && (static_cast<uint8_t>(phase[length]) & 0xc0u) == 0x80u)
					--length;
			}
			return phase.substr(0, length);
		}

	}

	struct Watchdog::State
	{
		mutable std::mutex Lock; // guards the members; held only to copy them
		std::chrono::steady_clock::time_point LastHeartbeat{};
		std::string Phase{};       // the base phase; empty: "Idle"
		std::string ScriptPhase{}; // the running gameplay script entry; empty: none
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
		std::string copy(Utils::TruncatePhase(phase));
		const std::scoped_lock lock(m_State->Lock);
		m_State->Phase = std::move(copy);
	}

	std::string Watchdog::ExchangePhase(std::string_view phase)
	{
		std::string copy(Utils::TruncatePhase(phase));
		const std::scoped_lock lock(m_State->Lock);
		std::swap(m_State->Phase, copy);
		return copy;
	}

	void Watchdog::SetScriptPhase(std::string_view phase)
	{
		const std::string_view text = Utils::TruncatePhase(phase);
		const std::scoped_lock lock(m_State->Lock);
		// assign reuses the capacity, so the per-callback updates of a running session do not allocate.
		m_State->ScriptPhase.assign(text.data(), text.size());
	}

	std::string Watchdog::GetPhase() const
	{
		const std::scoped_lock lock(m_State->Lock);
		if (!m_State->ScriptPhase.empty())
			return m_State->ScriptPhase;
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

	void FormatScriptPhase(std::string& out, std::string_view script, std::string_view callback)
	{
		std::string_view stem = script.substr(script.find_last_of('/') == std::string_view::npos ? 0 : script.find_last_of('/') + 1);
		if (const size_t extension = stem.find_last_of('.'); extension != std::string_view::npos && extension != 0)
			stem = stem.substr(0, extension);
		out.assign("Script:");
		out.append(stem);
		if (!stem.empty() && !callback.empty())
			out.push_back('.');
		out.append(callback);
	}

	WatchdogPhaseScope::WatchdogPhaseScope(Watchdog* watchdog, std::string_view phase)
		: m_Watchdog(watchdog)
	{
		if (m_Watchdog != nullptr)
			m_PreviousPhase = m_Watchdog->ExchangePhase(phase);
	}

	WatchdogPhaseScope::~WatchdogPhaseScope()
	{
		if (m_Watchdog != nullptr)
			m_Watchdog->SetPhase(m_PreviousPhase);
	}

}
