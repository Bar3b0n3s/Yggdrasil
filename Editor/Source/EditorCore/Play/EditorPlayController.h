#pragma once

#include "Engine/Asset/ReplayData.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
#include "Engine/Platform/Events.h"
#include "Engine/Session/PlaySession.h"

#include <cstdint>
#include <optional>
#include <string>

// The editor's play mode (Architecture §5.6, §12.4, §13.5 play.start/play.stop): at most one PlaySession at a time, made
// from the open edit scene through the serializer copy, never touching the edit scene. Owned by EditorContext
// (EditorContext::GetPlay); automation reaches it through EditorMethodContext (AutomationMethodContext::StartPlay,
// StopPlay, GetPlaySession), EditorApp's frame hooks drive it, and AutomationServer reports its state in "_meta" and
// releases a disconnected client's lockstep through it (§13.2).
//
// Frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 11). Main thread only; not copyable.

namespace Engine {

	class EditorContext;
	class EditorFeatureTestHost;
	class ScriptApiRegistry;
	class PlaySession;
	struct PlayStartOptions;

	class EditorPlayController
	{
	public:
		// `editor` is a documented back-reference that owns the controller.
		explicit EditorPlayController(EditorContext& editor);
		// Stops a running session (as Stop, without its events).
		~EditorPlayController();

		EditorPlayController(const EditorPlayController&) = delete;
		EditorPlayController& operator=(const EditorPlayController&) = delete;

		// Starts a session (§5.6, §13.5 play.start): the scene `options.ScenePath` names (strictly loaded from project://), or
		// the open edit scene copied through SceneSerializer::ToJson; the PlaySessionSpecification with a copy of the project's
		// settings (PlaySessionSpecification::Project), options.Mode and the seed (options.Seed, else
		// PlaySession::ComputeSessionSeed(Simulation.Seed, the scene's Seed)); then the run state: paused when options.Paused
		// or initialization requested a pause (script error, Debug.Break or fatal stop),
		// options.TimeScale, and lockstep owned by options.LockstepOwner when options.Lockstep. While a session is in lockstep
		// the editor defers asset and script reloads until it ends (§7.5 race rule 4, EditorAssetManager); a reload during
		// ordinary play marks the session modified. Appends PlayStateChanged ("Play" or "Simulate", or "Paused"). Errors:
		// InvalidState while a session runs, without a project, without an open scene (for an empty ScenePath), or while
		// the asset manager has error diagnostics (EditorAssetManager::HasErrorDiagnostics; §7.2: "fix them first", the
		// hint names project.validate); InvalidArgument for a time scale outside [0, PlaySession::MaxTimeScale]; for
		// ScenePath the errors of EditorMethodContext::ResolveProjectPath's rules and of the load; those of
		// PlaySession::Create. Nothing changes on error.
		[[nodiscard]] Status Start(const PlayStartOptions& options);

		// Stops the session (§5.6 Stop): destroys it, applies the reloads deferred while it was in lockstep, and appends
		// PlayStateChanged ("Edit"). The edit scene was never touched. Errors: InvalidState "not playing".
		[[nodiscard]] Status Stop();

		// M13 host state outlives every VM and is retained across Stop/Scene.Load.
		[[nodiscard]] ScriptErrorStream& GetScriptErrors();
		[[nodiscard]] Result<ScriptApiRegistry*> GetScriptApi();
		[[nodiscard]] Status StartRecording(const PlayStartOptions& options, bool restart);
		[[nodiscard]] Status StartReplay(const ReplayHeader& header, ClientId owner);
		[[nodiscard]] Result<ReplayHeader> DescribeReplayHeader() const;
		void ReleaseReplayInput(uint64_t sessionSerial);
		[[nodiscard]] bool IsLiveInputSuppressed() const;
		[[nodiscard]] bool IsTestRunActive() const;
		// Fallback state for hosts without a window. A borrowed engine window supplies live focus/size/headless state.
		void SetScriptEnvironment(ScriptEnvironment environment);
		[[nodiscard]] ScriptEnvironment GetScriptEnvironment() const;

		[[nodiscard]] bool IsPlaying() const;
		// The running session; nullptr in Edit mode. Valid until Stop.
		[[nodiscard]] PlaySession* GetSession() const;

