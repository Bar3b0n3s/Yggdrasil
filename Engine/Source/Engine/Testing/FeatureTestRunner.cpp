#include "EnginePCH.h"
#include "Engine/Testing/FeatureTestRunner.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Clock.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Scripting/ScriptTestHost.h"
#include "Engine/Session/PlayInputEventCodec.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Engine/Testing/CoverageGates.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <utility>

namespace Engine {

	namespace {

		uint32_t Count(uint64_t value)
		{
			return static_cast<uint32_t>(std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
		}

		bool Matches(std::string_view text, std::string_view filter)
		{
			const auto lower = [](char c)
			{
				return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
			};
			return std::search(text.begin(), text.end(), filter.begin(), filter.end(), [lower](char a, char b)
			{
				return lower(a) == lower(b);
			}) != text.end()
				|| filter.empty();
		}

		bool Passed(const TestCaseResult& row)
		{
			return row.Failures.empty() && (row.Status == "passed" || row.Status == "skipped" || (row.Status == "quit" && row.QuitExpected));
		}

		void AddFailure(TestCaseResult& row, TestFailureResult failure)
		{
			if (row.Failures.empty())
			{
				row.Message = failure.Message;
				row.File = failure.File;
				row.Line = failure.Line;
				row.JsonPointer = failure.JsonPointer;
			}
			row.Failures.push_back(std::move(failure));
		}

		TestFailureResult Failure(const Error& error)
		{
			return { error.ToString(), error.GetLocation().File, error.GetLocation().Line, error.GetLocation().JsonPointer.value_or("") };
		}

		std::string_view RunModeName(TestSuiteSettings::Mode mode)
		{
			switch (mode)
			{
				case TestSuiteSettings::Mode::Editor:  return "Editor";
				case TestSuiteSettings::Mode::Release: return "Release";
				case TestSuiteSettings::Mode::Dist:    return "Dist";
			}
			return "Editor";
		}

		Result<uint64_t> SampleBoundary(uint64_t tick, uint32_t hz)
		{
			const uint64_t whole = tick / hz, remainder = tick % hz;
			if (whole > (std::numeric_limits<uint64_t>::max() - AudioSampleRate) / AudioSampleRate)
				return MakeError(ErrorCode::InvalidArgument, "audio capture tick range overflows");
			return whole * AudioSampleRate + (remainder * AudioSampleRate + hz / 2) / hz;
		}

	}

	namespace Utils {

		// Script text (names, Test.Fail messages, error messages) made safe for every report format.
		static void SanitizeFailure(TestFailureResult& failure)
		{
			failure.Message = SanitizeTestReportText(failure.Message);
			failure.File = SanitizeTestReportText(failure.File);
			failure.JsonPointer = SanitizeTestReportText(failure.JsonPointer);
		}

		static void SanitizeRow(TestCaseResult& row)
		{
			row.Suite = SanitizeTestReportText(row.Suite);
			row.Case = SanitizeTestReportText(row.Case);
			row.Message = SanitizeTestReportText(row.Message);
			row.File = SanitizeTestReportText(row.File);
			row.JsonPointer = SanitizeTestReportText(row.JsonPointer);
			for (TestFailureResult& failure : row.Failures)
				SanitizeFailure(failure);
		}

		// Ends a row whose script-driven failures went past a cap (ScriptLimits.h) with the count of those left out.
		static void AddOmittedFailures(TestCaseResult& row, uint64_t omitted)
		{
			if (omitted != 0)
				AddFailure(row, { std::format("[truncated: {} more failures]", omitted), {}, 0, {} });
		}

		static void SanitizeSuite(TestSuiteResult& suite)
		{
			suite.Suite = SanitizeTestReportText(suite.Suite);
			suite.Scene = SanitizeTestReportText(suite.Scene);
			suite.Script = SanitizeTestReportText(suite.Script);
		}

	}

	struct FeatureTestRunner::State final : IPlaySessionTestHook, IScriptTestHost
	{
		enum class Phase : uint8_t
		{
			Idle,
			Suite,
			Case,
			Frame,
			CloseSuite,
			Replay,
			Verify,
			Done
		};
		struct Selection
		{
			TestSuiteSettings Settings{};
			TestSuiteDescription Description{};
			std::vector<size_t> Cases{};
			size_t SettingsIndex = 0; // the suite's index in Testing.Suites, for located errors
		};
		// An unclaimed script error of the running case; a claim (OnExpectedScriptError) releases it.
		struct Fault
		{
			ScriptError Error{};
			bool Fatal = false;
		};
		IFeatureTestHost* Host = nullptr; // Borrowed for the complete run, including teardown.
		FeatureTestRunOptions Options{};
		TestRunResult ReportData{};
		std::vector<Selection> Selected{};
		std::vector<std::string> Replays{};
		size_t SuiteIndex = 0, CaseIndex = 0, ReplayIndex = 0;
		size_t SuiteResultIndex = 0;
		Scope<PlaySession> Session{};
		Scope<Clock> FrameClock{};
		Scope<FixedStepScheduler> Scheduler{};
		ScriptCollectedSuite Collected{};
		ScriptReference Thread{};
		ScriptCaseResume Resume{};
		TestCaseResult Current{};
		std::vector<Fault> Faults{}; // at most MaxScriptTestUnclaimedErrors
		std::vector<TestFailureResult> OutsideFailures{};
		// Script-driven failures past a cap (KeepScriptFailure), counted for their row's final truncation failure.
		uint64_t OmittedCaseFailures = 0, OmittedOutsideFailures = 0;
		bool OmittedCaseError = false;  // one of the case's omitted failures was an unexpected script error
		size_t SuiteScriptFailures = 0; // script-driven failures the current suite's rows keep
		// Test.Inject* events queued for the session's next input tick (MaxScriptInjectedInputsPerTick).
		uint64_t InjectedTick = 0;
		size_t InjectedCount = 0;
		size_t SuiteScreenshots = 0; // Test.Screenshot calls of the current suite (MaxScriptTestScreenshotsPerSuite)
		bool ActiveCase = false, TerminalCase = false, Teardown = false, Published = false;
		bool SuiteFailed = false, FatalStop = false, Recollect = false, RunLimitReached = false;
		bool LimitReached = false; // the suite or run tick limit ended the suite's stepping
		uint64_t RunTicks = 0, SuiteTicks = 0, CaseTicks = 0, FrameIndex = 0, Generation = 0;
		uint64_t AudioBaseline = 0, CaptureOrigin = 0, CaptureStart = 0, CaptureEnd = 0;
		bool Capturing = false;
		uint32_t NonprogressFrames = 0;
		bool QuitReported = false;
		std::vector<float> Captured{};
		std::optional<Error> CaptureError{};
		std::optional<int32_t> Quit{};
		Phase CurrentPhase = Phase::Idle;
		ReplayPlayer Player{};
		ReplayHeader PlayingHeader{};
		uint64_t PlayingFinalTick = 0;
		std::optional<ReplayDocument> Recording{};
		std::string RecordingError{};
		bool VerifiedRecording = false;
		// Coverage (FeatureTestRunner.h; Docs/Decisions/0022-m14-contract.md decisions 3 to 5). CoverageProject: the
		// project's Testing.CoverageGates. CoverageRun: also unfiltered and not recording, so scenes and loads are recorded
		// and a completed run evaluates the gates.
		bool CoverageProject = false, CoverageRun = false;
		Scope<SceneCoverageRecorder> SceneCoverage{}; // created at the first recorded scene, from its type registry
		Scope<FormatCoverageRecorder> FormatCoverage{};
		AssetManager* ObservedAssets = nullptr; // the host's manager while this run's load observer is installed
		// True only while FeatureTestRunner::Advance runs: the observer counts the loads of the run's own work and ignores
		// those between two calls (an editor viewport rendering the published session in a host frame), whose number
		// depends on how many host frames ran (decision 5).
		bool CountingLoads = false;
		std::optional<Error> CoverageError{}; // the first recording failure: the run fails with unevaluated gates
		uint64_t ReplayGeneration = 0;        // the replay session's scene generation last recorded

