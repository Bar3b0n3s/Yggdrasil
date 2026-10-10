#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/InputMethods.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"
#include "Engine/Session/PlaySession.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The play methods (Architecture §5.6, §13.5 "play", §13.6): play.start, play.stop, play.pause, play.resume, play.step,
// play.state and play.setTimeScale, typed on AutomationMethodContext so the Editor and the Runtime share them (§13.5
// "Runtime subset": every play method except start and stop). Conventions as in MethodRegistry.h; frozen by the M7
// contract (Docs/Decisions/0012-m7-decisions.md decisions 3 and 7).
//
// Run states, in the vocabulary of "_meta".playState, session.info's playState and the PlayStateChanged event (since M4):
// Edit (the editor without a session), Play or Simulate (a running session, by its mode), Paused (play.state's "mode" then
// tells Play from Simulate). Lockstep (§13.6): play.start {lockstep: true} makes the requesting client the owner; ticks
// then advance only through the owner's play.step, and every other client's play.stop, play.pause, play.resume, play.step,
// play.setTimeScale and input.inject fail with InvalidState naming the owner ("Lockstep is owned by one client; others get
// InvalidState"). Edits of the play scene (target "play", §13.4) stay open to every client: lockstep owns time, not the
// scene, and an observer debugging the session needs them; a recording (M13) is invalidated by them anyway. The owner's
// play.pause leaves lockstep and pauses (as its disconnect does, §13.2); play.resume of a lockstep session is InvalidState
// ("lockstep sessions advance only through play.step"). Without lockstep, play.step needs a paused session (InvalidState
// "pause first or start with lockstep" while running), runs its ticks and leaves it paused (§5.6 "Step(n)"). One play.step
// runs at a time per session (a second one is InvalidState).
//
// play.step (a pending operation, §13.2, §13.6): queues its input events (stamped relative to the first tick it runs),
// then each frame runs as many ticks as fit PlayStepFrameBudget of the host's wall clock
// (AutomationMethodContext::GetWallClockTime), at least one (PlaySession::Tick: fixed step + frame phase), while the session's
// stepping flag keeps the host's loop unthrottled (§4.2). render:
//   - "every": one tick per frame, and every tick's frame phase extracts the game view, which the host renders;
//   - "last" (default): budgeted ticks; only the last tick extracts (the frames before it render the previous view);
//   - "none": budgeted ticks; no tick extracts.
// The budget decides only how ticks are split across frames, never their results: the final tick and state hash are
// identical for any budget, machine and configuration (§1.3). Cancelled when the client disconnects: the ticks already run
// stay, extraction is re-enabled, the stepping flag cleared, and the disconnect releases lockstep (§13.2). The operation
// holds no pointer to the session: every Poll resolves it again (AutomationMethodContext::GetPlaySession) and checks it is
// the session the step started on (PlaySession::GetSerial, never its address), so a session that ended meanwhile (the owner's play.stop, the editor UI's Stop in M10)
// resolves the step with Cancelled "the play session ended after <n> of <ticks> ticks".
//
// M13 adds play.waitFor (§13.6): a Luau predicate evaluated in the live Play VM after every tick. play.start carries
// parameters into Scene.GetLoadParameters and applies the optional pauseOnError override to this session only.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;

	// play.step's wall-clock budget per frame (§4.2 step 4, §13.6): ticks run until it is spent, at least one per frame.
	inline constexpr std::chrono::milliseconds PlayStepFrameBudget{ 50 };
	// The most ticks one play.step may ask for: about 4.6 hours of game time at 60 Hz.
	inline constexpr uint32_t MaxPlayStepTicks = 1000000;

	// Registry enum "PlayRunState": what play.state reports as "state", with the names "_meta".playState uses
	// (EditorPlayController::GetPlayStateName, MetaState).
	enum class PlayRunState : uint8_t
	{
		Edit,     // no session (editor only)
		Play,     // a running Play session: advancing with the host's frames, or through play.step in lockstep
		Simulate, // a running Simulate session (editor only)
		Paused    // a paused session of either mode (PlayStateResult::Mode)
	};

	// The run state of `session`: Edit for null (the editor without a session), Paused for a paused session, else Play or
	// Simulate by its mode. The one mapping behind play.state's "state", session.info's playState, both hosts' "_meta"
	// playState and the PlayStateChanged event's name (Docs/Decisions/0012-m7-decisions.md decision 8).
	[[nodiscard]] PlayRunState GetPlayRunState(const PlaySession* session);
	// The registry name of `state`: "Edit", "Play", "Simulate" or "Paused".
	[[nodiscard]] std::string_view PlayRunStateToString(PlayRunState state);

	// Registry enum "PlayStepRender": which ticks of a play.step render (see the file comment).
	enum class PlayStepRender : uint8_t
	{
		Every,
		Last,
		None
	};

	// play.start {mode?, lockstep?, seed?, scene?, parameters?, paused?, timeScale?, pauseOnError?} (§13.5). Registry enum
	// "PlayMode" (Play, Simulate) is registered with it.
	struct PlayStartParams
	{
		PlayMode Mode = PlayMode::Play;
		bool Lockstep = false;
		uint32_t Seed = 0;         // absent (HasParam): Project Simulation.Seed ^ Scene.Seed (§4.12)
		std::string Scene{};       // a project scene path ("Assets/Scenes/Level2.scene") to play instead of the open edit scene
		VariantValue Parameters{}; // Initial Scene.GetLoadParameters object; omitted means empty.
		bool Paused = false;
		float TimeScale = 1.0f;   // 0 to PlaySession::MaxTimeScale
		bool PauseOnError = true; // Per-session override; omitted inherits project policy.
	};

	// play.state's result, and the result of play.start, play.pause, play.resume and play.setTimeScale (the state after the
	// call).
	struct PlayStateResult
	{
		PlayRunState State = PlayRunState::Edit;
		PlayMode Mode = PlayMode::Play; // meaningful outside Edit; tells Play from Simulate while Paused
		uint32_t Tick = 0;              // the next tick: ticks run so far (ToAutomationCounter)
		std::string StateHash{};        // PlaySession::ComputeStateHash as 16 lowercase hex digits; empty in Edit
		bool Lockstep = false;
		std::string LockstepOwner{}; // the owning client's name; empty without lockstep or for an in-process driver
		bool OwnedByCaller = false;  // the requesting client owns lockstep
		float TimeScale = 1.0f;
		bool Modified = false;  // §7.5 race rule 4
		bool Recording = false; // Whether the session recorder is active.
		uint32_t EntityCount = 0;
		uint32_t MaxEntities = 0;
		// The game input as the last tick's step view saw it (PlayInput::GetSummary(InputPhase::Step)).
		PlayInputSummary Input{};
	};

	// play.stop's result: where the session ended.
	struct PlayStopResult
	{
		uint32_t Tick = 0;
		std::string StateHash{};
	};

	// play.step {ticks, input?, render?} (§13.5, §13.6).
	struct PlayStepParams
	{
		uint32_t Ticks = 1; // required; 1 to MaxPlayStepTicks
		// Events stamped with a tick offset from the first tick this call runs (InputEventParams::Tick < Ticks).
		std::vector<InputEventParams> Input{};
		PlayStepRender Render = PlayStepRender::Last;
	};

	struct PlayStepResult
	{
		uint32_t Tick = 0;       // the session's tick after the call (ticks run so far)
		std::string StateHash{}; // 16 lowercase hex digits
		uint32_t Ticks = 0;      // ticks this call ran (= params.ticks)
		uint32_t Frames = 0;     // host frames the call spanned (1 when every tick fit one frame's budget)
		uint32_t Rendered = 0;   // game-view extractions during the call (PlaySession::GetExtractionCount's growth)
	};

	// play.setTimeScale {scale} (§13.5).
	struct PlaySetTimeScaleParams
	{
		float Scale = 1.0f; // required; 0 to PlaySession::MaxTimeScale
	};

	struct PlayWaitForParams
	{
		std::string Until{};         // required Luau chunk returning a value
		uint32_t TimeoutTicks = 600; // required; 1 to MaxPlayStepTicks
	};

	struct PlayWaitForResult
	{
		bool Satisfied = false;
		uint32_t Tick = 0;
		VariantValue Value{}; // last predicate result, finite acyclic JSON
	};

	namespace Automation {

		// play.start (editor only): AutomationMethodContext::StartPlay with the params (the requesting client as the lockstep
		// owner). Errors: InvalidArgument at /parameters for a non-object; those of StartPlay.
		[[nodiscard]] Result<PlayStateResult> PlayStart(AutomationMethodContext& context, const PlayStartParams& params);
		// play.stop (editor only). Errors: InvalidState naming the owner for another client of a lockstep session; those of
		// AutomationMethodContext::StopPlay (InvalidState "not playing").
		[[nodiscard]] Result<PlayStopResult> PlayStop(AutomationMethodContext& context, const NoParams& params);
		// play.pause. Errors: InvalidState "not playing"; InvalidState naming the owner for another client of a lockstep session.
		[[nodiscard]] Result<PlayStateResult> PlayPause(AutomationMethodContext& context, const NoParams& params);
		// play.resume. Errors: InvalidState "not playing"; InvalidState for a lockstep session.
		[[nodiscard]] Result<PlayStateResult> PlayResume(AutomationMethodContext& context, const NoParams& params);
		// play.step (pending). Errors, before anything runs: InvalidState "not playing", for a running session without
		// lockstep, for another client of a lockstep session and while another play.step runs; InvalidParams for ticks out of
		// range and for an event whose tick is >= ticks (located at /input/<i>/tick); the event errors of input.inject.
		[[nodiscard]] Result<Scope<PendingOperation>> PlayStep(AutomationMethodContext& context, const PlayStepParams& params);
		// play.state; Edit without a session.
		[[nodiscard]] Result<PlayStateResult> PlayState(AutomationMethodContext& context, const NoParams& params);
		// play.setTimeScale. Errors: InvalidState "not playing" or for another client of a lockstep session; InvalidParams for
		// a scale out of range.
		[[nodiscard]] Result<PlayStateResult> PlaySetTimeScale(AutomationMethodContext& context, const PlaySetTimeScaleParams& params);
		// M13: pending, with the same admission, session-serial checks, frame budget and cancellation as play.step.
		// Compiles once before advancing; evaluates in the play VM after each tick. Lua truthiness decides satisfaction;
		// timeout returns satisfied:false and the last value, script faults return a located Error. No dry run/batch.
		// Requires Play (Simulate has no VM). Predicate execution has external-driver origin: before any host write
		// or session RNG use it invokes OnExternalMutation, invalidating an active input-only recording even on fault.
		// Re-resolves the current VM after Scene.Load. Tool, AvailableInRuntime, not Mutates, timeout 600 seconds.
		[[nodiscard]] Result<Scope<PendingOperation>> PlayWaitFor(AutomationMethodContext& context, const PlayWaitForParams& params);

		// The state of `context`'s session as play.state reports it (shared by the handlers above and input.inject).
		[[nodiscard]] PlayStateResult MakePlayStateResult(AutomationMethodContext& context);

	}

	// Registers PlayMode, PlayRunState, PlayStepRender, PlayStartParams, PlayStateResult, PlayStopResult, PlayStepParams,
	// PlayStepResult, PlaySetTimeScaleParams and PlayWaitForParams/Result (after RegisterInputMethodTypes, whose InputEventParams and PlayInputSummary
	// they use).
	void RegisterPlayMethodTypes(TypeRegistry& registry);

	// Registers the play methods: play.pause, play.resume, play.step, play.state and play.setTimeScale for every host
	// and play.waitFor (AvailableInRuntime), play.start and play.stop only when `includeEditorMethods`. Flags: play.start, play.stop and
	// play.step and play.waitFor are tools (§13.8: play_start, play_stop, play_step, play_waitFor); none supports a dry run (§13.4: play.* return
	// Unsupported for dryRun); none is AllowedInBatch (they are not commands); none is available in the launcher state;
	// none Mutates (a session never changes project files or the edit scene). play.step and play.waitFor are pending with a timeout of 600 s.
	void RegisterPlayMethods(MethodRegistry& methods, bool includeEditorMethods);

}
