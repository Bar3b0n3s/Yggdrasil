#include "TestsPCH.h"

#include "Engine/App/FrameLoop.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/DeathTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <array>

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

			void OnFrameRender(const FrameTime& frame) override
			{
				Calls.push_back(std::format("render {}", frame.FrameIndex));
			}
		public:
			std::vector<std::string> Calls;
			Key HandledKey = Key::None;
			bool HandleClose = false; // an editor that asks to save first
		};

		// Records the safe point next to the steps, updates and renders (§4.2 step 3).
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

			void OnFrameRender(const FrameTime& frame) override
			{
				Calls.push_back(std::format("render {}", frame.FrameIndex));
			}
		public:
			std::vector<std::string> Calls;
		};

		// Makes vulkan.hpp throw, as NVRHI's internal calls do on device loss or out-of-memory (§4.6 item 2): creating an
		// instance with a layer that does not exist throws vk::LayerNotPresentError.
		void ThrowSystemError()
		{
			const std::array<const char*, 1> layers = { "VK_LAYER_ENGINE_does_not_exist" };
			const vk::ApplicationInfo application("FrameLoopTests", 1, "Engine", 1, VK_API_VERSION_1_3);
			const vk::InstanceCreateInfo createInfo({}, &application, layers);
			const vk::Instance instance = vk::createInstance(createInfo);
			ENGINE_CORE_ERROR("vk::createInstance with a missing layer returned instead of throwing");
			instance.destroy();
		}

		// Throws a vk::SystemError from its update or its render hook: the frame-boundary catch covers the whole frame.
		class ThrowingClient final : public IFrameLoopClient
		{
		public:
			explicit ThrowingClient(bool throwInUpdate)
				: m_ThrowInUpdate(throwInUpdate)
			{
			}

			void OnFrameEvent(Event& /*event*/) override {}

			void OnFrameFixedStep(const SimStep& /*step*/) override {}

			void OnFrameUpdate(const FrameTime& /*frame*/) override
			{
				if (m_ThrowInUpdate)
					ThrowSystemError();
			}

			void OnFrameRender(const FrameTime& /*frame*/) override
			{
				if (!m_ThrowInUpdate)
					ThrowSystemError();
			}
		private:
			bool m_ThrowInUpdate = false;
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

			void OnFrameRender(const FrameTime& /*frame*/) override {}
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

	// Runs one frame whose update or render hook makes vulkan.hpp throw: the children of the frame-boundary catch tests.
	// Without a Vulkan loader in this process there is nothing to throw, which the parents report as a skip.
	static void RunThrowingFrame(bool throwInUpdate)
	{
		const ProcessContext* process = ProcessContext::GetCurrent();
		if (process == nullptr || !process->IsVulkanLoaderAvailable())
		{
			ENGINE_CORE_WARN("No Vulkan loader in the frame-loop child");
			return;
		}
		Result<Scope<EngineContext>> context = EngineContext::Create({ .WorkerCount = 0 });
		if (!context.has_value())
		{
			ENGINE_CORE_ERROR("The frame-loop child cannot create its engine context: {}", context.error());
			return;
		}
		ThrowingClient client(throwInUpdate);
		FrameLoop loop(**context, client, CreateScope<ManualClock>(FixedDelta), {});
		loop.RunFrame();
	}

	ENGINE_DEATH_TEST("App/FrameLoopRenderThrowsSystemError")
	{
		RunThrowingFrame(false);
	}

	ENGINE_DEATH_TEST("App/FrameLoopUpdateThrowsSystemError")
	{
		RunThrowingFrame(true);
	}

	// Runs the death-test child `name` and checks that the frame-boundary catch ended it through FatalError(Gpu) with a crash
	// report whose FramePhase breadcrumb is `phase`.
	static void CheckFrameBoundaryCatch(std::string_view name, std::string_view phase)
	{
		Test::TempDirectory directory("FrameBoundaryCatch");
		const ProcessSpecification specification = Test::MakeTestsChildSpecification({
			"--death-test=" + std::string(name),
			"--user-data-dir=" + Test::PathToUtf8(directory.GetPath()),
		});
		const Result<ProcessResult> child = Process::Run(specification, std::chrono::seconds(60));
		REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
		INFO("child stderr: ", child->StandardError);
		if (child->StandardError.contains("No Vulkan loader in the frame-loop child"))
		{
			Test::ReportGpuUnavailable("no Vulkan loader");
			return;
		}
		CHECK(child->ExitCode == ExitCode::Crash);
		CHECK(child->StandardError.contains("Fatal error (Gpu)"));
		const std::vector<std::filesystem::path> reports = Test::ListCrashFiles(directory / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
		REQUIRE(reports.size() == 1);
		const Result<std::string> report = FileSystem::ReadText(reports[0]);
		REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
		CHECK(report->contains(std::format("\n  FramePhase: {}\n", phase)));
	}

	TEST_SUITE("App")
	{
		TEST_CASE("FrameLoop: diagnostics measure completed frames without changing simulation")
		{
			const std::array<double, 2> deltas = { 1.0, 1.0 };
			Result<ScriptedClock> clock = ScriptedClock::Create(deltas);
			REQUIRE(clock.has_value());
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			const std::array<int, 4> milliseconds = { 0, 5, 20, 25 };
			size_t diagnosticCalls = 0;
			FrameLoopSpecification specification;
			specification.Loop.FixedHz = 10;
			specification.Loop.MaxStepsPerFrame = 2;
			specification.Loop.MaxFrameDelta = 0.5;
			specification.DiagnosticClock = [&milliseconds, &diagnosticCalls]()
			{
				REQUIRE(diagnosticCalls < milliseconds.size());
				return std::chrono::steady_clock::time_point(std::chrono::milliseconds(milliseconds[diagnosticCalls++]));
			};
			FrameLoop loop(*context, client, CreateScope<ScriptedClock>(std::move(*clock)), specification);
			CHECK(loop.GetStatistics().CompletedFrames == 0);
			CHECK(loop.GetStatistics().Fps == 0.0);
			CHECK(diagnosticCalls == 0);

			loop.RunFrame();
			CHECK(loop.GetStatistics().CompletedFrames == 1);
			CHECK(loop.GetStatistics().CpuMilliseconds == doctest::Approx(5.0));
			CHECK(loop.GetStatistics().Fps == 0.0);
			CHECK(loop.GetStatistics().DroppedSeconds == doctest::Approx(0.8));
			CHECK(loop.GetScheduler().GetTick() == 2);

			loop.RunFrame();
			CHECK(loop.GetStatistics().CompletedFrames == 2);
			CHECK(loop.GetStatistics().CpuMilliseconds == doctest::Approx(5.0));
			CHECK(loop.GetStatistics().Fps == doctest::Approx(50.0));
			CHECK(loop.GetStatistics().DroppedSeconds == doctest::Approx(1.6));
			CHECK(loop.GetScheduler().GetTick() == 4);
			CHECK(diagnosticCalls == 4);
			specification.Loop.FixedHz = 20;
			loop.SetLoopConfig(specification.Loop);
			CHECK(loop.GetStatistics().DroppedSeconds == doctest::Approx(1.6));
			CHECK(loop.GetStatistics().CompletedFrames == 2);
			CHECK(diagnosticCalls == 4);
		}

		TEST_CASE("FrameLoop: a ManualClock frame runs exactly one step before its update and render")
		{
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});

			for (int frame = 0; frame < 3; ++frame)
				loop.RunFrame();

			const std::vector<std::string> expected = {
				"step 0",
				"update 0",
				"render 0",
				"step 1",
				"update 1",
				"render 1",
				"step 2",
				"update 2",
				"render 2",
			};
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

			const std::vector<std::string> expected = { "drained", "safe point", "step 0", "update 0", "render 0", "safe point", "step 1", "update 1", "render 1" };
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
				CHECK(client.Calls.back() == std::format("render {}", frame));
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
			CHECK(std::ranges::count(client.Calls, std::string("render 9")) == 1);
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

		TEST_CASE("FrameLoop: a vk::SystemError from the render hook ends the process through FatalError"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §4.6 item 2: the one frame-boundary catch maps the error's result (VK_ERROR_LAYER_NOT_PRESENT here) to
			// FatalError(Gpu), which writes a crash report naming the frame phase, and exits with code 4.
			CheckFrameBoundaryCatch("App/FrameLoopRenderThrowsSystemError", "Render");
		}

		TEST_CASE("FrameLoop: a vk::SystemError from the update hook ends the process through FatalError"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The catch covers the whole frame (§4.2 step 7), not only rendering: NVRHI work outside the render step (a
			// screenshot read back during an update, an asset swap's upload) is guarded the same way.
			CheckFrameBoundaryCatch("App/FrameLoopUpdateThrowsSystemError", "Update");
		}

		TEST_CASE("FrameLoop: the time scale scales the steps and the frame delta")
		{
			// Docs/Decisions/0012-m7-decisions.md decision 3.
			const std::array<double, 1> deltas = { 4.0 * FixedDelta };
			Result<ScriptedClock> clock = ScriptedClock::Create(deltas);
			REQUIRE(clock.has_value());
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ScriptedClock>(std::move(*clock)), {});
			CHECK(loop.GetTimeScale() == 1.0);
			loop.SetTimeScale(0.5);
			loop.RunFrame();
			const auto steps = std::ranges::count_if(client.Calls, [](const std::string& call)
			{
				return call.starts_with("step ");
			});
			CHECK(steps == 2);
			CHECK(loop.GetLastFrameTime().DeltaTime == doctest::Approx(2.0 * FixedDelta));
			CHECK(loop.GetLastFrameTime().UnscaledDeltaTime == doctest::Approx(4.0 * FixedDelta));

			// A paused session's scale of 0 runs no step.
			loop.SetTimeScale(0.0);
			client.Calls.clear();
			loop.RunFrame();
			CHECK(std::ranges::none_of(client.Calls, [](const std::string& call)
			{
				return call.starts_with("step ");
			}));
		}

		TEST_CASE("FrameLoop: a ManualClock frame at a time scale other than 1 advances by the scaled fixed delta")
		{
			// Docs/Decisions/0012-m7-decisions.md decision 3.
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});
			loop.SetTimeScale(2.0);
			loop.RunFrame();
			CHECK(std::ranges::count_if(client.Calls, [](const std::string& call)
			{
				return call.starts_with("step ");
			}) == 2);
			CHECK(loop.GetLastFrameTime().DeltaTime == doctest::Approx(2.0 * FixedDelta));
		}

		TEST_CASE("FrameLoop: a suspended throttle runs frames without waiting for their slots")
		{
			// Docs/Decisions/0012-m7-decisions.md decision 3. One-second slots: a loop that still waited for them would need
			// hours for these frames and fail at the suite's timeout, a failure bound only; nothing here measures time (ADR 0008
			// decision 15).
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			constexpr uint64_t Frames = 10000;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(1.0),
				{ .Loop = { .FixedHz = 1 }, .MaxFrames = Frames, .ThrottleToFixedHz = true });
			CHECK_FALSE(loop.IsThrottleSuspended());
			loop.SetThrottleSuspended(true);
			CHECK(loop.IsThrottleSuspended());
			CHECK(loop.Run() == ExitCode::Success);
			CHECK(loop.GetFrameCount() == Frames);
		}

		TEST_CASE("FrameLoop: a new loop config rebuilds the scheduler and the ManualClock at its FixedHz")
		{
			// Docs/Decisions/0012-m7-decisions.md decision 3: the editor plays a project at its Simulation.FixedHz
			// (EditorPlayController::GetFrameLoopConfig).
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), {});
			loop.RunFrame();
			loop.RunFrame();

			// The current config changes nothing: the tick goes on.
			loop.SetLoopConfig(FrameLoopConfig{});
			CHECK(loop.GetScheduler().GetTick() == 2);

			// A 30 Hz project: the loop's tick restarts at 0 and every ManualClock frame steps 1/30 s.
			loop.SetLoopConfig({ .FixedHz = 30, .MaxStepsPerFrame = 3 });
			CHECK(loop.GetLoopConfig().FixedHz == 30);
			CHECK(loop.GetLoopConfig().MaxStepsPerFrame == 3);
			CHECK(loop.GetScheduler().GetTick() == 0);
			client.Calls.clear();
			loop.RunFrame();
			REQUIRE_FALSE(client.Calls.empty());
			CHECK(client.Calls.front() == "step 0");
			CHECK(loop.GetScheduler().GetFixedDelta() == doctest::Approx(1.0 / 30.0));
			CHECK(loop.GetLastFrameTime().DeltaTime == doctest::Approx(1.0 / 30.0));
			REQUIRE(loop.GetClock().GetKind() == ClockKind::Manual);
			CHECK(static_cast<const ManualClock&>(loop.GetClock()).GetFixedDelta() == doctest::Approx(1.0 / 30.0));
		}
	}

}