		~State() override
		{
			DestroySession();
			RemoveLoadObserver();
		}

		// Removes the load observer Begin installed on the host's asset manager (every end of a run, FeatureTestRunner.h).
		void RemoveLoadObserver()
		{
			if (ObservedAssets != nullptr)
				ObservedAssets->SetLoadObserver({});
			ObservedAssets = nullptr;
		}

		// Records the active session's scene for gates 1 and 1b (after setup, after each scene replacement and before
		// teardown). The recording-verification session never counts; after a failure nothing more is recorded.
		void RecordScene()
		{
			if (!CoverageRun || !Session || CoverageError || ReportData.Cancelled || CurrentPhase == Phase::Verify)
				return;
			const Scene& scene = Session->GetScene();
			if (!SceneCoverage)
				SceneCoverage = CreateScope<SceneCoverageRecorder>(scene.GetTypeRegistry());
			const Status recorded = SceneCoverage->Record(scene);
			if (!recorded)
				CoverageError = recorded.error();
		}

		// The gates of a completed run (`evaluate`) of a coverage project: the host's script counters plus the scene and
		// format counters, evaluated for the run's mode; MakeUnevaluatedCoverageGates for a filtered, recording, cancelled or
		// failed run. A recording or evaluation error fails the run and is logged.
		void FinishCoverage(bool evaluate)
		{
			if (!CoverageRun || !evaluate || ReportData.Cancelled)
			{
				ReportData.Coverage.Gates = MakeUnevaluatedCoverageGates();
				return;
			}
			const Status evaluated = [this]() -> Status
			{
				if (CoverageError)
					return std::unexpected(*CoverageError);
				std::vector<TestCoverageCounter>& counters = ReportData.Coverage.Counters;
				if (SceneCoverage)
					std::ranges::move(SceneCoverage->GetCounters(), std::back_inserter(counters));
				std::ranges::move(FormatCoverage->GetCounters(), std::back_inserter(counters));
				// A report lists only the entries of its own mode: never an EditorOnly one outside the editor. A counter that
				// breaks the mode rules stays, so the evaluation below reports it.
				std::erase_if(counters, [mode = Options.Mode](const TestCoverageCounter& counter)
				{
					const auto has = [&counter](TestSuiteSettings::Mode wanted)
					{
						return std::ranges::find(counter.Modes, wanted) != counter.Modes.end();
					};
					return has(TestSuiteSettings::Mode::Editor) && !has(mode);
				});
				std::ranges::sort(counters, {}, &TestCoverageCounter::Id);
				ENGINE_TRY_ASSIGN(ReportData.Coverage.Gates, EvaluateCoverageGates(counters, Options.Mode));
				return {};
			}();
			if (!evaluated)
			{
				ReportData.Coverage.Gates = MakeUnevaluatedCoverageGates();
				ReportData.Passed = false;
				ENGINE_CORE_ERROR("The coverage gates of the {} test run could not be evaluated: {}", RunModeName(Options.Mode), evaluated.error().ToString());
				return;
			}
			ReportData.Passed = ReportData.Passed && std::ranges::all_of(ReportData.Coverage.Gates, &TestCoverageGateResult::Passed);
		}

		TestSuiteResult* SuiteResult()
		{
			return SuiteResultIndex < ReportData.Suites.size() ? &ReportData.Suites[SuiteResultIndex] : nullptr;
		}

		// Whether a script-driven failure (Test.Expect*, an unexpected script error) joins a row that holds `kept`
		// failures: one place stays free for the row's truncation failure, and the suite's budget must allow it
		// (ScriptLimits.h). Otherwise it is counted in `omitted`.
		bool KeepScriptFailure(size_t kept, uint64_t& omitted)
		{
			if (kept + 1 < MaxScriptTestFailuresPerCase && SuiteScriptFailures < MaxScriptTestFailuresPerSuite)
			{
				++SuiteScriptFailures;
				return true;
			}
			++omitted;
			return false;
		}

		void Report(const ScriptTestReport& report) override
		{
			if (report.Signal == ScriptTestSignal::Skip)
			{
				if (ActiveCase)
				{
					Current.Status = "skipped";
					if (Current.Failures.empty())
					{
						Current.Message = report.Message;
						Current.File = report.File;
						Current.Line = report.Line;
					}
				}
				return;
			}
			TestFailureResult failure{ report.Message, report.File, report.Line, {} };
			if (ActiveCase)
			{
				if (KeepScriptFailure(Current.Failures.size(), OmittedCaseFailures))
					AddFailure(Current, std::move(failure));
			}
			else if (KeepScriptFailure(OutsideFailures.size(), OmittedOutsideFailures))
				OutsideFailures.push_back(std::move(failure));
		}

		void OnScriptError(const ScriptError& error, bool fatal) override
		{
			if (auto* suite = SuiteResult())
				suite->ScriptErrors = Count(static_cast<uint64_t>(suite->ScriptErrors) + 1);
			FatalStop = FatalStop || fatal;
			if (ActiveCase && !Teardown)
			{
				// A claim releases its fault, so the cap bounds the unclaimed ones; a later error cannot be claimed.
				if (Faults.size() < MaxScriptTestUnclaimedErrors)
					Faults.push_back({ error, fatal });
				else
				{
					++OmittedCaseFailures;
					OmittedCaseError = true;
				}
			}
			else if (KeepScriptFailure(OutsideFailures.size(), OmittedOutsideFailures))
				OutsideFailures.push_back({ error.Message, error.Script, error.Line, error.JsonPointer });
		}

		void OnExpectedScriptError(const ScriptError& error) override
		{
			if (!ActiveCase || Teardown)
				return;
			const auto claimed = std::ranges::find_if(Faults, [&error](const Fault& fault)
			{
				return !fault.Fatal && fault.Error.ID == error.ID && fault.Error.Script == error.Script && fault.Error.JsonPointer == error.JsonPointer
					&& fault.Error.Line == error.Line && fault.Error.Message == error.Message;
			});
			if (claimed != Faults.end())
				Faults.erase(claimed);
		}

		Status InjectInput(const Json& event) override
		{
			if (!Session)
				return MakeError(ErrorCode::InvalidState, "test input needs an active session");
			ENGINE_TRY_ASSIGN(auto decoded, ParsePlayInputEvent(event, Session->GetInput().GetActions(), {}, false));
			// The queue, the applied tick and a recording keep every injected event (ScriptLimits.h).
			const uint64_t tick = Session->GetInput().GetNextTick();
			if (InjectedTick != tick)
			{
				InjectedTick = tick;
				InjectedCount = 0;
			}
			if (InjectedCount >= MaxScriptInjectedInputsPerTick)
				return MakeError(ErrorCode::InvalidArgument, "Test.Inject* queues at most {} events per input tick", MaxScriptInjectedInputsPerTick);
			ENGINE_TRY(Session->GetInput().Queue(tick, decoded.Event));
			++InjectedCount;
			return {};
		}

