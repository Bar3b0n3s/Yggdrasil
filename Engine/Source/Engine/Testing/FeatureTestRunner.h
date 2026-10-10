#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayPlayer.h"
#include "Engine/Testing/TestResults.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

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

	struct FeatureTestRunOptions
	{
		TestSuiteSettings::Mode Mode = TestSuiteSettings::Mode::Editor;
		std::string Filter{};
		// 0: no extra run-wide limit; otherwise counts actual fixed ticks across suites, cases, replays and record verification.
		// Case and suite limits still apply independently. Does not change a declared case TimeoutTicks.
		uint64_t TimeoutTicks = 0;
		// Exactly one selected suite. Publication requires fresh ordinary playback with Verify and StrictHash; there is
		// no unverified-record option. The adapter, not this class, owns the destination path and atomic file write.
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
		[[nodiscard]] virtual Result<Scope<PlaySession>> CreateSuiteSession(const TestSuiteSettings& suite,
			TestSuiteSettings::Mode mode, IPlaySessionTestHook& testHook, IScriptTestHost& testHost) = 0;
		// Fresh ordinary gameplay VM: no suite collection, test driver or TestHost. OwnsAudioTime=true, lockstep and
		// live input suppressed. Verification cannot exit the application on Quit; the host retains that request.
		[[nodiscard]] virtual Result<Scope<PlaySession>> CreateReplaySession(const ReplayHeader& header, TestSuiteSettings::Mode mode) = 0;
		// Publishes a borrowed view for UI/automation reads. Null before destruction; the host never advances it from
		// its ordinary frame loop or lets another writer/driver take over during the runner's exclusive lease.
		virtual void SetActiveTestSession(PlaySession* session) = 0;
		[[nodiscard]] virtual AudioEngine* GetAudioEngine() const = 0;
		[[nodiscard]] virtual Status CaptureScreenshot(PlaySession& session, std::string_view name) = 0;
		[[nodiscard]] virtual Status ReloadScript(PlaySession& session, AssetHandle script) = 0;
		// Run-local counter snapshot/delta, preserving stable ids and mode availability. M13 must not report unevaluated
		// M14 gates as passes. The host adapts the parent's ScriptApiRegistry snapshot plus other coverage producers.
		[[nodiscard]] virtual Result<TestCoverageReport> GetCoverage() const = 0;
	};

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
		// without a reset opcode. Keep the original starting header and observed terminal hash unchanged. Clock and Case
		// isolation have no blanket exclusion. After suite teardown, run a fresh normal-play session through ReplayPlayer
		// with Verify=true and StrictHash=true: no suite collection, TestHost, test tasks, overrides or injected snapshot.
		// Publication requires passing selected cases, no invalidation and successful reproduction. Failure is Validation
		// with the reason and expected/actual hash/tick; never substitute the verifier's hash or omit an earlier segment.
		// Verification is cancellable and counts toward the run-wide tick cap, but not case/suite ticks or coverage.
		[[nodiscard]] Result<bool> Advance();
		// Available after a terminal run, including a recording-verification error; preserves the original case results.
		[[nodiscard]] Result<TestRunResult> GetResult() const;
		// Transfers the successful, strictly verified candidate once; InvalidState before verification, after failure or
		// cancellation, when Record was false, or after transfer. The adapter writes through its host with provenance only
		// after this succeeds. Cancellation, reproduction failure and write failure never replace the destination file.
		[[nodiscard]] Result<ReplayDocument> TakeRecording();
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