		// EditorApp's frame hooks (§4.2 steps 5 to 7): OnFixedStep once per loop step (PlaySession::AdvanceLoopStep),
		// OnUpdate once per frame (PlaySession::AdvanceLoopFrame) after the steps. No effect in Edit mode.
		void OnFixedStep();
		void OnUpdate(const FrameTime& frame);
		// After UI dispatch, automation handlers and pending-operation polls have returned. Applies queued ordinary-play
		// script reloads and consumes Quit through Stop; deterministic drivers keep ownership of their sessions.
		void OnSafePoint();
		// Asset publication only queues work: never enters a VM or drains further imports from a publication callback.
		void OnAssetReload(AssetHandle source);
		// The editor's physical-input ingress. Test/replay ownership and lockstep suppress device input; injected
		// simulation input uses PlayInput directly. Losing window focus releases the play cursor.
		void OnInputEvent(const Event& event, bool gameFocused);

		// The time scale the editor's frame loop applies (Application::SetFrameTimeScale): the session's, 1 in Edit mode.
		[[nodiscard]] double GetFrameTimeScale() const;
		// Whether the editor's loop runs unthrottled this frame (Application::SetFrameThrottleSuspended): while a play.step
		// runs (PlaySession::IsStepping, §4.2: lockstep is unthrottled); false in Edit mode.
		[[nodiscard]] bool IsFrameThrottleSuspended() const;
		// The loop the editor runs while a session exists (Application::SetFrameLoopConfig, FrameLoop::SetLoopConfig): the
		// session's project Simulation.FixedHz and MaxStepsPerFrame (PlaySession::GetProjectSettings) with the default
		// MaxFrameDelta, so play without lockstep runs at the project's rate in real time and frame deltas are the project's
		// FixedDelta (Docs/Decisions/0012-m7-decisions.md decision 3); nullopt in Edit mode, where the editor runs its own
		// ApplicationSpecification::Loop.
		[[nodiscard]] std::optional<FrameLoopConfig> GetFrameLoopConfig() const;

		// §13.2 "Disconnect": when `client` owns the session's lockstep, releases lockstep and pauses play (appending
		// PlayStateChanged "Paused"). No effect otherwise. AutomationServer calls it when a client disconnects.
		void OnClientDisconnected(ClientId client);

		// What "_meta" reports (§13.4, MetaState): "Edit", "Play", "Simulate" or "Paused", and the session's tick while
		// playing (nullopt in Edit mode).
		[[nodiscard]] std::string GetPlayStateName() const;
		[[nodiscard]] std::optional<uint64_t> GetTick() const;
	private:
		struct PreparedSession
		{
			Scope<PlaySession> Session{};
			ReplayHeader Header{};
			bool PausedDuringStartup = false; // During ordinary startup, before the launch pause option applies.
		};
		// The feature host builds scratch sessions without audio or publication; the real run acquires its lease first.
		// With activate false, returns an unactivated candidate with the launch flags configured; the replacement owner
		// retires its old session, installs and activates the candidate, then preserves pauses requested by startup callbacks.
		[[nodiscard]] Result<PreparedSession> PrepareSession(const PlayStartOptions& options, ProjectSettings project,
			bool emptyScene = false, IPlaySessionTestHook* hook = nullptr, IScriptTestHost* testHost = nullptr,
			bool ownsAudio = false, bool scratch = false, ScriptApiRegistry* api = nullptr, bool activate = true);
		[[nodiscard]] Status CheckTestAdmission(ClientId client) const;
		[[nodiscard]] Status CheckPlayScripts(const ProjectSettings& project);
		[[nodiscard]] Status ReplaceSession(const PlayStartOptions& options, ProjectSettings project);
		[[nodiscard]] Status AcquireTestRun(EditorFeatureTestHost& owner, ClientId client);
		void PublishTestSession(EditorFeatureTestHost& owner, PlaySession* session);
		void ReleaseTestRun(EditorFeatureTestHost& owner);
		friend class EditorFeatureTestHost;
	private:
		// The editor (a documented back-reference that owns the controller), the session and the deferral of reloads
		// (EditorPlayController.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