		Status CaptureScreenshot(std::string_view name) override
		{
			if (!Session)
				return MakeError(ErrorCode::InvalidState, "test screenshot needs an active session");
			// A capture may write a new file, so a suite's captures are bounded (ScriptLimits.h); a refused one writes nothing.
			if (SuiteScreenshots >= MaxScriptTestScreenshotsPerSuite)
				return MakeError(ErrorCode::InvalidState, "Test.Screenshot takes at most {} screenshots per suite", MaxScriptTestScreenshotsPerSuite);
			++SuiteScreenshots;
			return Host->CaptureScreenshot(*Session, name);
		}

		Status BeginAudioCapture(uint32_t ticks) override
		{
			auto* audio = Host ? Host->GetAudioEngine() : nullptr;
			if (!Session || !audio || !Session->IsAudioTimeOwned())
				return MakeError(ErrorCode::InvalidState, "test audio capture requires owned simulation audio time");
			if (Capturing)
				return MakeError(ErrorCode::InvalidState, "an audio capture is already active");
			const uint64_t tick = Session->GetTick();
			if (ticks == 0 || tick > std::numeric_limits<uint64_t>::max() - ticks)
				return MakeError(ErrorCode::InvalidArgument, "audio capture ticks must be positive and within range");
			const auto hz = Session->GetProjectSettings().Simulation.FixedHz;
			ENGINE_TRY_ASSIGN(auto start, SampleBoundary(tick, hz));
			ENGINE_TRY_ASSIGN(auto end, SampleBoundary(tick + ticks, hz));
			if (end - start > AudioEngine::MaxCaptureFrames || AudioBaseline > std::numeric_limits<uint64_t>::max() - end)
				return MakeError(ErrorCode::InvalidArgument, "audio capture exceeds the sample limit");
			CaptureStart = AudioBaseline + start;
			CaptureEnd = AudioBaseline + end;
			CaptureOrigin = audio->GetStats().PulledFrames;
			Captured.clear();
			CaptureError.reset();
			Capturing = true;
			audio->StartCapture();
			return {};
		}

		void DrainCapture()
		{
			if (!Capturing)
				return;
			auto* audio = Host->GetAudioEngine();
			auto capture = audio->StopCapture();
			const uint64_t end = CaptureOrigin + capture.GetFrameCount();
			const uint64_t from = std::max(CaptureOrigin, CaptureStart), to = std::min(end, CaptureEnd);
			if (to > from)
			{
				const auto offset = static_cast<size_t>((from - CaptureOrigin) * AudioChannelCount);
				const auto length = static_cast<size_t>((to - from) * AudioChannelCount);
				Captured.insert(Captured.end(), capture.Samples.begin() + static_cast<ptrdiff_t>(offset), capture.Samples.begin() + static_cast<ptrdiff_t>(offset + length));
			}
			const auto pulled = audio->GetStats().PulledFrames;
			if (capture.Truncated && end < CaptureEnd && pulled > std::max(end, CaptureStart))
				CaptureError = Error(ErrorCode::Validation, "audio capture lost samples before its requested end");
			CaptureOrigin = pulled;
			audio->StartCapture();
		}

		bool IsAudioCaptureReady() const override
		{
			return Capturing && (CaptureError.has_value() || Host->GetAudioEngine()->GetStats().PulledFrames >= CaptureEnd);
		}

		Result<AudioLevels> EndAudioCapture() override
		{
			if (!IsAudioCaptureReady())
				return MakeError(ErrorCode::InvalidState, "audio capture is not ready");
			DrainCapture();
			if (CaptureError)
			{
				Error error = *CaptureError;
				CancelAudioCapture();
				return std::unexpected(std::move(error));
			}
			if (Captured.size() != (CaptureEnd - CaptureStart) * AudioChannelCount)
			{
				CancelAudioCapture();
				return MakeError(ErrorCode::Validation, "audio capture sample interval is incomplete");
			}
			const auto levels = MeasureAudioLevels(Captured);
			CancelAudioCapture();
			return levels;
		}

		void CancelAudioCapture() override
		{
			if (Capturing && Host && Host->GetAudioEngine())
				static_cast<void>(Host->GetAudioEngine()->StopCapture());
			Capturing = false;
			Captured.clear();
			CaptureError.reset();
		}

		Status ReloadScript(AssetHandle script) override
		{
			if (!Session)
				return MakeError(ErrorCode::InvalidState, "test reload needs an active session");
			return Host->ReloadScript(*Session, script);
		}

		uint64_t ComputeStateHash() const override { return Session ? Session->ComputeStateHash() : 0; }
		ScriptExtractionState GetLastExtraction() const override { return Session ? ScriptExtractionState{ Session->GetLastExtraction().Alpha, Session->GetExtractionCount() } : ScriptExtractionState{}; }
		std::optional<glm::vec3> GetExtractedPosition(UUID entity) const override
		{
			if (!Session)
				return std::nullopt;
			const auto& snapshot = Session->GetLastExtraction();
			if (snapshot.HasCamera && snapshot.Camera.Entity == entity)
				return snapshot.Camera.Position;
			for (const auto& mesh : snapshot.Meshes)
				if (mesh.Entity == entity)
					return glm::vec3(mesh.World[3]);
			for (const auto& text : snapshot.Texts)
				if (text.Entity == entity)
					return glm::vec3(text.World[3]);
			for (const auto& light : snapshot.Lights)
				if (light.Entity == entity)
					return light.Position;
			return std::nullopt;
		}
		void OnDebugBreak() override
		{
			if (auto* suite = SuiteResult())
				suite->Breaks = Count(static_cast<uint64_t>(suite->Breaks) + 1);
		}
		void OnQuit(int32_t code) override
		{
			if (!Quit)
				Quit = code;
		}

		void AfterTasks(PlaySession& session, const SimStep&) override
		{
			if (!ActiveCase || TerminalCase || Quit || FatalStop)
				return;
			auto* scripts = session.GetScripts();
			if (!scripts)
			{
				Current.Status = "error";
				AddFailure(Current, { "test VM is unavailable", Current.File, Current.Line, {} });
				TerminalCase = true;
				return;
			}
			auto resumed = scripts->ResumeCase(Thread);
			if (!resumed)
			{
				Current.Status = "error";
				AddFailure(Current, Failure(resumed.error()));
				TerminalCase = true;
			}
			else
			{
				Resume = std::move(*resumed);
				TerminalCase = Resume.State != ScriptCaseState::Yielded;
				if (Resume.State == ScriptCaseState::Failed)
				{
					Current.Status = Resume.Error ? "error" : "failed";
					// ScriptEngine has already forwarded a script fault through OnScriptError. Defer its
					// classification with the other occurrences; the resume result must not duplicate it.
					const bool reported = Resume.Error && std::any_of(Faults.begin(), Faults.end(), [this](const auto& fault)
					{
						return fault.Error.Script == Resume.Error->Script && fault.Error.Line == Resume.Error->Line
							&& fault.Error.JsonPointer == Resume.Error->JsonPointer && fault.Error.Message == Resume.Error->Message;
					});
					if (!reported && (Resume.Error || Current.Failures.empty()))
						AddFailure(Current, { Resume.Message, Resume.File, Resume.Line, Resume.Error ? Resume.Error->JsonPointer : "" });
				}
				else if (Resume.State == ScriptCaseState::Skipped)
				{
					Current.Status = "skipped";
					if (Current.Failures.empty())
					{
						Current.Message = Resume.Message;
						Current.File = Resume.File;
						Current.Line = Resume.Line;
					}
				}
			}
			if (TerminalCase)
			{
				scripts->CancelCase(Thread);
				CancelAudioCapture();
			}
		}

