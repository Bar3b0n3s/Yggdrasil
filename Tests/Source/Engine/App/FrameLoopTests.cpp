#include "TestsPCH.h"

#include "Engine/App/FrameLoop.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/DeathTest.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

namespace Engine {

	namespace {

		// Records every callback as text, and marks the events of `HandledKey` handled.
		class RecordingClient final : public IFrameLoopClient
		{
		public:
			void OnFrameEvent(Event& event) override
			{
				if (const KeyEvent* key = std::get_if<KeyEvent>(&event); key != nullptr && key->KeyCode == HandledKey)
					SetHandled(event);
				if (HandleClose && std::holds_alternative<WindowCloseEvent>(event))
					SetHandled(event);
				Calls.push_back(std::format("event {}", GetEventName(event)));
			}

			void OnFrameFixedStep(const SimStep& step) override
			{
				Calls.push_back(std::format("step {}", step.Tick));
			}

			void OnFrameUpdate(const FrameTime& frame) override
			{
				Calls.push_back(std::format("update {}", frame.FrameIndex));
			}
		public:
			std::vector<std::string> Calls;
			Key HandledKey = Key::None;
			bool HandleClose = false; // an editor that asks to save first
		};

		// Records the safe point next to the steps and updates (§4.2 step 3).
		class SafePointClient final : public IFrameLoopClient
		{
		public:
			void OnFrameEvent(Event& /*event*/) override {}

			void OnFrameSafePoint() override
			{
				Calls.push_back("safe point");
			}

			void OnFrameFixedStep(const SimStep& step) override
			{
				Calls.push_back(std::format("step {}", step.Tick));
			}

			void OnFrameUpdate(const FrameTime& frame) override
			{
				Calls.push_back(std::format("update {}", frame.FrameIndex));
			}
		public:
			std::vector<std::string> Calls;
		};

		// Crashes in its fixed step once told to.
		class CrashingClient final : public IFrameLoopClient
		{
		public:
			void OnFrameEvent(Event& /*event*/) override {}

			void OnFrameFixedStep(const SimStep& /*step*/) override
			{
				if (CrashInFixedStep)
					CrashHandler::SimulateCrash();
			}

			void OnFrameUpdate(const FrameTime& /*frame*/) override {}
		public:
			bool CrashInFixedStep = false;
		};

	}

	static constexpr double FixedDelta = 1.0 / 60.0;

	static Scope<EngineContext> CreateContext(std::optional<WindowSpecification> window = std::nullopt)
	{
		Result<Scope<EngineContext>> context = EngineContext::Create({ .WorkerCount = 0, .Window = std::move(window) });
		REQUIRE_MESSAGE(context.has_value(), context.error().ToString());
		return std::move(*context);
	}

	// Runs a headless frame, writes a fatal-error report after it, then runs a frame whose fixed step crashes: the child of
	// the frame-phase test, whose --user-data-dir puts both reports into the test's directory.
	ENGINE_DEATH_TEST("App/FrameLoopCrashesInAFixedStep")
	{
		Result<Scope<EngineContext>> context = EngineContext::Create({ .WorkerCount = 0 });
		if (!context.has_value())
		{
			ENGINE_CORE_ERROR("The frame-loop child cannot create its engine context: {}", context.error());
			return;
		}
		CrashingClient client;
		FrameLoop loop(**context, client, CreateScope<ManualClock>(FixedDelta), {});
		loop.RunFrame();
		const Result<std::filesystem::path> report = CrashHandler::WriteFatalErrorReport(FatalErrorKind::DeviceLost, "Between frames");
		ENGINE_CORE_WARN("Report between frames: [{}]", report.has_value() ? Test::PathToUtf8(*report) : report.error().ToString());
		client.CrashInFixedStep = true;
		loop.RunFrame();
	}

	TEST_SUITE("App")
	{
		TEST_CASE("FrameLoop: a ManualClock frame runs exactly one step before its update")
		{
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});

			for (int frame = 0; frame < 3; ++frame)
				loop.RunFrame();

