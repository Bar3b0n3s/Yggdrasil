#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;

	enum class InputRecordAction : uint8_t
	{
		Start,
		Stop
	};

	struct InputRecordExpectation
	{
		uint32_t Tick = 0;
		std::string Luau{};
	};

	// action required. Start accepts scene/parameters/seed/restart; Stop requires path and accepts expect. Members of
	// the other action are rejected by presence, not silently ignored. Start while playing requires restart:true.
	struct InputRecordParams
	{
		InputRecordAction Action = InputRecordAction::Start;
		bool Restart = false;
		std::string Scene{};
		VariantValue Parameters{};
		uint32_t Seed = 0; // absent inherits the session/project seed
		std::string Path{};
		std::vector<InputRecordExpectation> Expect{};
	};

	struct InputRecordResult
	{
		bool Recording = false;
		uint32_t Tick = 0;
		uint32_t Events = 0;
		std::string Path{};
		std::string StateHash{};
		uint32_t Checkpoints = 0;    // the state-hash checkpoints written (ReplayCheckpointInterval); 0 on Start
		uint32_t InputCheckTick = 0; // where the input check ended (Stop with input events); 0 when none ran
		// Stop: notes on a written replay, none on Start (M19 decision 3, A-F29): the input check's
		// MakeFinalHashIndependentWarning when its run without input differed only at checkpoints and reproduced the
		// recorded final hash.
		std::vector<std::string> Warnings{};
	};

	struct InputReplayParams
	{
		std::string Path{}; // required, source replay path or cooked replay asset resolved by the host
		bool Verify = false;
		bool StrictHash = false;   // requires verify:true
		bool WithoutInput = false; // ReplayPlaybackOptions::WithoutInput: none of the replay's events are applied; requires verify:true
		// ReplayPlaybackOptions::ContinuePastCheckpoint (M19 decision 3, A-F29): a checkpoint that differs no longer ends the
		// playback; requires strictHash:true (InvalidArgument at /continuePastCheckpoint otherwise). The games and determinism
		// suites' input checks pass it, with withoutInput, to warn about a replay whose final hash alone does not depend on its
		// input.
		bool ContinuePastCheckpoint = false;
	};

	struct InputReplayResult
	{
		bool Passed = false;
		bool Verified = false;
		bool HashChecked = false;
		bool HashMatched = false;
		uint32_t FinalTick = 0; // ToAutomationCounter, as play.state; persistent replay ticks remain uint64_t
		std::string StateHash{};
		std::string ExpectedStateHash{};
		std::string StopReason{};        // ReplayPlaybackResult::StopReason: why playback ended before the replay's final tick
		uint32_t CheckpointsChecked = 0; // ReplayPlaybackResult::CheckpointsChecked
		bool CheckpointFailed = false;   // ReplayPlaybackResult::CheckpointFailed
		bool WithoutInput = false;       // the playback applied none of the replay's events (params.withoutInput)
		// continuePastCheckpoint (M19 decision 3): ReplayPlaybackResult::CheckpointsFailed and the tick of its
		// FirstCheckpointFailure; 0 otherwise.
		uint32_t CheckpointsFailed = 0;
		uint32_t FirstFailedCheckpointTick = 0;
		// ReplayPlaybackResult::Changes (M19 decision 2, A-FPRINT): what changed since the recording, by the replay's start
		// fingerprint, when a hash differed in a playback with its input; the error's message names the first.
		std::vector<std::string> Changes{};
		struct Expectation
		{
			uint32_t Tick = 0;
			bool Evaluated = false;
			bool Satisfied = false;
			VariantValue Value{};
			std::string Message{};
			std::string File{};
			uint32_t Line = 0;
			std::string JsonPointer{};
		};
		std::vector<Expectation> Expect{};
	};

	// input.readReplay {path, cursor?, limit?} (§13.5; Docs/Decisions/0030-m19-contract.md decision 14, A-RREAD).
	struct InputReadReplayParams
	{
		std::string Path{};   // required: a project .replay source, project-relative or project://
		std::string Cursor{}; // convention 9: opaque, "" for the first event
		uint32_t Limit = 100; // events per page, 1 to 1000
	};

	// Registry struct "InputReadReplayCheckpoint": one state-hash checkpoint of the replay.
	struct InputReadReplayCheckpoint
	{
		uint32_t Tick = 0;
		std::string StateHash{};
	};

	struct InputReadReplayResult
	{
		std::string Path{};         // the replay's canonical project-relative path
		uint32_t FormatVersion = 0; // the file's own version, before migration (ReplayLoadReport::FileVersion)
		// The header as the file writes it, PascalCase keys (embedded data, convention 2): {Scene: {Handle, Path},
		// Parameters, Seed, FixedHz, EngineVersion, Config}; Seed keeps its 64 bits.
		VariantValue Header{};
		uint32_t FinalTick = 0;
		std::string FinalStateHash{};
		uint32_t EventCount = 0;                      // every input event of the replay
		std::vector<VariantValue> Events{};           // this page (see InputReadReplay)
		std::string NextCursor{};                     // "" when the page ends with the replay's last event
		std::vector<InputRecordExpectation> Expect{}; // {tick, luau}, the form input.record's and test.run's expect take
		std::vector<InputReadReplayCheckpoint> Checkpoints{};
		bool HasFingerprint = false; // the replay carries a start fingerprint (version 3 and recorded, decision 2)
	};

	namespace Automation {

		// Recording owns lockstep from tick zero and observes every applied input, including test/device events.
		// Validates the complete request before replacing a session; Start's parameters become the session's in the replay
		// header's form (CanonicalizeReplayParameters: floats as float; out of range is InvalidArgument at /parameters/...).
		// A mutation/reload invalidates the recording; Stop then returns its reason, writes nothing and ends the recording,
		// so the owner can start again (restart:true). Another lockstep owner's call is InvalidState. Writes use
		// the host's confined replay-output path and normal provenance, with no dry-run/batch promise. Stop validates
		// write authorization before consuming the recording. The host cancels the immediate Start's recorder on client
		// disconnect, without needing a pending Poll, and writes no partial/invalid recording.
		// Pending (M16 FX-4): Start resolves in its first Poll. Every Stop first verifies the candidate with its input (M17,
		// Docs/Decisions/0027-m17-contract.md decision 3, A1; from M19 a recording without input events too,
		// Docs/Decisions/0030-m19-contract.md decision 4, AL-EVFREE): the play session restarts for the candidate
		// (RestartForReplay, as input.replay does) and plays it with ReplayPlaybackOptions{Verify, StrictHash} and its input,
		// its expectations compiled from their source. Unless that playback passes (every checkpoint, the final hash and every
		// expectation reproduced, no early stop) and raises no script error, Stop writes nothing and fails with Validation at
		// /path naming the first failure, a hint and the playback's InputReplayResult as error data "verification", and the play
		// session is the verification's. Once the playback reached its final tick with nothing else failed, the verification
		// stops its session's scripts (ScriptEngine::Stop, as test.run does before it reads a replay's errors) and counts the
		// errors their OnDestroy raises too (decision 5, AL-TEARDOWN): the first such error refuses the recording like any other
		// script error, its reason naming it "at teardown", and the play session is then the verification's, paused at its final
		// tick with its scripts stopped (script.eval in the play context is refused there; scene.query and entity.get read its
		// final state). A recording without input events is then written (inputCheckTick 0): it has no input to depend on.
		// One with input events gets the recording guard, the input check: the play session restarts again and plays the
		// candidate WithoutInput with ContinuePastCheckpoint (decision 3, A-F29). When that playback passes (every checkpoint,
		// the final hash and every expectation reproduced without input), whatever script errors it raised, Stop writes nothing
		// and fails with Validation naming the replay ("its outcome does not depend on its input"), a hint and the check's
		// InputReplayResult as error data "inputCheck"; otherwise it writes the replay (inputCheckTick: the tick of the first
		// checkpoint that differed, else where the check ended). When the check failed only at checkpoints, every expectation
		// satisfied and the recorded final hash reproduced, the replay is written with one warning in InputRecordResult::
		// Warnings (MakeFinalHashIndependentWarning): its final hash does not depend on its input, so only its checkpoints prove
		// the play. Each playback advances at least one tick per Poll within PlayStepFrameBudget. Whatever the outcome, the play
		// session is then the last playback's, paused where it ended (the input check's at the replay's final tick, since it
		// continues past its checkpoints), with lockstep released, as after input.replay. A disconnect during either playback
		// writes nothing. The verification names no divergence causes in its stop reason (ReplayPlaybackOptions::
		// NameDivergenceCauses false) and no fingerprint changes: nothing changed since a recording made moments before.
		[[nodiscard]] Result<Scope<PendingOperation>> InputRecord(AutomationMethodContext& context, const InputRecordParams& params);
		// A fresh session from the validated header. Pending: at least one tick per Poll within the common 50 ms
		// budget, never an unbounded synchronous replay. Reject another time owner or active driver before replacing
		// the session. Completion, failure and cancellation pause the owned session and release this operation's input
		// suppression, stepping, audio/time and temporary lockstep ownership without another Poll. Check session serial
		// before cleanup, so a replacement session is untouched. Verification uses cooked Expect bytecode; Dist never
		// compiles a predicate. Errors identify replay/expectation/tick. A strict hash or expectation failure returns
		// Validation with the complete outcome in error data, preserving all locations and hash status; so does gameplay
		// that quits or stops fatally before the replay's final tick (stopReason, the tick reached, unevaluated later
		// expectations), which is a failed replay, never Cancelled. With strictHash a checkpoint whose hash differs ends the
		// playback there as failed (checkpointFailed, the tick and both hashes in the message and the outcome); its error, and
		// that of a final hash that differs, carries GetReplayDivergenceHint as its hint (M17 friction F03). withoutInput
		// applies none of the replay's events (ReplayPlaybackOptions::WithoutInput): a replay whose verification still passes
		// that way proves nothing about its input. That playback is the input check, whose failure is the expected outcome
		// for a replay that depends on its input, so its errors carry no hint, neither they nor its stop reason name the
		// divergence causes, and their message says that the replay ran without its input events. withoutInput without
		// verify checks nothing, so it is InvalidArgument at /withoutInput, as strictHash without verify is at /strictHash.
		// M19 (Docs/Decisions/0030-m19-contract.md decision 3, A-F29): continuePastCheckpoint plays on past every checkpoint
		// that differs to the final tick (checkpointsFailed, firstFailedCheckpointTick, the final hash compared as usual) and
		// fails the replay as before when any differed, its message naming the first failed checkpoint's tick and hashes;
		// without strictHash it is InvalidArgument at /continuePastCheckpoint. M19 (decision 2, A-FPRINT): a checkpoint or
		// final hash that differs in a playback with the replay's input reports the start fingerprint's changes (changes),
		// and the error's message ends with "; changed since the recording: <the first change>" when there is one.
		[[nodiscard]] Result<Scope<PendingOperation>> InputReplay(AutomationMethodContext& context, const InputReplayParams& params);
		// input.readReplay (M19 decision 14, A-RREAD): the replay at params.Path read back, through
		// AutomationMethodContext::ReadReplayDocument (the project's source .replay, migrated to the current version), so a
		// replay made with input.record can be re-recorded from its own events. Immediate; reads only. Events are a page of
		// at most params.Limit from params.Cursor (convention 9), each in play.step's input form (InputEventParams: camelCase
		// keys, only the members its type reads, enum names in PascalCase) with its tick the replay's absolute tick, so
		// play.step {ticks: finalTick, input: events} from tick 0 of the header's session applies every event of a replay
		// whose events fit one page. Errors: InvalidArgument for a malformed cursor (at /cursor) or a limit outside 1 to
		// 1000; those of ReadReplayDocument (NotFound, Validation and Parse located in the file, InvalidArgument for a
		// path outside the project, Unsupported in the Runtime, which holds no replay sources).
		[[nodiscard]] Result<InputReadReplayResult> InputReadReplay(AutomationMethodContext& context, const InputReadReplayParams& params);

	}

	void RegisterReplayMethodTypes(TypeRegistry& registry);
	// input.record and input.replay are tools and available in Runtime, require an open project/session, and are neither
	// batch nor dry-run operations. Neither is Mutates (only transient state unless Stop writes); Stop explicitly checks
	// write policy, as test.run does for its optional recording. Both are pending, with a 600-second timeout. M19:
	// input.readReplay is an immediate, read-only tool (input_read_replay), editor only (not AvailableInRuntime), neither a
	// batch nor a dry-run operation.
	void RegisterReplayMethods(MethodRegistry& methods);

}