		void OutsideRow(std::string_view name)
		{
			if (OutsideFailures.empty() && OmittedOutsideFailures == 0)
				return;
			TestCaseResult row;
			row.Suite = Collected.Name.empty() ? Selected[SuiteIndex].Description.Name : Collected.Name;
			row.Case = name;
			row.Status = "error";
			for (auto& failure : OutsideFailures)
				AddFailure(row, std::move(failure));
			OutsideFailures.clear();
			Utils::AddOmittedFailures(row, std::exchange(OmittedOutsideFailures, 0));
			ReportData.Cases.push_back(std::move(row));
			SuiteFailed = true;
		}

		void DestroySession()
		{
			CancelAudioCapture();
			if (!Session)
				return;
			RecordScene();
			Teardown = true;
			if (auto* scripts = Session->GetScripts())
			{
				scripts->CancelCase(Thread);
				scripts->EndSuite();
			}
			Thread = {};
			Collected.Cases.clear();
			if (Published)
				Host->SetActiveTestSession(nullptr);
			Published = false;
			Session.reset();
			Scheduler.reset();
			FrameClock.reset();
			InjectedCount = 0;
			Teardown = false;
		}

		Status Collect(bool compare)
		{
			auto* scripts = Session->GetScripts();
			if (!scripts)
				return MakeError(ErrorCode::InvalidState, "suite session has no ScriptEngine");
			ENGINE_TRY_ASSIGN(auto asset, Host->ResolveTestScript(Selected[SuiteIndex].Settings.Script));
			ENGINE_TRY_ASSIGN(auto collected, scripts->CollectSuite(asset, *this));
			const auto& expected = Selected[SuiteIndex].Description;
			if (compare)
			{
				bool same = expected.Name == collected.Name && expected.Cases.size() == collected.Cases.size();
				for (size_t i = 0; same && i < collected.Cases.size(); ++i)
				{
					const auto& a = expected.Cases[i];
					const auto& b = collected.Cases[i];
					const uint32_t timeout = b.TimeoutTicks == 0 ? Host->GetProjectSettings().Testing.CaseTimeoutTicks : b.TimeoutTicks;
					same = a.Name == b.Name && a.File == b.File && a.Line == b.Line && a.TimeoutTicks == timeout;
				}
				if (!same)
					return std::unexpected(Error(ErrorCode::Validation, "suite recollection changed ordered case identities or effective options").WithLocation({ .File = collected.File, .Line = collected.Line, .JsonPointer = std::nullopt, .Entity = {} }));
			}
			Collected = std::move(collected);
			Generation = Session->GetSceneGeneration();
			Recollect = false;
			return {};
		}

		// §10.1, ADR 0015 decision 5: every suite and replay verification owns simulation audio time for its whole
		// lifetime (the host sets PlaySessionSpecification::OwnsAudioTime), ScriptedClock suites included, so voices never
		// advance on device time during a run. Checked against the session the host returned, not assumed.
		Status CheckAudioTime() const
		{
			if (Host->GetAudioEngine() != nullptr && !Session->IsAudioTimeOwned())
				return std::unexpected(Error(ErrorCode::InvalidState, "the host's test session does not own simulation audio time")
						.WithHint("IFeatureTestHost sessions set PlaySessionSpecification::OwnsAudioTime for every suite and replay"));
			return {};
		}

		Status OpenSession()
		{
			InjectedCount = 0;
			AudioBaseline = Host->GetAudioEngine() ? Host->GetAudioEngine()->GetStats().PulledFrames : 0;
			const auto& settings = Selected[SuiteIndex].Settings;
			ENGINE_TRY_ASSIGN(Session, Host->CreateSuiteSession(settings, Options.Mode, *this, *this));
			if (!Session)
				return MakeError(ErrorCode::InvalidState, "host returned an empty suite session");
			Host->SetActiveTestSession(Session.get());
			Published = true;
			if (!settings.Scene.empty())
			{
				ENGINE_TRY_ASSIGN(auto header, Host->DescribeReplayHeader(*Session));
				if (auto* suite = SuiteResult())
					suite->SceneId = header.Scene.Handle.ToString();
			}
			const auto& simulation = Session->GetProjectSettings().Simulation;
			FrameLoopConfig config;
			config.FixedHz = simulation.FixedHz;
			config.MaxStepsPerFrame = simulation.MaxStepsPerFrame;
			Scheduler = CreateScope<FixedStepScheduler>(config);
			if (settings.Clock.empty())
			{
				FrameClock = CreateScope<ManualClock>(Session->GetFixedDelta());
				Session->SetLockstep(true);
			}
			else
			{
				std::vector<double> deltas(settings.Clock.begin(), settings.Clock.end());
				ENGINE_TRY_ASSIGN(auto clock, ScriptedClock::Create(deltas));
				if (std::none_of(deltas.begin(), deltas.end(), [](double delta)
				{
					return delta > 0;
				}))
					return std::unexpected(Error(ErrorCode::Validation, "suite Clock cannot make simulation progress").WithLocation({ .File = {}, .JsonPointer = "/Testing/Suites/" + std::to_string(Selected[SuiteIndex].SettingsIndex) + "/Clock", .Entity = {} }));
				FrameClock = CreateScope<ScriptedClock>(std::move(clock));
				Session->SetLockstep(false);
			}
			ENGINE_TRY(CheckAudioTime());
			FrameIndex = 0;
			if (Options.Record && RecordingError.empty())
			{
				auto header = Host->DescribeReplayHeader(*Session);
				if (!header)
					RecordingError = header.error().ToString();
				else if (!Session->GetRecorder())
					RecordingError = "suite session has no replay recorder";
				else
				{
					auto begun = Session->GetRecorder()->Begin(*header, *Session);
					if (!begun)
						RecordingError = begun.error().ToString();
				}
			}
			ENGINE_TRY(Collect(true));
			if (auto quit = Session->GetQuitRequest())
				OnQuit(*quit);
			OutsideRow("<setup>");
			RecordScene();
			return {};
		}

		void FinishSegment()
		{
			if (!Session)
				return;
			if (auto* suite = SuiteResult())
				suite->FinalStateHash = UUID(Session->ComputeStateHash()).ToString();
			if (Options.Record && RecordingError.empty())
			{
				auto* recorder = Session->GetRecorder();
				auto finished = recorder ? recorder->Finish(*Session) : Result<ReplayDocument>(MakeError(ErrorCode::InvalidState, "recording session has no recorder"));
				if (!finished)
					RecordingError = finished.error().ToString();
				else if (!Recording)
					Recording = std::move(*finished);
				else
				{
					if (finished->FinalTick > std::numeric_limits<uint64_t>::max() - Recording->FinalTick)
						RecordingError = "recording tick offset overflow";
					else
					{
						for (auto& event : finished->Events)
						{
							event.Tick += Recording->FinalTick;
							Recording->Events.push_back(std::move(event));
						}
						Recording->FinalTick += finished->FinalTick;
						Recording->FinalStateHash = finished->FinalStateHash;
					}
				}
			}
			DestroySession();
			OutsideRow("<teardown>");
			QuitWithoutCase("<teardown>");
		}