			const std::vector<std::string> expected = { "step 0", "update 0", "step 1", "update 1", "step 2", "update 2" };
			CHECK(client.Calls == expected);
			CHECK(loop.GetFrameCount() == 3);
			CHECK(loop.GetScheduler().GetTick() == 3);
			CHECK(loop.GetLastFrameTime().Alpha == 1.0);
			CHECK(loop.GetLastFrameTime().DeltaTime == doctest::Approx(FixedDelta));
			CHECK(loop.GetClock().GetKind() == ClockKind::Manual);
			CHECK_FALSE(loop.IsExitRequested());
		}

		TEST_CASE("FrameLoop: the safe point runs once per frame after the queue drain and before the steps")
		{
			Scope<EngineContext> context = CreateContext();
			SafePointClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});
			SafePointClient* recorder = &client;
			context->GetMainThreadQueue().Post([recorder]()
			{
				recorder->Calls.push_back("drained");
			});

			loop.RunFrame();
			loop.RunFrame();

			const std::vector<std::string> expected = { "drained", "safe point", "step 0", "update 0", "safe point", "step 1", "update 1" };
			CHECK(client.Calls == expected);
		}

		TEST_CASE("FrameLoop: other clocks run as many steps as the scheduler gives")
		{
			// 0, 1, 3 and 5 steps (0.5 s is clamped to 0.25 s, and the step cap is 5).
			const std::array<double, 4> deltas = { 0.0, FixedDelta, 3.0 * FixedDelta, 0.5 };
			Result<ScriptedClock> clock = ScriptedClock::Create(deltas);
			REQUIRE(clock.has_value());

			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ScriptedClock>(std::move(*clock)), {});
			std::vector<size_t> stepsPerFrame;
			for (int frame = 0; frame < 4; ++frame)
			{
				client.Calls.clear();
				loop.RunFrame();
				stepsPerFrame.push_back(static_cast<size_t>(std::ranges::count_if(client.Calls, [](const std::string& call)
				{
					return call.starts_with("step ");
				})));
				CHECK(client.Calls.back() == std::format("update {}", frame));
			}
			const std::vector<size_t> expected = { 0, 1, 3, 5 };
			CHECK(stepsPerFrame == expected);
		}

		TEST_CASE("FrameLoop: MaxFrames ends Run with Success after that many frames")
		{
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), { .MaxFrames = 10 });
			CHECK(loop.Run() == ExitCode::Success);
			CHECK(loop.GetFrameCount() == 10);
			CHECK(std::ranges::count(client.Calls, std::string("update 9")) == 1);
		}

		TEST_CASE("FrameLoop: the first exit request wins and an early request runs no frame")
		{
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});
			loop.RequestExit(ExitCode::Timeout);
			loop.RequestExit(ExitCode::Failed);
			CHECK(loop.IsExitRequested());
			CHECK(loop.GetExitCode() == ExitCode::Timeout);
			CHECK(loop.Run() == ExitCode::Timeout);
			CHECK(loop.GetFrameCount() == 0);
			CHECK(client.Calls.empty());
		}

		TEST_CASE("FrameLoop: events reach the client first and only unhandled ones reach InputState")
		{
			Scope<EngineContext> context = CreateContext(WindowSpecification{ .Title = "Loop", .Width = 64, .Height = 64 });
			RecordingClient client;
			client.HandledKey = Key::H;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});

			Window& window = *context->GetWindow();
			window.InjectEvent(KeyEvent{ .KeyCode = Key::H, .Action = ButtonAction::Pressed });
			window.InjectEvent(KeyEvent{ .KeyCode = Key::U, .Action = ButtonAction::Pressed });
			loop.RunFrame();

			CHECK(std::ranges::count(client.Calls, std::string("event KeyEvent")) == 2);
			// The loop injects but never latches (§5.7: the simulation latches), so the test latches.
			context->GetInputState().LatchFrame();
			CHECK_FALSE(context->GetInputState().IsKeyDown(InputPhase::Frame, Key::H));
			CHECK(context->GetInputState().IsKeyDown(InputPhase::Frame, Key::U));
			CHECK_FALSE(loop.IsExitRequested());

			// An unhandled close ends the loop with Success after the current frame.
			window.InjectEvent(WindowCloseEvent{});
			loop.RunFrame();
			CHECK(loop.IsExitRequested());
			CHECK(loop.GetExitCode() == ExitCode::Success);
		}

		TEST_CASE("FrameLoop: a handled WindowCloseEvent does not end the loop")
		{
			Scope<EngineContext> context = CreateContext(WindowSpecification{ .Title = "Close", .Width = 64, .Height = 64 });
			RecordingClient client;
			client.HandleClose = true;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});
			context->GetWindow()->InjectEvent(WindowCloseEvent{});
			loop.RunFrame();
			CHECK(std::ranges::count(client.Calls, std::string("event WindowCloseEvent")) == 1);
			CHECK_FALSE(loop.IsExitRequested());
			CHECK(loop.GetExitCode() == ExitCode::Success);
		}

		TEST_CASE("FrameLoop: crash reports name the frame phase, which is cleared after each frame")
		{
			Test::TempDirectory directory("FramePhase");
			const ProcessSpecification specification = Test::MakeTestsChildSpecification({
				"--death-test=App/FrameLoopCrashesInAFixedStep",
				"--user-data-dir=" + Test::PathToUtf8(directory.GetPath()),
			});
			const Result<ProcessResult> child = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == ExitCode::Crash);

			// Between two frames the phase is cleared.
			const std::filesystem::path between = Test::PathFromUtf8(Test::FindBracketedValue(child->StandardError, "Report between frames: "));
			const Result<std::string> betweenReport = FileSystem::ReadText(between);
			REQUIRE_MESSAGE(betweenReport.has_value(), betweenReport.error().ToString());
			CHECK(betweenReport->contains("\nReason: Fatal error (DeviceLost): Between frames\n"));
			CHECK(betweenReport->contains("\n  FramePhase: (none)\n"));

			// The crash in the fixed step names the step phase.
			const std::vector<std::filesystem::path> reports =
				Test::ListCrashFiles(directory / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
			REQUIRE(reports.size() == 2);
			const std::filesystem::path& crash = reports[0] == between ? reports[1] : reports[0];
			const Result<std::string> crashReport = FileSystem::ReadText(crash);
			REQUIRE_MESSAGE(crashReport.has_value(), crashReport.error().ToString());
			CHECK(crashReport->contains("\n  FramePhase: FixedStep\n"));
		}

		TEST_CASE("FrameLoop: queued main-thread tasks run in each frame")
		{
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});
			std::vector<int> ran;
			context->GetMainThreadQueue().Post([&ran]()
			{
				ran.push_back(1);
			});
			context->GetMainThreadQueue().Post([&ran]()
			{
				ran.push_back(2);
			});
			loop.RunFrame();
			const std::vector<int> expected = { 1, 2 };
			CHECK(ran == expected);
			CHECK(context->GetMainThreadQueue().GetPendingCount() == 0);
		}

		// Runs only in a windowed child process (Support/WindowedChild.h): a native window, minimized, while the loop runs
		// about one second of frames on the system clock.
		TEST_CASE("FrameLoop: a minimized window uses little CPU time per second"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			Scope<EngineContext> context = CreateContext(WindowSpecification{ .Title = "Minimized", .Width = 320, .Height = 240 });
			Window& window = *context->GetWindow();
			window.Minimize();
			REQUIRE(Test::WaitUntilMinimized(window, true));

			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<SystemClock>(), { .MaxFrames = 60 });
			const auto wallStart = std::chrono::steady_clock::now();
			const Result<double> cpuStart = Process::GetCurrentCpuSeconds();
			REQUIRE(cpuStart.has_value());
			CHECK(loop.Run() == ExitCode::Success);
			const Result<double> cpuEnd = Process::GetCurrentCpuSeconds();
			REQUIRE(cpuEnd.has_value());
			const double wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();

			// 60 frames each waiting up to FixedDelta for an event take roughly a second; a spinning loop would use a full
			// core for it. The threshold leaves room for slow CI machines. This is the one test that reads the wall clock:
			// the Roadmap acceptance criterion is CPU time per wall-clock second (ADR 0005 decision 13).
			CAPTURE(wallSeconds);
			CAPTURE(*cpuEnd - *cpuStart);
			CHECK(wallSeconds > 0.5);
			CHECK((*cpuEnd - *cpuStart) / wallSeconds < 0.25);
		}

		TEST_CASE("FrameLoop: a minimized window idles")
		{
			ENGINE_CHECK_WINDOWED_CHILD("FrameLoop: a minimized window uses little CPU time per second");
		}
	}

}
