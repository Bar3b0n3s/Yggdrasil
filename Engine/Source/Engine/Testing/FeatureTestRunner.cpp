#include "EnginePCH.h"
#include "Engine/Testing/FeatureTestRunner.h"

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Clock.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptTestHost.h"
#include "Engine/Session/PlayInputEventCodec.h"
#include "Engine/Session/ReplayRecorder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

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

		Result<uint64_t> SampleBoundary(uint64_t tick, uint32_t hz)
		{
			const uint64_t whole = tick / hz, remainder = tick % hz;
			if (whole > (std::numeric_limits<uint64_t>::max() - AudioSampleRate) / AudioSampleRate)
				return MakeError(ErrorCode::InvalidArgument, "audio capture tick range overflows");
			return whole * AudioSampleRate + (remainder * AudioSampleRate + hz / 2) / hz;
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
		};
		struct Fault
		{
			ScriptError Error{};
			bool Fatal = false;
			bool Claimed = false;
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
		std::vector<Fault> Faults{};
		std::vector<TestFailureResult> OutsideFailures{};
		bool ActiveCase = false, TerminalCase = false, Teardown = false, Published = false;
		bool SuiteFailed = false, FatalStop = false, Recollect = false, RunLimitReached = false;
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

		~State() override { DestroySession(); }

		TestSuiteResult* SuiteResult()
		{
			return SuiteResultIndex < ReportData.Suites.size() ? &ReportData.Suites[SuiteResultIndex] : nullptr;
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
				AddFailure(Current, std::move(failure));
			else
				OutsideFailures.push_back(std::move(failure));
		}

		void OnScriptError(const ScriptError& error, bool fatal) override
		{
			if (auto* suite = SuiteResult())
				suite->ScriptErrors = Count(static_cast<uint64_t>(suite->ScriptErrors) + 1);
			FatalStop = FatalStop || fatal;
			if (ActiveCase && !Teardown)
				Faults.push_back({ error, fatal, false });
			else
				OutsideFailures.push_back({ error.Message, error.Script, error.Line, error.JsonPointer });
		}

		void OnExpectedScriptError(const ScriptError& error) override
		{
			if (!ActiveCase || Teardown)
				return;
			for (auto& fault : Faults)
				if (!fault.Fatal && !fault.Claimed && fault.Error.ID == error.ID && fault.Error.Script == error.Script && fault.Error.JsonPointer == error.JsonPointer && fault.Error.Line == error.Line && fault.Error.Message == error.Message)
				{
					fault.Claimed = true;
					return;
				}
		}

		Status InjectInput(const Json& event) override
		{
			if (!Session)
				return MakeError(ErrorCode::InvalidState, "test input needs an active session");
			ENGINE_TRY_ASSIGN(auto decoded, ParsePlayInputEvent(event, Session->GetInput().GetActions(), {}, false));
			return Session->GetInput().Queue(Session->GetInput().GetNextTick(), decoded.Event);
		}

		Status CaptureScreenshot(std::string_view name) override
		{
			if (!Session)
				return MakeError(ErrorCode::InvalidState, "test screenshot needs an active session");
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
			if (OutsideFailures.empty())
				return;
			TestCaseResult row;
			row.Suite = Collected.Name.empty() ? Selected[SuiteIndex].Description.Name : Collected.Name;
			row.Case = name;
			row.Status = "error";
			for (auto& failure : OutsideFailures)
				AddFailure(row, std::move(failure));
			OutsideFailures.clear();
			ReportData.Cases.push_back(std::move(row));
			SuiteFailed = true;
		}

		void DestroySession()
		{
			CancelAudioCapture();
			if (!Session)
				return;
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

		Status OpenSession()
		{
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
					return std::unexpected(Error(ErrorCode::Validation, "suite Clock cannot make simulation progress").WithLocation({ .File = {}, .JsonPointer = "/Testing/Suites/" + std::to_string(SuiteIndex) + "/Clock", .Entity = {} }));
				FrameClock = CreateScope<ScriptedClock>(std::move(clock));
				Session->SetLockstep(false);
			}
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
				if (!fault.Claimed)
				{
					Current.Status = "error";
					AddFailure(Current, { fault.Error.Message, fault.Error.Script, fault.Error.Line, fault.Error.JsonPointer });
				}
			Faults.clear();
			if (Quit && !QuitReported)
			{
				QuitReported = true;
				Current.Status = "quit";
				Current.QuitCode = *Quit;
				Current.QuitExpected = Selected[SuiteIndex].Settings.ExpectQuit == *Quit;
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
					Current.Status = "timeout";
					AddFailure(Current, { "suite or run tick limit exceeded", Resume.File, Resume.Line, {} });
					TerminalCase = true;
					FatalStop = true;
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
				if (Quit || FatalStop || CaseIndex >= Selected[SuiteIndex].Cases.size())
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
			return Player.Begin(std::move(data), *Session, { true, true });
		}

		Status CaptureCoverage()
		{
			ENGINE_TRY_ASSIGN(ReportData.Coverage, Host->GetCoverage());
			return {};
		}

		void Complete()
		{
			DestroySession();
			Player.Cancel();
			ReportData.Passed = !ReportData.Cancelled && std::all_of(ReportData.Cases.begin(), ReportData.Cases.end(), [](const auto& row)
			{
				return Passed(row);
			}) && std::all_of(ReportData.Suites.begin(), ReportData.Suites.end(), [](const auto& suite)
			{
				return suite.Passed;
			});
			CurrentPhase = Phase::Done;
		}

		Status ReplayStep(bool verification)
		{
			if (Options.TimeoutTicks && RunTicks >= Options.TimeoutTicks && Session->GetTick() < PlayingFinalTick)
				return MakeError(ErrorCode::Timeout, "run tick limit exceeded during replay");
			const auto tick = Session->GetTick();
			ENGINE_TRY_ASSIGN(bool complete, Player.Advance(*Session, *Host));
			RunTicks += Session->GetTick() - tick;
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
				if (!playback.Passed)
					return MakeError(ErrorCode::Validation, "recording cannot reproduce ordinary gameplay at tick {}: expected {}, observed {}", playback.FinalTick, playback.ExpectedStateHash, playback.StateHash);
				VerifiedRecording = true;
				Complete();
				return {};
			}
			const auto name = "replay:" + Replays[ReplayIndex];
			for (const auto& error : errors.GetErrors())
			{
				TestCaseResult row;
				row.Suite = name;
				row.Case = "<scripts>";
				row.Status = "error";
				row.Ticks = Count(error.Tick);
				AddFailure(row, { error.Message, error.Script, error.Line, error.JsonPointer });
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
			if (playback.Expect.empty() || !playback.HashMatched)
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
					Quit.reset();
					QuitReported = false;
					SuiteTicks = 0;
					CaseIndex = 0;
					OutsideFailures.clear();
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
						if (!Session)
						{
							ENGINE_TRY_ASSIGN(auto data, Host->LoadReplay(Replays[ReplayIndex]));
							ENGINE_TRY(BeginReplay(std::move(data)));
						}
						else
							ENGINE_TRY(ReplayStep(false));
						return false;
					}
					ENGINE_TRY(CaptureCoverage());
					if (!Options.Record)
					{
						Complete();
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
		for (const auto& settings : host.GetProjectSettings().Testing.Suites)
		{
			if (std::find(settings.Modes.begin(), settings.Modes.end(), options.Mode) == settings.Modes.end())
				continue;
			State::Selection selection;
			selection.Settings = settings;
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
		next->CurrentPhase = State::Phase::Suite;
		m_State = std::move(next);
		return {};
	}

	Result<bool> FeatureTestRunner::Advance()
	{
		auto result = m_State->Advance();
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