		void FinishCase()
		{
			for (const auto& fault : Faults)
			{
				Current.Status = "error";
				if (KeepScriptFailure(Current.Failures.size(), OmittedCaseFailures))
					AddFailure(Current, { fault.Error.Message, fault.Error.Script, fault.Error.Line, fault.Error.JsonPointer });
			}
			Faults.clear();
			if (std::exchange(OmittedCaseError, false))
				Current.Status = "error";
			Utils::AddOmittedFailures(Current, std::exchange(OmittedCaseFailures, 0));
			if (Quit && !QuitReported)
			{
				QuitReported = true;
				Current.Status = "quit";
				Current.QuitCode = *Quit;
				// ExpectQuit < 0 means the suite must not quit at all, so even Application.Quit(-1) is unexpected.
				const int32_t expected = Selected[SuiteIndex].Settings.ExpectQuit;
				Current.QuitExpected = expected >= 0 && expected == *Quit;
				if (!Current.QuitExpected)
					AddFailure(Current, { std::format("unexpected quit({})", *Quit), Current.File, Current.Line, {} });
			}
			else if (!Current.Failures.empty() && (Current.Status == "passed" || Current.Status == "skipped"))
				Current.Status = "failed";
			Current.Ticks = Count(CaseTicks);
			SuiteFailed = SuiteFailed || !Passed(Current);
			ReportData.Cases.push_back(std::move(Current));
			if (Session && Session->GetScripts())
				Session->GetScripts()->CancelCase(Thread);
			CancelAudioCapture();
			Thread = {};
			ActiveCase = false;
			TerminalCase = false;
			++CaseIndex;
		}

		void QuitWithoutCase(std::string_view name = "<setup>")
		{
			if (!Quit || QuitReported)
				return;
			Current = {};
			Current.Suite = Selected[SuiteIndex].Description.Name;
			Current.Case = name;
			Current.File = Selected[SuiteIndex].Settings.Script;
			Current.Status = "passed";
			CaseTicks = 0;
			FinishCase();
		}

		Status StartNextCase()
		{
			if (Quit)
			{
				QuitWithoutCase();
				CurrentPhase = Phase::CloseSuite;
				return {};
			}
			if (FatalStop || CaseIndex >= Selected[SuiteIndex].Cases.size())
			{
				CurrentPhase = Phase::CloseSuite;
				return {};
			}
			if (!Session)
				ENGINE_TRY(OpenSession());
			if (Recollect)
			{
				ENGINE_TRY(Collect(true));
				OutsideRow("<setup>");
			}
			if (Quit || FatalStop)
			{
				if (Quit)
					QuitWithoutCase();
				CurrentPhase = Phase::CloseSuite;
				return {};
			}
			const auto index = Selected[SuiteIndex].Cases[CaseIndex];
			const auto& descriptor = Collected.Cases[index];
			Current = {};
			Current.Suite = Collected.Name;
			Current.Case = descriptor.Name;
			Current.Status = "passed";
			Current.File = descriptor.File;
			Current.Line = descriptor.Line;
			Resume = {};
			Resume.File = descriptor.File;
			Resume.Line = descriptor.Line;
			CaseTicks = 0;
			Faults.clear();
			OmittedCaseFailures = 0;
			OmittedCaseError = false;
			NonprogressFrames = 0;
			ENGINE_TRY_ASSIGN(Thread, Session->GetScripts()->StartCase(descriptor.Function));
			ActiveCase = true;
			TerminalCase = false;
			CurrentPhase = Phase::Frame;
			return {};
		}

		Status Frame()
		{
			const double delta = FrameClock->Delta();
			const bool manual = FrameClock->GetKind() == ClockKind::Manual;
			const double scale = manual ? 1.0 : Session->GetTimeScale();
			const auto steps = manual ? Scheduler->StepExactly(1) : Scheduler->Advance(delta, scale);
			const auto timeout = Selected[SuiteIndex].Description.Cases[Selected[SuiteIndex].Cases[CaseIndex]].TimeoutTicks;
			const auto suiteLimit = Host->GetProjectSettings().Testing.SuiteTickLimit;
			for (uint32_t i = 0; i < steps.StepCount; ++i)
			{
				if (SuiteTicks >= suiteLimit || (Options.TimeoutTicks && RunTicks >= Options.TimeoutTicks))
				{
					// Stepping stops at the limit. A case that already finished earlier in this frame keeps its result;
					// the cases the limit keeps from running are reported once the frame ends.
					if (!TerminalCase)
					{
						Current.Status = "timeout";
						AddFailure(Current, { "suite or run tick limit exceeded", Resume.File, Resume.Line, {} });
						TerminalCase = true;
						Session->GetScripts()->CancelCase(Thread);
						CancelAudioCapture();
					}
					LimitReached = true;
					RunLimitReached = Options.TimeoutTicks && RunTicks >= Options.TimeoutTicks;
					break;
				}
				if (!TerminalCase && CaseTicks >= timeout)
				{
					Current.Status = "timeout";
					AddFailure(Current, { "case tick timeout", Resume.File, Resume.Line, {} });
					TerminalCase = true;
					Session->GetScripts()->CancelCase(Thread);
					CancelAudioCapture();
				}
				const bool countingCase = !TerminalCase;
				Session->FixedStep();
				++RunTicks;
				++SuiteTicks;
				if (countingCase)
					++CaseTicks;
				if (!TerminalCase && CaseTicks >= timeout)
				{
					Current.Status = "timeout";
					AddFailure(Current, { "case tick timeout", Resume.File, Resume.Line, {} });
					TerminalCase = true;
					Session->GetScripts()->CancelCase(Thread);
					CancelAudioCapture();
				}
				if (Quit || FatalStop)
					break;
			}
			Session->FrameUpdate({ delta * scale, delta, steps.Alpha, FrameIndex++ });
			DrainCapture();
			if (auto quit = Session->GetQuitRequest())
				OnQuit(*quit);
			if (Session->GetSceneGeneration() != Generation)
			{
				if (!Recollect)
					RecordScene();
				Recollect = true;
				Thread = {};
				if (!TerminalCase && !Quit)
				{
					Current.Status = "error";
					AddFailure(Current, { "case yielded across Scene.Load; return in the requesting frame and verify the new scene in the next case", Resume.File, Resume.Line, {} });
					TerminalCase = true;
				}
			}
			if (FatalStop && !TerminalCase)
			{
				Current.Status = "error";
				AddFailure(Current, { "script VM stopped", Resume.File, Resume.Line, {} });
				TerminalCase = true;
			}
			// A scaled clock can stop producing ticks while every callback still returns within its watchdog.
			// Count completed frames, not wall time: a case gets TimeoutTicks consecutive frames without a step.
			NonprogressFrames = steps.StepCount == 0 ? NonprogressFrames + 1 : 0;
			if (!TerminalCase && !Quit && NonprogressFrames >= timeout)
			{
				Current.Status = "timeout";
				AddFailure(Current, { "scripted clock made no fixed-step progress within the case frame limit", Resume.File, Resume.Line, {} });
				TerminalCase = true;
				FatalStop = true;
				Session->GetScripts()->CancelCase(Thread);
				CancelAudioCapture();
			}
			if (TerminalCase || Quit)
			{
				FinishCase();
				if (LimitReached && !Quit && !FatalStop && CaseIndex < Selected[SuiteIndex].Cases.size())
				{
					// The limit leaves selected cases unrun: report it once, as a suite-level row.
					TestCaseResult row;
					row.Suite = Collected.Name;
					row.Case = "<tick limit>";
					row.Status = "timeout";
					row.Ticks = Count(SuiteTicks);
					AddFailure(row, { std::format("suite or run tick limit exceeded before {} more selected case(s) ran", Selected[SuiteIndex].Cases.size() - CaseIndex), Selected[SuiteIndex].Settings.Script, 0, {} });
					ReportData.Cases.push_back(std::move(row));
					SuiteFailed = true;
				}
				if (Quit || FatalStop || LimitReached || CaseIndex >= Selected[SuiteIndex].Cases.size())
					CurrentPhase = Phase::CloseSuite;
				else
				{
					if (Selected[SuiteIndex].Settings.Isolation == TestSuiteSettings::IsolationMode::Case)
						FinishSegment();
					CurrentPhase = Phase::Case;
				}
			}
			return {};
		}

