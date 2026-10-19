#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayPlayer.h"
#include "Engine/Testing/TestResults.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class AssetManager;
	class AudioEngine;
	class IPlaySessionTestHook;
	class IScriptTestHost;

	struct TestCaseDescription
	{
		std::string Name{};
		std::string File{};
		uint32_t Line = 0;
		uint32_t TimeoutTicks = 0;
	};

	struct TestSuiteDescription
	{
		std::string Name{};
		std::string Script{};
		std::string Scene{};
		std::vector<TestSuiteSettings::Mode> Modes{};
		std::vector<TestCaseDescription> Cases{};
	};

	struct TestInventory
	{
		std::vector<TestSuiteDescription> Suites{};
		std::vector<std::string> Replays{};
	};

	// What a successful FeatureTestRunner::Begin selected, in run order (FeatureTestRunner::GetSelection;
	// Docs/Decisions/0027-m17-contract.md decision 3, A2).
	struct FeatureTestSelection
	{
		// Each selected suite's index into the host's ProjectSettings::Testing.Suites, in settings order: a suite whose Modes
		// hold the run's mode, with a case whose "<suite>/<case>" the filter matches or, without cases, whose name it matches
		// (a case-insensitive substring; an empty filter matches everything).
		std::vector<size_t> Suites{};
		// Each replay the filter matches, as Discover lists it (its canonical project-relative path, in sorted order).
		std::vector<std::string> Replays{};
	};

	struct FeatureTestRunOptions
	{
		TestSuiteSettings::Mode Mode = TestSuiteSettings::Mode::Editor;
		std::string Filter{};
		// 0: no extra run-wide limit; otherwise counts actual fixed ticks across suites, cases, replays and record verification
		// (the input check included).
		// Case and suite limits still apply independently. Does not change a declared case TimeoutTicks.
		uint64_t TimeoutTicks = 0;
		// Exactly one selected suite. Publication requires fresh ordinary playback with Verify and StrictHash and, for a
		// recording with input events, an input check that fails (see Advance); there is no unverified-record option. The
		// adapter, not this class, owns the destination path and atomic file write.
		// The recorded suite runs with its Parameters in the replay header's form (CanonicalizeReplayParameters; Begin
		// fails with its located Validation error for a float out of the float range).
		bool Record = false;
	};

	// Services of the owning Editor/Runtime/test harness. This host is a documented borrowed back-reference that
	// outlives the runner and all sessions it creates. No automation context in Testing; no Testing type in Scripting.
	// An automation adapter acquires its exclusive run lease before constructing this host and preserves request
	// provenance until completed outputs have been written. A renderer-none host still supports every logic service.
	class IFeatureTestHost : public IReplayEvaluator
	{
	public:
		~IFeatureTestHost() override = default;
		[[nodiscard]] virtual const ProjectSettings& GetProjectSettings() const = 0;
		[[nodiscard]] virtual Result<AssetHandle> ResolveTestScript(std::string_view path) = 0;
		[[nodiscard]] virtual Result<std::vector<std::string>> ExpandReplayPaths(std::span<const std::string> globs) = 0;
		[[nodiscard]] virtual Result<AssetRef<ReplayData>> LoadReplay(std::string_view path) = 0;
		// Original scene identity and setup values of the active session, never the scene reached after Scene.Load.
		// Editor adapters delegate to AutomationMethodContext::DescribeReplayHeader; Runtime uses its asset metadata.
		[[nodiscard]] virtual Result<ReplayHeader> DescribeReplayHeader(const PlaySession& session) const = 0;
		// Strict scene/asset loading, fresh runtime scene and VM; omitted Scene means empty. Apply suite Parameters,
		// exact Overrides, effective seed, test mode, and OwnsAudioTime=true BEFORE callbacks/audio setup. ManualClock
		// suites are lockstep; Clock suites use ScriptedClock. Install testHost before OnCreate/OnStart and retain it
		// through replacement-VM setup and teardown. Audio decoding is Deterministic from engine construction, including
		// windowed Editor and a Runtime launched for tests; Device is independent. The caller owns the session exclusively.
		// The runner verifies that the returned session owns simulation audio time whenever GetAudioEngine is non-null and
		// fails the suite's setup otherwise. Each suite and replay session starts from the host's test-run cursor baseline
		// (CursorMode::Normal): a cursor mode a previous suite set never leaks into the next.
		[[nodiscard]] virtual Result<Scope<PlaySession>> CreateSuiteSession(const TestSuiteSettings& suite,
			TestSuiteSettings::Mode mode, IPlaySessionTestHook& testHook, IScriptTestHost& testHost) = 0;
		// Fresh ordinary gameplay VM: no suite collection, test driver or TestHost. OwnsAudioTime=true, lockstep and
		// live input suppressed, cursor at the test-run baseline. Verification cannot exit the application on Quit; the
		// host retains that request. A replay that cannot load, start or play becomes failed replay:<path> rows, and a
		// quit or fatal stop before its FinalTick a failed replay (ReplayPlaybackResult::StopReason); the run continues.
		// Every replay is played with StrictHash: the first checkpoint whose state hash differs ends it with a failed
		// "Checkpoint@<tick>" row (located at /Checkpoints/<index>/StateHash) naming both hashes.
		[[nodiscard]] virtual Result<Scope<PlaySession>> CreateReplaySession(const ReplayHeader& header, TestSuiteSettings::Mode mode) = 0;
		// Publishes a borrowed view for UI/automation reads. Null before destruction; the host never advances it from
		// its ordinary frame loop or lets another writer/driver take over during the runner's exclusive lease.
		virtual void SetActiveTestSession(PlaySession* session) = 0;
		[[nodiscard]] virtual AudioEngine* GetAudioEngine() const = 0;
		[[nodiscard]] virtual Status CaptureScreenshot(PlaySession& session, std::string_view name) = 0;
		[[nodiscard]] virtual Status ReloadScript(PlaySession& session, AssetHandle script) = 0;
		// The script API counters of gates 2, 3 and 4 for this run (Docs/Decisions/0022-m14-contract.md decision 4):
		// MakeScriptApiCoverageCounters over the parent's ScriptApiRegistry snapshot in the run's mode against the baseline
		// the host took immediately before FeatureTestRunner::Begin, identically in the Editor and the Runtime, with
		// RegistryFingerprint the XXH64 of the registry's generated declarations. Gates stays empty: the runner adds the
		// scene and format counters and evaluates the gates (see FeatureTestRunner).
		[[nodiscard]] virtual Result<TestCoverageReport> GetCoverage() const = 0;
		// M14: the asset manager every suite and replay session of this host loads through (the editor's
		// EditorAssetManager, the Runtime's RuntimeAssetManager), for the run's load observer (gate 9,
		// AssetManager::SetLoadObserver); a documented back-reference that outlives the runner. Null only for a host without
		// assets (unit-test hosts), in which case every format counter stays 0.
		[[nodiscard]] virtual AssetManager* GetAssetManager() const = 0;
	};

	// The view a script test's screenshot (IFeatureTestHost::CaptureScreenshot) extracts from `session` at `width` x `height`:
	// the primary camera with the session's project rendering quality (ProjectSettings.Rendering; ADR 0017: every capture
	// path observes it), so the capture matches the game view and viewport.screenshot. Shared by the Editor and Runtime.
	[[nodiscard]] RenderExtractionRequest MakeTestScreenshotRequest(const PlaySession& session, uint32_t width, uint32_t height);

	// Coverage (§15.5, §15.6; Docs/Decisions/0022-m14-contract.md decisions 2 to 6). When the host's project has
	// Testing.CoverageGates true, Begin selects every suite and replay (empty Filter) and Record is false, the run records
	// coverage: Begin resets a FormatCoverageRecorder and installs the load observer on host.GetAssetManager() (none for a
	// null manager). The observer records only the loads made inside Advance, the run's own work (session setup, ticks,
	// frames, captures, reloads, scene replacement); loads between two Advance calls, such as an editor viewport rendering
	// the published session or a thumbnail in a host frame, never count, because their number depends on how many host
	// frames the wall-clock budget lets run. No session path loads asynchronously (the script API's Assets.Load is
	// synchronous), so a LoadAsync publication, which happens at the host's main-thread drain, never counts either.
	// The SceneCoverageRecorder is created at the first recorded scene from its Scene::GetTypeRegistry()
	// (CoverageGates.h). Every suite and replay session's scene is recorded after setup, after each scene replacement and
	// before teardown (the recording-verification session is not). The observer is removed whenever the run ends: on
	// completion, on Cancel, when Begin fails after installing it, and in the destructor. The result's Coverage.Counters
	// are then the host's script counters plus the scene and format counters, sorted by id, and Coverage.Gates holds
	// EvaluateCoverageGates for options.Mode; Passed additionally requires every gate to pass. If recording or evaluating
	// fails (a recorder error, or EvaluateCoverageGates' Validation), the run fails: Gates is MakeUnevaluatedCoverageGates,
	// Passed is false and the error is logged at Error through the Engine logger, naming the mode. A filtered, recording or
	// cancelled run of such a project reports MakeUnevaluatedCoverageGates and its Passed ignores them. Without
	// Testing.CoverageGates, Counters holds the script counters only and Gates is empty.
	//
	// Shared runner core (§11.10), present in Dist. Owns sessions and clocks; uses ScriptEngine opaque references only.
	// The host adapter supplies exclusivity, disconnect handling and file output. No shell, compiler or RPC dependency.
	// Test capture uses existing AudioEngine capture/PulledFrames, anchored when the suite takes audio time. A capture
	// beginning in phase 4 of tick K measures only [round(K*48000/Hz), round((K+n)*48000/Hz)), half up with checked integer
	// arithmetic. Exclude pending earlier ticks and later samples of grouped frame pulls. IsAudioCaptureReady may delay
	// delivery until the next phase 4 after AudioUpdate; never pull audio again or reset the clock to satisfy a capture.
	class FeatureTestRunner
	{
	public:
		FeatureTestRunner();
		~FeatureTestRunner();
		FeatureTestRunner(const FeatureTestRunner&) = delete;
		FeatureTestRunner& operator=(const FeatureTestRunner&) = delete;

		// Collects in scratch sessions, releases all references/sessions and leaves no active session behind. Does not
		// execute cases. Settings order is preserved; replay globs are de-duplicated and sorted by canonical path.
		[[nodiscard]] static Result<TestInventory> Discover(IFeatureTestHost& host, TestSuiteSettings::Mode mode);
		[[nodiscard]] Status Begin(IFeatureTestHost& host, const FeatureTestRunOptions& options = {});
		// One bounded unit: a collection/teardown transition, one ManualClock tick, or one ScriptedClock frame (0..N
		// fixed steps + exactly one frame phase). true means complete. Host wall budgets may partition calls only.
		// Scripted-clock nonprogress is bounded separately: TimeoutTicks consecutive completed frames with no fixed
		// step time out the current case and end its suite. Reset this counter at case start and after any stepped frame.
		// Frame callbacks still run before the check; finite pauses shorter than this frame budget may recover normally.
		// Reported ticks and the existing case/suite/run tick budgets continue counting only actual simulation ticks.
		// Cases run in declaration order as separate resumable threads; Suite isolation shares one session, Case
		// isolation recollects in a fresh VM and identifies the selected case by declaration identity, never a stale handle.
		// Never start the next case until the current frame finishes, even when it contains several fixed steps. A case
		// requesting Scene.Load must return in that frame. In Suite isolation, re-require/recollect the same suite asset
		// in the replacement VM, validate the suite name and full ordered case identities/options, then continue at the
		// retained next index. Identity compares declaration ordinal/name/location and effective options, never handles.
		// A still-yielded case gets a located error with a next-case verification hint, never a stale resume. Recollection
		// mismatch is a located suite error. Results, budgets and counters survive; earlier cases are never executed again.
		// Case isolation still restarts the configured scene; Quit ends the suite without recollection. Approved M13 ADR
		// refinement (proposal section 9); no coroutine continuation crosses a VM lifetime.
		// With Record, retain every applied input in order and concatenate isolated sessions by cumulative tick offset,
		// without a reset opcode; each segment's state-hash checkpoints (ReplayRecorder::CaptureCheckpoint; a suite Clock that
		// runs several steps in one frame gets none at the ticks inside such a frame, which start from no completed-tick
		// boundary) move by the same offset. Keep the original starting header and observed terminal hash unchanged. Clock and Case
		// isolation have no blanket exclusion. After suite teardown, run a fresh normal-play session through ReplayPlayer
		// with Verify=true and StrictHash=true: no suite collection, TestHost, test tasks, overrides or injected snapshot.
		// Every checkpoint and the final hash must reproduce. A recording with input events then gets its input check
		// (M16 FX-4): another fresh session plays it with WithoutInput; when that playback passes too (every checkpoint and
		// the final hash reproduced without input), whatever script errors it raised, the recording's outcome does not depend
		// on its input and the run fails with Validation and a hint (end the recording while the played state is in the scene).
		// Publication requires passing selected cases, no invalidation, successful reproduction and a failed input check
		// (or no input events). Failure is Validation with the reason and expected/actual hash/tick; never substitute the
		// verifier's hash or omit an earlier segment. Verification and the input check are cancellable and count toward the
		// run-wide tick cap, but not case/suite ticks or coverage.
		[[nodiscard]] Result<bool> Advance();
		// Available after a terminal run, including a recording-verification error; preserves the original case results,
		// with Counts and FailedCases set from them (SummarizeTestRunCases).
		[[nodiscard]] Result<TestRunResult> GetResult() const;
		// Transfers the successful, strictly verified candidate once; InvalidState before verification, after failure or
		// cancellation, when Record was false, or after transfer. The adapter writes through its host with provenance only
		// after this succeeds. Cancellation, reproduction failure and write failure never replace the destination file.
		[[nodiscard]] Result<ReplayDocument> TakeRecording();
		// Where the input check of this run's verified recording ended (M16 friction S3-03): the tick of the first checkpoint
		// whose state hash differed without the input, else where that playback ended (the recording's final tick, or where
		// gameplay stopped). nullopt when no input check let a recording through: no recording, a recording without input
		// events (nothing to check), or a run that failed or was cancelled before. Kept after TakeRecording, until the next
		// Begin.
		[[nodiscard]] std::optional<uint64_t> GetInputCheckTick() const;
		// The selection of the last successful Begin (FeatureTestSelection), kept until the next Begin (through cancellation
		// and completion), so an adapter can check what the run will play before its first Advance and before it acquires its
		// lease: test.run refuses a dirty open scene that a selected suite or replay plays (Docs/Decisions/0027-m17-contract.md
		// decision 3, A2). InvalidState when no Begin has succeeded.
		[[nodiscard]] Result<FeatureTestSelection> GetSelection() const;
		// Idempotent. Cancel current case, EndSuite, stop captures, unpublish/destroy session, then release audio/time.
		// No Poll/Advance is required after cancellation; unwritten recordings are discarded.
		void Cancel();
		[[nodiscard]] bool IsRunning() const;
		[[nodiscard]] std::string GetPhase() const;
	private:
		struct State;
		Scope<State> m_State{};
	};

}
