#include "EnginePCH.h"
#include "Engine/App/FrameLoop.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Platform/CrashHandler.h"

#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace Engine {

	namespace Utils {

		// The ClockKind enumerator name, for the "Frame loop started" line.
		static std::string_view ClockKindToString(ClockKind kind)
		{
			switch (kind)
			{
				case ClockKind::System:   return "System";
				case ClockKind::Manual:   return "Manual";
				case ClockKind::Scripted: return "Scripted";
			}

			ENGINE_CORE_ASSERT(false, "Unknown ClockKind {}", std::to_underlying(kind));
			return "Unknown";
		}

	}

	FrameLoop::FrameLoop(EngineContext& context, IFrameLoopClient& client, Scope<Clock> clock,
		const FrameLoopSpecification& specification)
		: m_Context(&context), m_Client(&client), m_Clock(std::move(clock)), m_Specification(specification), m_Scheduler(specification.Loop)
	{
		ENGINE_CORE_ASSERT(m_Clock != nullptr, "FrameLoop needs a clock");
		if (Window* window = m_Context->GetWindow())
		{
			window->SetEventCallback([this](Event& event)
			{
				DispatchEvent(event);
			});
		}
	}

	FrameLoop::~FrameLoop()
	{
		if (Window* window = m_Context->GetWindow())
			window->SetEventCallback({});
	}

	void FrameLoop::RunFrame()
	{
		ENGINE_PROFILE_SCOPE("FrameLoop::RunFrame");
		const std::chrono::steady_clock::time_point frameStart =
			m_Specification.ThrottleToFixedHz ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

		// 1. Events: the window delivers them to DispatchEvent. A minimized window waits for one instead of spinning (§4.2).
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "Events");
		if (Window* window = m_Context->GetWindow())
		{
			ENGINE_PROFILE_SCOPE("FrameLoop::Events");
			if (window->IsMinimized())
				window->WaitEventsTimeout(m_Scheduler.GetFixedDelta());
			else
				window->PollEvents();
		}

		// 2. Work posted to the main thread: job completions, asset swaps, file-watcher results.
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "MainThreadQueue");
		m_Context->GetMainThreadQueue().Drain();

		// 3. The safe point (§4.2 step 3): the client's automation server runs queued requests here.
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "SafePoint");
		{
			ENGINE_PROFILE_SCOPE("FrameLoop::SafePoint");
			m_Client->OnFrameSafePoint();
		}

		// 4. and 5. The fixed steps: exactly one per ManualClock frame (Alpha = 1), otherwise what the scheduler gives.
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "FixedStep");
		const double delta = m_Clock->Delta();
		const FrameSteps steps =
			m_Clock->GetKind() == ClockKind::Manual ? m_Scheduler.StepExactly(1) : m_Scheduler.Advance(delta, 1.0);
		{
			ENGINE_PROFILE_SCOPE("FrameLoop::FixedSteps");
			for (uint32_t step = 0; step < steps.StepCount; ++step)
				m_Client->OnFrameFixedStep(m_Scheduler.GetSimStep(steps.FirstTick + step));
		}

		// 6. The frame update. Time scaling arrives with play sessions, so the scaled delta equals the clock's.
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "Update");
		const FrameTime frame = {
			.DeltaTime = delta,
			.UnscaledDeltaTime = delta,
			.Alpha = steps.Alpha,
			.FrameIndex = m_FrameCount,
		};
		{
			ENGINE_PROFILE_SCOPE("FrameLoop::Update");
			m_Client->OnFrameUpdate(frame);
		}

		// Step 7 of §4.2, rendering (OnRender, ImGui, present), joins here with the renderer, which also wraps the whole
		// frame in the allowlisted frame-boundary catch of vk::SystemError (§4.6 item 2, §8.14). Until then nothing in the
		// frame calls Vulkan, so there is nothing for that catch to handle.

		m_LastFrameTime = frame;
		++m_FrameCount;
		if (m_Specification.MaxFrames.has_value() && m_FrameCount >= *m_Specification.MaxFrames)
			RequestExit(ExitCode::Success);
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, {});

		// A frame that ends the loop does not wait for its slot.
		if (m_Specification.ThrottleToFixedHz && !m_ExitRequested)
			WaitForFrameSlot(frameStart);
	}

	int FrameLoop::Run()
	{
		ENGINE_CORE_INFO("Frame loop started: {} clock, {} Hz, {}", Utils::ClockKindToString(m_Clock->GetKind()),
			m_Specification.Loop.FixedHz, m_Specification.ThrottleToFixedHz ? "throttled" : "unthrottled");
		while (!m_ExitRequested)
			RunFrame();
		return m_ExitCode;
	}

	void FrameLoop::RequestExit(int exitCode)
	{
		if (m_ExitRequested)
			return;
		m_ExitRequested = true;
		m_ExitCode = exitCode;
	}

	bool FrameLoop::IsExitRequested() const
	{
		return m_ExitRequested;
	}

	int FrameLoop::GetExitCode() const
	{
		return m_ExitCode;
	}

	uint64_t FrameLoop::GetFrameCount() const
	{
		return m_FrameCount;
	}

	const FixedStepScheduler& FrameLoop::GetScheduler() const
	{
		return m_Scheduler;
	}

	const Clock& FrameLoop::GetClock() const
	{
		return *m_Clock;
	}

	const FrameTime& FrameLoop::GetLastFrameTime() const
	{
		return m_LastFrameTime;
	}

	void FrameLoop::DispatchEvent(Event& event)
	{
		m_Client->OnFrameEvent(event);
		if (IsHandled(event))
			return;
		if (std::holds_alternative<WindowCloseEvent>(event))
			RequestExit(ExitCode::Success);
		m_Context->GetInputState().Inject(event);
	}

	void FrameLoop::WaitForFrameSlot(std::chrono::steady_clock::time_point frameStart)
	{
		const auto slot = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::duration<double>(m_Scheduler.GetFixedDelta()));
		// Slots follow each other, so a frame that ran long is made up by the next ones instead of drifting.
		m_FrameSlotEnd = m_FrameSlotEnd.has_value() ? *m_FrameSlotEnd + slot : frameStart + slot;

		// A loop that fell more than a slot behind (a stall, a debugger) starts pacing anew instead of running a burst of
		// unpaced frames to catch up.
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		if (*m_FrameSlotEnd + slot < now)
			m_FrameSlotEnd = now;
		std::this_thread::sleep_until(*m_FrameSlotEnd);
	}

}
