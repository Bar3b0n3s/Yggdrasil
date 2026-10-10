#pragma once

#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Session/PlayInput.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	class PlaySession;

	// Main-thread recorder, one per session. The host owns the initiating client and the output/provenance transaction.
	// This class never writes files or keeps a PlaySession pointer. Session calls CaptureAppliedInput immediately after
	// each ApplyTick, with GetLastAppliedEvents (including expanded taps), for every tick including empty ones.
	class ReplayRecorder
	{
	public:
		ReplayRecorder();
		~ReplayRecorder();
		ReplayRecorder(const ReplayRecorder&) = delete;
		ReplayRecorder& operator=(const ReplayRecorder&) = delete;

		// InvalidState unless tick 0 and no recording is active; validates the header and binds GetSerial. The host must
		// start/restart the correct scene, parameters and seed before Begin and ensure they match the header.
		[[nodiscard]] Status Begin(const ReplayHeader& header, const PlaySession& session);
		[[nodiscard]] Status CaptureAppliedInput(uint64_t tick, std::span<const PlayInputEvent> events);
		// First reason wins: external play-scene edits, hot reload, test-driver mutation/randomness, or discontinuity.
		// Ordinary gameplay writes and gameplay Scene.Load do not invalidate a recording.
		void Invalidate(std::string_view reason);
		// A validated snapshot, not an I/O operation: on success ends recording and returns the document. InvalidState
		// for invalidation/serial mismatch; InvalidArgument for bad expectation ticks. The host compiles all supplied
		// expectations before Finish and writes atomically afterwards. No partial file is published on any failure.
		// Finish proves structural validity only. FeatureTestRunner additionally verifies the candidate in fresh ordinary
		// gameplay before making it available for publication; it never replaces this observed hash with the verifier's.
		[[nodiscard]] Result<ReplayDocument> Finish(const PlaySession& session, std::span<const ReplayExpectation> expectations = {});
		// Idempotent; drops unwritten data (disconnect, stop, shutdown). Never writes an invalid recording.
		void Cancel();
		[[nodiscard]] bool IsRecording() const;
	private:
		struct State;
		Scope<State> m_State{};
	};

}
