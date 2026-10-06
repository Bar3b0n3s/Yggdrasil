#include "TestsPCH.h"

#include "Engine/App/FrameLoop.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Platform/Process.h"
#include "Support/TestOptions.h"
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
		};

	}

	static constexpr double FixedDelta = 1.0 / 60.0;

	static Scope<EngineContext> CreateContext(std::optional<WindowSpecification> window = std::nullopt)
	{
		Result<Scope<EngineContext>> context = EngineContext::Create({ .WorkerCount = 0, .Window = std::move(window) });
		REQUIRE_MESSAGE(context.has_value(), context.error().ToString());
		return std::move(*context);
	}

	TEST_SUITE("App")
	{
		TEST_CASE("FrameLoop: a ManualClock frame runs exactly one step before its update" * doctest::skip(true))
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

		TEST_CASE("FrameLoop: other clocks run as many steps as the scheduler gives" * doctest::skip(true))
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

		TEST_CASE("FrameLoop: MaxFrames ends Run with Success after that many frames" * doctest::skip(true))
		{
			Scope<EngineContext> context = CreateContext();
			RecordingClient client;
			FrameLoop loop(*context, client, CreateScope<ManualClock>(FixedDelta), { .MaxFrames = 10 });
			CHECK(loop.Run() == ExitCode::Success);
			CHECK(loop.GetFrameCount() == 10);
			CHECK(std::ranges::count(client.Calls, std::string("update 9")) == 1);
		}

		TEST_CASE("FrameLoop: the first exit request wins and an early request runs no frame" * doctest::skip(true))
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

		TEST_CASE("FrameLoop: events reach the client first and only unhandled ones reach InputState" * doctest::skip(true))
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

		TEST_CASE("FrameLoop: queued main-thread tasks run in each frame" * doctest::skip(true))
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
			for (int poll = 0; poll < 200 && !window.IsMinimized(); ++poll)
				window.WaitEventsTimeout(0.01);
			REQUIRE(window.IsMinimized());

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

		TEST_CASE("FrameLoop: a minimized window idles" * doctest::skip(true))
		{
			ENGINE_CHECK_WINDOWED_CHILD("FrameLoop: a minimized window uses little CPU time per second");
		}
	}

}
