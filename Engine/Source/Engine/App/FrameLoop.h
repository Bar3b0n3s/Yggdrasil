#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Clock.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Time.h"
#include "Engine/Platform/Events.h"

#include <chrono>
#include <cstdint>
#include <optional>

// The main loop (Architecture §4.2): fixed-step simulation, variable-rate frames.

namespace Engine {

	class EngineContext;

	// What one frame calls, in the order the frame calls it. Application is the production client; tests use a recording
	// client to check the order.
	class IFrameLoopClient
	{
	public:
		virtual ~IFrameLoopClient() = default;

		// One polled or injected event, before InputState sees it. Marking it handled keeps it from InputState, and a
		// handled WindowCloseEvent does not end the loop (an editor asking to save first).
		virtual void OnFrameEvent(Event& event) = 0;
		// One fixed step; zero or more per frame (step.Tick increases by one per call).
		virtual void OnFrameFixedStep(const SimStep& step) = 0;
		// Once per frame, after the frame's steps.
		virtual void OnFrameUpdate(const FrameTime& frame) = 0;
	};

	struct FrameLoopSpecification
	{
		FrameLoopConfig Loop{};
		// End the loop with ExitCode::Success after this many frames (--frames N); nullopt runs until an exit request.
		std::optional<uint64_t> MaxFrames{};
		// Pace frames to FixedHz per wall-clock second (§4.2: headless play without lockstep never runs unthrottled).
		// Lockstep, batch and test runs, and unit tests, leave it off.
		bool ThrottleToFixedHz = false;
	};

	// Runs frames over an EngineContext for one client (§4.2). Not copyable or movable; main thread only.
	//
	// One frame (RunFrame), in order:
	//   1. Events: when the context has a window, Window::PollEvents, or Window::WaitEventsTimeout(FixedDelta) while it
	//      is minimized (§4.2: the loop idles instead of spinning). Each delivered event goes to OnFrameEvent; an unhandled
	//      WindowCloseEvent requests exit with ExitCode::Success; every unhandled event is then injected into the
	//      context's InputState. The loop does not latch InputState: the simulation that reads it latches at the points
	//      §5.7 gives (PlaySession, M7), so events stamped for a tick are applied before that tick's LatchStep.
	//   2. MainThreadQueue::Drain of the context: job completions, asset swaps and file-watcher results in posting order.
	//   3. Steps: with a ManualClock, exactly one step (FixedStepScheduler::StepExactly(1), Alpha = 1, §4.2); with any
	//      other clock, FixedStepScheduler::Advance(clock.Delta(), 1.0). OnFrameFixedStep runs once per step, in tick
	//      order.
	//   4. OnFrameUpdate with this frame's FrameTime: the clock's delta (DeltaTime equals UnscaledDeltaTime until time
	//      scaling arrives with play sessions), the scheduler's Alpha, and the frame index from 0.
	//   5. The frame count grows by one; when it reaches MaxFrames the loop requests exit with ExitCode::Success (an
	//      earlier request keeps its code). With ThrottleToFixedHz the loop then sleeps until the frame's wall-clock slot
	//      ends.
	// Automation pumping (M4) and rendering with the frame-boundary vk::SystemError catch (M5) join the frame at the
	// places §4.2 gives.
	//
	// Run logs one Info line before the first frame, "Frame loop started: <ClockKind> clock, <FixedHz> Hz" with the
	// ClockKind enumerator name, so a process's output shows which clock it runs on.
	class FrameLoop
	{
	public:
		// `context` and `client` are documented back-references (§4.7): both must outlive the loop. `clock` must be
		// non-null (asserted). The scheduler is built from specification.Loop.
		FrameLoop(EngineContext& context, IFrameLoopClient& client, Scope<Clock> clock, const FrameLoopSpecification& specification);
		~FrameLoop();

		FrameLoop(const FrameLoop&) = delete;
		FrameLoop& operator=(const FrameLoop&) = delete;

		// Runs one frame (see the class comment), also after an exit request.
		void RunFrame();

		// Runs frames until exit is requested and returns the requested exit code. An exit requested before Run (by the
		// client's initialization) runs no frame.
		[[nodiscard]] int Run();

		// Ends the loop after the current frame with `exitCode`. The first request wins; later ones are ignored.
		void RequestExit(int exitCode);

		[[nodiscard]] bool IsExitRequested() const;
		// The requested exit code; ExitCode::Success while none was requested.
		[[nodiscard]] int GetExitCode() const;
		// The number of frames completed.
		[[nodiscard]] uint64_t GetFrameCount() const;
		[[nodiscard]] const FixedStepScheduler& GetScheduler() const;
		[[nodiscard]] const Clock& GetClock() const;
		// The FrameTime of the last completed frame (all zero before the first).
		[[nodiscard]] const FrameTime& GetLastFrameTime() const;
	private:
		// Step 1 for one delivered event: the client, then the exit request of an unhandled close, then InputState.
		void DispatchEvent(Event& event);
		// Step 5's pacing: sleeps until the wall-clock slot of the frame that started at `frameStart` ends.
		void WaitForFrameSlot(std::chrono::steady_clock::time_point frameStart);
	private:
		EngineContext* m_Context = nullptr;   // documented back-reference: outlives the loop
		IFrameLoopClient* m_Client = nullptr; // documented back-reference: outlives the loop
		Scope<Clock> m_Clock;
		FrameLoopSpecification m_Specification;
		FixedStepScheduler m_Scheduler;
		FrameTime m_LastFrameTime;
		uint64_t m_FrameCount = 0;
		int m_ExitCode = 0; // ExitCode::Success until a request
		bool m_ExitRequested = false;
		// The end of the current throttle slot (ThrottleToFixedHz); nullopt before the first throttled frame.
		std::optional<std::chrono::steady_clock::time_point> m_FrameSlotEnd;
	};

}
