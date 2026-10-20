#pragma once

#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class PlaySession;

	struct ReplayPlaybackOptions
	{
		bool Verify = true;
		// Requires Verify. FeatureTest and determinism always set both true. Compares every state-hash checkpoint as playback
		// reaches its tick (ReplayData::Checkpoints) and the final hash at FinalTick.
		bool StrictHash = false;
		// Plays the replay's header and tick count with none of its input events: the check that its outcome depends on its
		// input. Expectations, checkpoints and the final hash are compared as usual, so a playback that still passes proves
		// that the replay's verification does not depend on its input (the recording guard of input.record and test.run's
		// record, input.replay {withoutInput}, the games suite). Failing that way is the expected outcome of a replay that
		// depends on its input, so a checkpoint that differs names no divergence causes (GetReplayDivergenceCauses) in
		// ReplayPlaybackResult::StopReason, which says instead that the replay ran without its input events.
		bool WithoutInput = false;
		// Whether a checkpoint that differs in a playback with the replay's input names the divergence causes
		// (GetReplayDivergenceCauses) in ReplayPlaybackResult::StopReason. False keeps the plain "the state hash at checkpoint
		// tick T is X, but the recording's is Y": input.record stop's verification of a candidate recorded moments before
		// (Docs/Decisions/0027-m17-contract.md decision 3, A1), where nothing changed since the recording and its refusal's
		// hint names the usual causes instead (M17 review C-01). Ignored WithoutInput, which never names them.
		bool NameDivergenceCauses = true;
		// Requires StrictHash (InvalidArgument from Begin otherwise; Docs/Decisions/0030-m19-contract.md decision 3, A-F29). A
		// checkpoint whose hash differs no longer ends the playback: the first one is kept in
		// ReplayPlaybackResult::FirstCheckpointFailure, every one counts in CheckpointsFailed, and the playback goes on to
		// FinalTick and compares the final hash as usual (HashMatched), so the input check can tell a recording whose final
		// hash alone does not depend on its input. Such a playback has failed whenever a checkpoint differed (Passed false,
		// CheckpointFailed true), whatever the final hash; StopReason stays empty unless gameplay ended it early.
		bool ContinuePastCheckpoint = false;
	};

	// The first checkpoint whose state hash differed in a playback that continued past it
	// (ReplayPlaybackOptions::ContinuePastCheckpoint).
	struct ReplayCheckpointFailure
	{
		uint64_t Tick = 0;
		std::string StateHash{};         // observed
		std::string ExpectedStateHash{}; // recorded
	};

	struct ReplayExpectationOutcome
	{
		uint64_t Tick = 0;
		bool Evaluated = false;
		bool Satisfied = false;
		VariantValue Value{};
		std::string File{};
		uint32_t Line = 0;
		std::optional<Error> Failure{};
	};

	struct ReplayPlaybackResult
	{
		bool Passed = false;
		bool Verified = false;
		bool HashChecked = false;
		bool HashMatched = false;
		std::vector<ReplayExpectationOutcome> Expect{};
		uint64_t FinalTick = 0; // the completed tick playback reached: the replay's FinalTick unless it stopped early
		std::string StateHash{};
		std::string ExpectedStateHash{};
		// Why playback ended before the replay's FinalTick: the game quit (Application.Quit), a fatal script error stopped
		// the session, or a checkpoint's state hash differed (CheckpointFailed). Empty when it reached FinalTick. A stopped
		// playback has failed: HashMatched is false and the expectations after FinalTick (the tick reached) are reported
		// unevaluated with a located failure.
		std::string StopReason{};
		// The checkpoints compared (StrictHash only), the failed one included. Playback stops at the first checkpoint whose
		// state hash differs, after that boundary's expectations: CheckpointFailed is true, FinalTick is the checkpoint's
		// tick, StateHash and ExpectedStateHash are the observed and the recorded hash there, and StopReason names the tick
		// and both hashes. The failed checkpoint is ReplayData::Checkpoints[CheckpointsChecked - 1].
		uint64_t CheckpointsChecked = 0;
		bool CheckpointFailed = false;
		// ContinuePastCheckpoint (decision 3): every checkpoint whose hash differed, and the first of them; 0 and nullopt
		// otherwise (a playback that stopped at its failed checkpoint describes it in FinalTick, StateHash and
		// ExpectedStateHash, as before).
		uint64_t CheckpointsFailed = 0;
		std::optional<ReplayCheckpointFailure> FirstCheckpointFailure{};
		// What differs between the replay's start fingerprint (ReplayData::Fingerprint) and this playback's own, taken at
		// Begin (ComputeReplayFingerprint; Docs/Decisions/0030-m19-contract.md decision 2, A-FPRINT): one line per change,
		// DescribeReplayFingerprintChanges' lines, at most MaxReplayFingerprintChanges. Filled only when a hash differed (the
		// failed checkpoint or the final hash) in a playback with the replay's input; empty for a replay without a fingerprint
		// (one migrated from version 1 or 2), for a playback WithoutInput and when nothing differs. A checkpoint's StopReason
		// and the errors of input.replay and test.run's Checkpoint@<tick> and FinalStateHash rows name the first of them.
		std::vector<std::string> Changes{};
	};

	// M17 friction F03: what a replay whose state hash differs from the recording's at a checkpoint or at its final tick
	// means. The state hash covers every serialized component, the look's included (PostProcess, Environment, the lights,
	// Camera, Text), so tuning the look after a recording fails its replay like any other change.
	// Both belong only to a playback with the replay's input, never to one WithoutInput (ReplayPlaybackOptions).
	//   GetReplayDivergenceCauses: the causes, which ReplayPlaybackResult::StopReason names for a checkpoint that differs
	//   unless ReplayPlaybackOptions::NameDivergenceCauses is false.
	//   GetReplayDivergenceHint: the causes and the remedy, the hint of input.replay's checkpoint and final-hash failures and
	//   the text test.run's Checkpoint@<tick> and FinalStateHash rows add to their messages.
	[[nodiscard]] std::string_view GetReplayDivergenceCauses();
	[[nodiscard]] std::string_view GetReplayDivergenceHint();

	// M19 (Docs/Decisions/0030-m19-contract.md decision 3, A-F29; ADR 0026 friction F29): the warning of an input check
	// (a playback WithoutInput with ContinuePastCheckpoint) whose run differed from the recording at checkpoints, so the
	// recording passes the guard, but reproduced its final state hash: the final hash proves only the end state, not the
	// play. Exactly: "'<replay>': its final state hash does not depend on its input: without its input events the run first
	// differed from the recording at checkpoint tick <checkpointTick>, then reached the recorded final state hash
	// <finalStateHash>, so only its checkpoints prove the play; end the recording while the played state is still in the
	// scene". input.record's Warnings, test.run's recording block and the games and determinism suites' input checks give
	// it word for word (the suites' Python spells the same text).
	[[nodiscard]] std::string MakeFinalHashIndependentWarning(std::string_view replay, uint64_t checkpointTick, std::string_view finalStateHash);

	// The adapter executes expectation.Script in the current play VM using ScriptEngine::ExecuteBytecode.
	// Re-resolve that VM for each call, especially after Scene.Load. No compiler, Automation or Testing dependency.
	class IReplayEvaluator
	{
	public:
		virtual ~IReplayEvaluator() = default;
		[[nodiscard]] virtual Result<Json> Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation) = 0;
	};

	// A main-thread, compiler-free driver shared by input.replay and FeatureTest (including Dist). It does not own a
	// session, host clock, audio engine or lockstep client. The caller owns them and re-resolves the session each call.
	class ReplayPlayer
	{
	public:
		ReplayPlayer();
		~ReplayPlayer();
		ReplayPlayer(const ReplayPlayer&) = delete;
		ReplayPlayer& operator=(const ReplayPlayer&) = delete;

		// Session is fresh at tick 0 with the header's scene/parameters/seed/FixedHz; host validates scene identity and
		// suppresses live input before calling. Checks the entire event stream against the action map before queueing
		// anything (with options.WithoutInput too, which then queues none), stores immutable replay data and the session
		// serial. Does not step or evaluate a chunk. With the replay's input and a replay fingerprint, it also takes the
		// session's own fingerprint (ComputeReplayFingerprint(session.GetScene(), session.GetScriptSchemas(),
		// session.GetAssets()), decision 2), which ReplayPlaybackResult::Changes compares when a hash differs; its errors fail
		// Begin. InvalidArgument for ContinuePastCheckpoint without StrictHash.
		[[nodiscard]] Status Begin(AssetRef<ReplayData> replay, const PlaySession& session, const ReplayPlaybackOptions& options = {});
		// Runs at most one Tick (fixed + frame), then evaluates all expectations at the resulting completed-tick boundary,
		// then (StrictHash) compares the boundary's checkpoint, completing the playback as failed when it differs (see
		// ReplayPlaybackResult::CheckpointFailed), or with ContinuePastCheckpoint recording it and going on (decision 3).
		// Checks tick-0 expectations before the first Tick, including FinalTick=0.
		// Returns true when complete. A gameplay
		// quit request or a fatal stop (PlaySession::GetFatalError) that keeps the session from reaching FinalTick completes
		// the playback as failed (ReplayPlaybackResult::StopReason), never an outer error. Any other changed serial or
		// unexpected tick is Cancelled. Gameplay Scene.Load preserves serial and absolute tick. False predicates and
		// predicate errors are collected as outcomes; structurally invalid playback/session failure is an outer error.
		[[nodiscard]] Result<bool> Advance(PlaySession& session, IReplayEvaluator& evaluator);
		[[nodiscard]] Result<ReplayPlaybackResult> GetResult() const;
		// Idempotent; drops all unconsumed authored events and evaluator work. Already applied input stays at the paused
		// boundary's state; an applied Tap retains its generated next-tick Up. Explicit held buttons/axes remain until
		// subsequent input or input.inject releaseAll changes them. No future authored replay event enters PlayInput.
		// Caller separately pauses/releases its own time/input/audio lease; no session pointer is retained here.
		void Cancel();
	private:
		struct State;
		Scope<State> m_State{};
	};

}