		Status BeginReplay(AssetRef<ReplayData> data)
		{
			PlayingHeader = data->Header;
			PlayingFinalTick = data->FinalTick;
			ENGINE_TRY_ASSIGN(Session, Host->CreateReplaySession(data->Header, Options.Mode));
			if (!Session)
				return MakeError(ErrorCode::InvalidState, "host returned an empty replay session");
			Host->SetActiveTestSession(Session.get());
			Published = true;
			ENGINE_TRY(CheckAudioTime());
			ENGINE_TRY(Player.Begin(std::move(data), *Session, { true, true }));
			ReplayGeneration = Session->GetSceneGeneration();
			RecordScene();
			return {};
		}

		void AddReplayScriptErrorRows(const std::string& name)
		{
			for (const auto& error : Session->GetScriptErrors().GetErrors())
			{
				TestCaseResult row;
				row.Suite = name;
				row.Case = "<scripts>";
				row.Status = "error";
				row.Ticks = Count(error.Tick);
				AddFailure(row, { error.Message, error.Script, error.Line, error.JsonPointer });
				ReportData.Cases.push_back(std::move(row));
			}
		}

		// A replay that could not load, start or play becomes its own failed replay:<path> rows; the run continues with
		// the next replay (a run tick limit skips the rest, as it does for suites).
		void FailReplay(const Error& error)
		{
			const auto name = "replay:" + Replays[ReplayIndex];
			const bool runLimit = error.GetCode() == ErrorCode::Timeout && Options.TimeoutTicks && RunTicks >= Options.TimeoutTicks;
			TestSuiteResult suite;
			suite.Suite = name;
			suite.Scene = PlayingHeader.Scene.Path;
			suite.SceneId = PlayingHeader.Scene.Handle.IsValid() ? PlayingHeader.Scene.Handle.ToString() : std::string();
			suite.Mode = Options.Mode;
			if (Session)
			{
				if (auto* scripts = Session->GetScripts())
					scripts->Stop();
				AddReplayScriptErrorRows(name);
				suite.Ticks = Count(Session->GetTick());
				suite.ScriptErrors = Count(Session->GetScriptErrors().GetCursor());
			}
			TestCaseResult row;
			row.Suite = name;
			row.Case = "<replay>";
			row.Status = runLimit ? "timeout" : "error";
			row.Ticks = suite.Ticks;
			AddFailure(row, Failure(error));
			if (row.File.empty())
				row.File = Replays[ReplayIndex];
			ReportData.Cases.push_back(std::move(row));
			suite.Passed = false;
			ReportData.Suites.push_back(std::move(suite));
			DestroySession();
			Player.Cancel();
			++ReplayIndex;
			if (runLimit)
				ReplayIndex = Replays.size();
		}

		Status StartReplay()
		{
			PlayingHeader = {};
			PlayingFinalTick = 0;
			ENGINE_TRY_ASSIGN(auto data, Host->LoadReplay(Replays[ReplayIndex]));
			return BeginReplay(std::move(data));
		}

		Status CaptureCoverage()
		{
			ENGINE_TRY_ASSIGN(ReportData.Coverage, Host->GetCoverage());
			return {};
		}

		// Ends the run. `evaluate`: it completed normally, so a coverage run evaluates its gates (FinishCoverage).
		void Complete(bool evaluate = false)
		{
			DestroySession();
			Player.Cancel();
			RemoveLoadObserver();
			// Rows carry script-supplied text; JSON, JUnit and the RPC result must never fail on it (§11.10).
			for (TestCaseResult& row : ReportData.Cases)
				Utils::SanitizeRow(row);
			for (TestSuiteResult& suite : ReportData.Suites)
				Utils::SanitizeSuite(suite);
			ReportData.Passed = !ReportData.Cancelled && std::all_of(ReportData.Cases.begin(), ReportData.Cases.end(), [](const auto& row)
			{
				return Passed(row);
			}) && std::all_of(ReportData.Suites.begin(), ReportData.Suites.end(), [](const auto& suite)
			{
				return suite.Passed;
			});
			if (CoverageProject)
				FinishCoverage(evaluate);
			CurrentPhase = Phase::Done;
		}

