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
		// record, input.replay {withoutInput}, the games suite).
		bool WithoutInput = false;
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
	};

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
		// serial. Does not step or evaluate a chunk.
		[[nodiscard]] Status Begin(AssetRef<ReplayData> replay, const PlaySession& session, const ReplayPlaybackOptions& options = {});
		// Runs at most one Tick (fixed + frame), then evaluates all expectations at the resulting completed-tick boundary,
		// then (StrictHash) compares the boundary's checkpoint, completing the playback as failed when it differs (see
		// ReplayPlaybackResult::CheckpointFailed). Checks tick-0 expectations before the first Tick, including FinalTick=0.
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
