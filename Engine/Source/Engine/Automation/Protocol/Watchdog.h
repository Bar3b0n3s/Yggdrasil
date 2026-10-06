#pragma once

#include "Engine/Core/Base.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The automation watchdog (Architecture §13.2): an agent never hangs on a frozen editor. The main thread beats once per
// pump; when it has not beaten for the stall threshold, the I/O thread answers new requests itself with Busy (-32007),
// naming the main thread's current phase from the phase marker ("Automation:debug.stall", from M13
// "Script:Board.OnFixedUpdate", from M6 "AssetImport:Track.glb").

namespace Engine {

	// §13.2: "if the main thread has not pumped for 5 s".
	inline constexpr std::chrono::milliseconds DefaultWatchdogStallThreshold{ 5000 };

	// The phase marker and the main thread's heartbeat. Thread-safe: the main thread writes, the I/O thread reads, and no
	// call blocks for longer than copying a short string. Time is passed in by the caller (steady clock in production), so
	// tests are exact and nothing here reads a clock (CodeStyle §14). Wall-clock time is right here: the watchdog measures
	// how long the process has been unresponsive, never simulated time.
	class Watchdog
	{
	public:
		// The longest phase text kept, in bytes; longer phases are cut at a UTF-8 character boundary.
		static constexpr size_t MaxPhaseLength = 256;

		// `stallThreshold` > 0 (asserted). The heartbeat starts at `now`, so a server that has not pumped yet is not stalled.
		Watchdog(std::chrono::milliseconds stallThreshold, std::chrono::steady_clock::time_point now);
		~Watchdog();

		Watchdog(const Watchdog&) = delete;
		Watchdog& operator=(const Watchdog&) = delete;

		// Main thread: the main thread is alive at `now` (AutomationServer::Pump calls it first, and the Dispatcher between
		// steps of a long pump).
		void Heartbeat(std::chrono::steady_clock::time_point now);

		// Main thread: what the main thread is doing; empty means "Idle". The text is copied.
		void SetPhase(std::string_view phase);

		// Any thread: the current phase ("Idle" when none is set).
		[[nodiscard]] std::string GetPhase() const;

		// Any thread: how long the main thread has not beaten when that exceeds the stall threshold; nullopt while alive.
		[[nodiscard]] std::optional<std::chrono::milliseconds> GetStall(std::chrono::steady_clock::time_point now) const;

		[[nodiscard]] std::chrono::milliseconds GetStallThreshold() const { return m_StallThreshold; }
	private:
		struct State; // the heartbeat and the phase text under their lock (Watchdog.cpp)
	private:
		std::chrono::milliseconds m_StallThreshold = DefaultWatchdogStallThreshold;
		Scope<State> m_State;
	};

	// Sets a phase for the lifetime of the scope and restores the previous one afterwards (main thread). `watchdog` may be
	// null (in-process calls without a server), which makes the scope do nothing.
	class WatchdogPhaseScope
	{
	public:
		WatchdogPhaseScope(Watchdog* watchdog, std::string_view phase);
		~WatchdogPhaseScope();

		WatchdogPhaseScope(const WatchdogPhaseScope&) = delete;
		WatchdogPhaseScope& operator=(const WatchdogPhaseScope&) = delete;
	private:
		Watchdog* m_Watchdog = nullptr; // documented back-reference: outlives the scope; null for none
		std::string m_PreviousPhase;    // restored by the destructor
	};

}
