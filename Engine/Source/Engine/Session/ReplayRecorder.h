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
		// start/restart the correct scene, parameters and seed before Begin and ensure they match the header. From replay
		// version 3 (stream B; Docs/Decisions/0030-m19-contract.md decision 2) it also takes the start fingerprint,
		// ComputeReplayFingerprint(session.GetScene(), session.GetScriptSchemas(), session.GetAssets()), whose errors fail
		// Begin, and Finish writes it into the candidate's Fingerprint; the test runner's concatenated recording keeps its
		// first segment's.
		[[nodiscard]] Status Begin(const ReplayHeader& header, const PlaySession& session);
		[[nodiscard]] Status CaptureAppliedInput(uint64_t tick, std::span<const PlayInputEvent> events);
		// The state-hash checkpoint of the completed-tick boundary `session` (this recorder's own session) is at: when its
		// tick is a positive multiple of ReplayCheckpointInterval, the recording keeps {tick, ComputeStateHash()}. Session
		// calls it at the start of every fixed step of an active recording that starts from a completed-tick boundary, before
		// the step applies input, so the hash describes the boundary the previous tick (its frame phase and pending
		// Scene.Load included) left; ReplayPlayer compares at the same boundary. A step that follows the previous one in the
		// same frame (a FeatureTest suite's scripted clock grouping steps) starts from no completed boundary, since the
		// previous tick's frame phase has not run, so its tick gets no checkpoint even when it is a multiple of the interval
		// (readers accept any increasing ticks). The final tick's hash stays Finish's FinalStateHash. Does nothing while no
		// recording is active, after invalidation or on a tick discontinuity (CaptureAppliedInput invalidates that).
		void CaptureCheckpoint(const PlaySession& session);
		// First reason wins: external play-scene edits, hot reload, test-driver mutation/randomness, or discontinuity.
		// Ordinary gameplay writes and gameplay Scene.Load do not invalidate a recording.
		void Invalidate(std::string_view reason);
		// A validated snapshot, not an I/O operation: on success ends recording and returns the document. InvalidState
		// for invalidation/serial mismatch, which also ends the recording (it can never succeed, and the owner must be able
		// to restart: input.record start with restart:true, test.run and input.replay are then admitted again);
		// InvalidArgument for bad expectation ticks, which keeps it so a corrected Stop can retry. The host compiles all
		// supplied expectations before Finish and writes atomically afterwards. No partial file is published on any failure.
		// Finish proves structural validity only. FeatureTestRunner additionally verifies the candidate in fresh ordinary
		// gameplay before making it available for publication; it never replaces this observed hash with the verifier's.
		// The candidate carries the checkpoints CaptureCheckpoint kept (every positive multiple of ReplayCheckpointInterval
		// before FinalTick that is a completed-tick boundary). Both publishers (input.record stop, test.run's record) then refuse a candidate with input
		// events whose outcome an input-free run reproduces (ReplayPlaybackOptions::WithoutInput).
		[[nodiscard]] Result<ReplayDocument> Finish(const PlaySession& session, std::span<const ReplayExpectation> expectations = {});
		// Idempotent; drops unwritten data (disconnect, stop, shutdown). Never writes an invalid recording.
		void Cancel();
		[[nodiscard]] bool IsRecording() const;
	private:
		struct State;
		Scope<State> m_State{};
	};

}