		Status ReplayStep(bool verification)
		{
			if (Options.TimeoutTicks && RunTicks >= Options.TimeoutTicks && Session->GetTick() < PlayingFinalTick)
				return MakeError(ErrorCode::Timeout, "run tick limit exceeded during replay");
			const auto tick = Session->GetTick();
			ENGINE_TRY_ASSIGN(bool complete, Player.Advance(*Session, *Host));
			RunTicks += Session->GetTick() - tick;
			if (Session->GetSceneGeneration() != ReplayGeneration)
			{
				ReplayGeneration = Session->GetSceneGeneration();
				RecordScene();
			}
			if (!complete)
				return {};
			ENGINE_TRY_ASSIGN(auto playback, Player.GetResult());
			// Teardown can report faults too. Stop while the session-persistent history is still available;
			// the eventual session destructor observes the already-stopped engine.
			if (auto* scripts = Session->GetScripts())
				scripts->Stop();
			const auto& errors = Session->GetScriptErrors();
			if (verification)
			{
				const bool scriptFailed = errors.GetCursor() != 0;
				DestroySession();
				if (scriptFailed)
					return MakeError(ErrorCode::Validation, "recording ordinary-play verification reported script errors");
				if (!playback.StopReason.empty())
					return MakeError(ErrorCode::Validation, "recording cannot reproduce ordinary gameplay: {}", playback.StopReason);
				if (!playback.Passed)
					return MakeError(ErrorCode::Validation, "recording cannot reproduce ordinary gameplay at tick {}: expected {}, observed {}", playback.FinalTick, playback.ExpectedStateHash, playback.StateHash);
				VerifiedRecording = true;
				Complete();
				return {};
			}
			const auto name = "replay:" + Replays[ReplayIndex];
			AddReplayScriptErrorRows(name);
			if (!playback.StopReason.empty())
			{
				// Gameplay quit or stopped fatally before FinalTick: the replay failed where it ended.
				TestCaseResult row;
				row.Suite = name;
				row.Case = "<stopped>";
				row.Status = "error";
				row.Ticks = Count(playback.FinalTick);
				AddFailure(row, { playback.StopReason, Replays[ReplayIndex], 0, "/FinalTick" });
				ReportData.Cases.push_back(std::move(row));
			}
			for (const auto& expectation : playback.Expect)
			{
				TestCaseResult row;
				row.Suite = name;
				row.Case = std::format("Expect@{}", expectation.Tick);
				row.Status = expectation.Satisfied ? "passed" : "failed";
				row.Ticks = Count(expectation.Tick);
				if (expectation.Failure)
				{
					AddFailure(row, Failure(*expectation.Failure));
					if (expectation.Failure->GetCode() != ErrorCode::Validation)
						row.Status = "error";
				}
				ReportData.Cases.push_back(std::move(row));
			}
			if (playback.StopReason.empty() && (playback.Expect.empty() || !playback.HashMatched))
			{
				TestCaseResult row;
				row.Suite = name;
				row.Case = "FinalStateHash";
				row.Status = playback.HashMatched ? "passed" : "failed";
				row.Ticks = Count(playback.FinalTick);
				if (!playback.HashMatched)
					AddFailure(row, { std::format("expected {}, observed {}", playback.ExpectedStateHash, playback.StateHash), Replays[ReplayIndex], 0, "/FinalStateHash" });
				ReportData.Cases.push_back(std::move(row));
			}
			TestSuiteResult suite;
			suite.Suite = name;
			suite.Scene = PlayingHeader.Scene.Path;
			suite.SceneId = PlayingHeader.Scene.Handle.ToString();
			suite.Mode = Options.Mode;
			suite.Ticks = Count(playback.FinalTick);
			suite.FinalStateHash = playback.StateHash;
			suite.ScriptErrors = Count(errors.GetCursor());
			suite.Passed = playback.Passed && suite.ScriptErrors == 0;
			ReportData.Suites.push_back(std::move(suite));
			DestroySession();
			Player.Cancel();
			++ReplayIndex;
			return {};
		}

		Result<bool> Advance()
		{
			switch (CurrentPhase)
			{
				case Phase::Idle: return MakeError(ErrorCode::InvalidState, "test run has not begun");
				case Phase::Done: return true;
				case Phase::Suite:
				{
					if (SuiteIndex >= Selected.size())
					{
						CurrentPhase = Phase::Replay;
						return false;
					}
					SuiteFailed = false;
					FatalStop = false;
					LimitReached = false;
					Quit.reset();
					QuitReported = false;
					SuiteTicks = 0;
					CaseIndex = 0;
					OutsideFailures.clear();
					OmittedOutsideFailures = 0;
					SuiteScriptFailures = 0;
					SuiteScreenshots = 0;
					Collected = {};
					Recollect = false;
					SuiteResultIndex = ReportData.Suites.size();
					TestSuiteResult suite;
					suite.Suite = Selected[SuiteIndex].Description.Name;
					suite.Script = Selected[SuiteIndex].Settings.Script;
					suite.Scene = Selected[SuiteIndex].Settings.Scene;
					suite.Mode = Options.Mode;
					ReportData.Suites.push_back(std::move(suite));
					const auto opened = OpenSession();
					if (!opened)
					{
						OutsideFailures.push_back(Failure(opened.error()));
						OutsideRow("<setup>");
						CurrentPhase = Phase::CloseSuite;
					}
					else
						CurrentPhase = Phase::Case;
					return false;
				}
				case Phase::Case:
				{
					const auto started = StartNextCase();
					if (!started)
					{
						OutsideFailures.push_back(Failure(started.error()));
						OutsideRow("<setup>");
						CurrentPhase = Phase::CloseSuite;
					}
					return false;
				}
				case Phase::Frame: ENGINE_TRY(Frame()); return false;
				case Phase::CloseSuite:
				{
					FinishSegment();
					if (Selected[SuiteIndex].Settings.ExpectQuit >= 0 && !Quit)
					{
						TestCaseResult row;
						row.Suite = Selected[SuiteIndex].Description.Name;
						row.Case = "<quit>";
						row.Status = "failed";
						AddFailure(row, { "suite did not request its expected quit code", Selected[SuiteIndex].Settings.Script, 0, {} });
						ReportData.Cases.push_back(std::move(row));
						SuiteFailed = true;
					}
					auto& suite = ReportData.Suites[SuiteResultIndex];
					suite.Ticks = Count(SuiteTicks);
					suite.Passed = !SuiteFailed;
					++SuiteIndex;
					if (RunLimitReached)
					{
						SuiteIndex = Selected.size();
						ReplayIndex = Replays.size();
					}
					CurrentPhase = Phase::Suite;
					return false;
				}
				case Phase::Replay:
				{
					if (ReplayIndex < Replays.size())
					{
						const Status stepped = Session ? ReplayStep(false) : StartReplay();
						if (!stepped)
							FailReplay(stepped.error());
						return false;
					}
					ENGINE_TRY(CaptureCoverage());
					if (!Options.Record)
					{
						Complete(true);
						return true;
					}
					if (!RecordingError.empty())
						return MakeError(ErrorCode::Validation, "recording invalidated: {}", RecordingError);
					if (!Recording || std::any_of(ReportData.Cases.begin(), ReportData.Cases.end(), [](const auto& row)
					{
						return !Passed(row);
					}))
						return MakeError(ErrorCode::Validation, "recording requires passing selected cases and a valid starting scene");
					ENGINE_TRY(ValidateReplayDocument(*Recording));
					CurrentPhase = Phase::Verify;
					return false;
				}
				case Phase::Verify:
				{
					if (!Session)
					{
						auto data = CreateRef<ReplayData>();
						data->Header = Recording->Header;
						data->Events = Recording->Events;
						data->FinalTick = Recording->FinalTick;
						data->FinalStateHash = Recording->FinalStateHash;
						ENGINE_TRY(BeginReplay(std::move(data)));
					}
					else
						ENGINE_TRY(ReplayStep(true));
					return CurrentPhase == Phase::Done;
				}
			}
			return false;
		}
	};

	RenderExtractionRequest MakeTestScreenshotRequest(const PlaySession& session, uint32_t width, uint32_t height)
	{
		const RenderingSettings& rendering = session.GetProjectSettings().Rendering;
		RenderExtractionRequest request;
		request.Camera = RenderCameraSource::Primary;
		request.Width = width;
		request.Height = height;
		request.Quality = { .ShadowMapSize = rendering.ShadowMapSize, .SsaoHalfResolution = rendering.SsaoHalfResolution };
		return request;
	}

	FeatureTestRunner::FeatureTestRunner()
		: m_State(CreateScope<State>())
	{
	}
	FeatureTestRunner::~FeatureTestRunner() = default;

