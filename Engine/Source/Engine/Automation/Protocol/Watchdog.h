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
	//
	// Two phases are kept. The base phase belongs to the automation layers (the pump, a request, a pending operation:
	// SetPhase and WatchdogPhaseScope). The script phase (M13) names the gameplay script entry the main thread is running
	// ("Script:<Script>.<Callback>", FormatScriptPhase); hosts set it from their play sessions around every protected script
	// entry, wherever the session is advanced from (the editor's frame hooks, a play.step, a test run). While it is set it is
	// the reported phase; clearing it reveals the base phase again, so neither owner has to save or restore the other's.
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

		// Main thread: what the automation layers are doing (the base phase); empty means "Idle". The text is copied.
		void SetPhase(std::string_view phase);
		// Main thread: sets the base phase and returns the previous one ("" for Idle), for WatchdogPhaseScope.
		[[nodiscard]] std::string ExchangePhase(std::string_view phase);
		// Main thread: the running gameplay script entry (see the class comment); empty clears it. Called on every protected
		// script entry and exit, so it reuses its storage and never allocates once the text fits.
		void SetScriptPhase(std::string_view phase);

		// Any thread: the current phase: the script phase while one is set, else the base phase ("Idle" when none is set).
		[[nodiscard]] std::string GetPhase() const;

		// Any thread: how long the main thread has not beaten when that exceeds the stall threshold; nullopt while alive.
		[[nodiscard]] std::optional<std::chrono::milliseconds> GetStall(std::chrono::steady_clock::time_point now) const;

		[[nodiscard]] std::chrono::milliseconds GetStallThreshold() const { return m_StallThreshold; }
	private:
		struct State; // the heartbeat and the phase texts under their lock (Watchdog.cpp)
	private:
		std::chrono::milliseconds m_StallThreshold = DefaultWatchdogStallThreshold;
		Scope<State> m_State;
	};

	// Writes the script phase of a gameplay entry into `out` (reusing its storage): "Script:<stem>.<callback>", where <stem>
	// is the file name of `script` without its extension ("Assets/Scripts/Board.luau" -> "Board"), so a runaway
	// OnFixedUpdate of Board.luau reads "Script:Board.OnFixedUpdate" (§13.2). Without a script it is "Script:<callback>"
	// (a test case body: "Script:Test.Case"), without a callback "Script:<stem>".
	void FormatScriptPhase(std::string& out, std::string_view script, std::string_view callback);

	// Sets a base phase for the lifetime of the scope and restores the previous one afterwards (main thread). `watchdog` may
	// be null (in-process calls without a server), which makes the scope do nothing.
	class WatchdogPhaseScope
	{
	public:
		WatchdogPhaseScope(Watchdog* watchdog, std::string_view phase);
		~WatchdogPhaseScope();

		WatchdogPhaseScope(const WatchdogPhaseScope&) = delete;
		WatchdogPhaseScope& operator=(const WatchdogPhaseScope&) = delete;
	private:
		Watchdog* m_Watchdog = nullptr; // documented back-reference: outlives the scope; null for none
		std::string m_PreviousPhase;    // restored by the destructor; empty for Idle
	};

}