	Result<TestInventory> FeatureTestRunner::Discover(IFeatureTestHost& host, TestSuiteSettings::Mode mode)
	{
		if (mode != TestSuiteSettings::Mode::Editor && mode != TestSuiteSettings::Mode::Release && mode != TestSuiteSettings::Mode::Dist)
			return MakeError(ErrorCode::InvalidArgument, "test discovery requires Editor, Release or Dist mode");
		TestInventory inventory;
		for (const auto& settings : host.GetProjectSettings().Testing.Suites)
		{
			if (std::find(settings.Modes.begin(), settings.Modes.end(), mode) == settings.Modes.end())
				continue;
			State scratch;
			scratch.Host = &host;
			ENGINE_TRY_ASSIGN(scratch.Session, host.CreateSuiteSession(settings, mode, scratch, scratch));
			if (!scratch.Session || !scratch.Session->GetScripts())
				return MakeError(ErrorCode::InvalidState, "suite discovery needs a script session");
			ENGINE_TRY_ASSIGN(auto asset, host.ResolveTestScript(settings.Script));
			ENGINE_TRY_ASSIGN(auto collected, scratch.Session->GetScripts()->CollectSuite(asset, scratch));
			TestSuiteDescription description;
			description.Name = collected.Name;
			description.Script = settings.Script;
			description.Scene = settings.Scene;
			description.Modes = settings.Modes;
			for (const auto& item : collected.Cases)
				description.Cases.push_back({ item.Name, item.File, item.Line, item.TimeoutTicks == 0 ? host.GetProjectSettings().Testing.CaseTimeoutTicks : item.TimeoutTicks });
			inventory.Suites.push_back(std::move(description));
		}
		ENGINE_TRY_ASSIGN(inventory.Replays, host.ExpandReplayPaths(host.GetProjectSettings().Testing.Replays));
		std::sort(inventory.Replays.begin(), inventory.Replays.end());
		inventory.Replays.erase(std::unique(inventory.Replays.begin(), inventory.Replays.end()), inventory.Replays.end());
		return inventory;
	}

	Status FeatureTestRunner::Begin(IFeatureTestHost& host, const FeatureTestRunOptions& options)
	{
		if (IsRunning())
			return MakeError(ErrorCode::InvalidState, "test run is already active");
		if (host.GetProjectSettings().Testing.CaseTimeoutTicks == 0 || host.GetProjectSettings().Testing.SuiteTickLimit == 0)
			return MakeError(ErrorCode::InvalidArgument, "case timeout and suite tick limit must be positive");
		ENGINE_TRY_ASSIGN(auto inventory, Discover(host, options.Mode));
		auto next = CreateScope<State>();
		next->Host = &host;
		next->Options = options;
		next->ReportData.Mode = options.Mode;
		size_t inventoryIndex = 0;
		const auto& suites = host.GetProjectSettings().Testing.Suites;
		for (size_t settingsIndex = 0; settingsIndex < suites.size(); ++settingsIndex)
		{
			const TestSuiteSettings& settings = suites[settingsIndex];
			if (std::find(settings.Modes.begin(), settings.Modes.end(), options.Mode) == settings.Modes.end())
				continue;
			State::Selection selection;
			selection.Settings = settings;
			selection.SettingsIndex = settingsIndex;
			selection.Description = inventory.Suites[inventoryIndex++];
			for (size_t i = 0; i < selection.Description.Cases.size(); ++i)
				if (Matches(selection.Description.Name + "/" + selection.Description.Cases[i].Name, options.Filter))
					selection.Cases.push_back(i);
			if (!selection.Cases.empty() || (selection.Description.Cases.empty() && Matches(selection.Description.Name, options.Filter)))
				next->Selected.push_back(std::move(selection));
		}
		for (const auto& path : inventory.Replays)
			if (Matches(path, options.Filter))
				next->Replays.push_back(path);
		if (options.Record && next->Selected.size() != 1)
			return MakeError(ErrorCode::InvalidArgument, "recording requires exactly one selected suite");
		if (options.Record)
		{
			// The recorded suite runs with its parameters in the replay header's form (floats as float, §6.7), so the
			// verifying replay and every later one read exactly the values the recorded run read.
			State::Selection& recorded = next->Selected.front();
			const Json parameters = recorded.Settings.Parameters.IsNull() ? Json::object() : recorded.Settings.Parameters.Get();
			ENGINE_TRY_ASSIGN(const Json canonical, CanonicalizeReplayParameters(parameters, std::format("/Testing/Suites/{}/Parameters", recorded.SettingsIndex)));
			recorded.Settings.Parameters = VariantValue(canonical);
		}
		next->CoverageProject = host.GetProjectSettings().Testing.CoverageGates;
		next->CoverageRun = next->CoverageProject && options.Filter.empty() && !options.Record;
		if (next->CoverageRun)
		{
			// Installed last, so nothing in Begin can fail after it; a State that ends any other way removes it.
			next->FormatCoverage = CreateScope<FormatCoverageRecorder>();
			if (AssetManager* assets = host.GetAssetManager())
			{
				assets->SetLoadObserver([recorder = next->FormatCoverage.get(), assets, counting = &next->CountingLoads](AssetHandle handle)
				{
					if (*counting)
						recorder->RecordLoad(*assets, handle);
				});
				next->ObservedAssets = assets;
			}
		}
		next->CurrentPhase = State::Phase::Suite;
		m_State = std::move(next);
		return {};
	}

	Result<bool> FeatureTestRunner::Advance()
	{
		// Gate 9 counts only the loads made inside this call: a host frame between two calls (the editor's viewports render
		// the published session, its thumbnails load) never counts, so no format count depends on the wall-clock budget
		// that partitions the calls into host frames (FeatureTestRunner.h).
		m_State->CountingLoads = true;
		auto result = m_State->Advance();
		m_State->CountingLoads = false;
		if (!result && m_State->CurrentPhase != State::Phase::Idle)
		{
			m_State->VerifiedRecording = false;
			m_State->Recording.reset();
			m_State->Complete();
			m_State->ReportData.Passed = false;
		}
		return result;
	}

	Result<TestRunResult> FeatureTestRunner::GetResult() const
	{
		if (m_State->CurrentPhase != State::Phase::Done)
			return MakeError(ErrorCode::InvalidState, "test run has not completed");
		return m_State->ReportData;
	}

	Result<ReplayDocument> FeatureTestRunner::TakeRecording()
	{
		if (!m_State->VerifiedRecording || !m_State->Recording || m_State->ReportData.Cancelled)
			return MakeError(ErrorCode::InvalidState, "no verified recording is available");
		auto recording = std::move(*m_State->Recording);
		m_State->Recording.reset();
		m_State->VerifiedRecording = false;
		return recording;
	}

	void FeatureTestRunner::Cancel()
	{
		if (!IsRunning())
			return;
		m_State->ReportData.Cancelled = true;
		m_State->VerifiedRecording = false;
		m_State->Recording.reset();
		m_State->Complete();
	}

	bool FeatureTestRunner::IsRunning() const
	{
		return m_State->CurrentPhase != State::Phase::Idle && m_State->CurrentPhase != State::Phase::Done;
	}

	std::string FeatureTestRunner::GetPhase() const
	{
		switch (m_State->CurrentPhase)
		{
			case State::Phase::Idle:       return "idle";
			case State::Phase::Suite:      return "collect suite";
			case State::Phase::Case:       return "start case";
			case State::Phase::Frame:      return "run case";
			case State::Phase::CloseSuite: return "teardown suite";
			case State::Phase::Replay:     return "replay";
			case State::Phase::Verify:     return "verify recording";
			case State::Phase::Done:       return "complete";
		}
		return "idle";
	}

}
