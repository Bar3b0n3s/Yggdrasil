#include "EnginePCH.h"
#include "Engine/App/FrameLoop.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/CrashHandler.h"
#include "Engine/Session/PlaySession.h"

#include <vulkan/vulkan.hpp>

#include <cmath>
#include <format>
#include <string>
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

		// FrameLoopConfig has no equality operator: every member compared exactly.
		static bool IsSameLoopConfig(const FrameLoopConfig& left, const FrameLoopConfig& right)
		{
			return left.FixedHz == right.FixedHz && left.MaxStepsPerFrame == right.MaxStepsPerFrame && left.MaxFrameDelta == right.MaxFrameDelta;
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
		// The one allowlisted frame-boundary catch (§4.6 item 2, §8.14). vulkan.hpp's enhanced mode throws from calls NVRHI
		// makes internally; first-party Vulkan calls never throw. The frame never resumes: the handler ends the process, and
		// the FramePhase breadcrumb still names the step that threw.
		try
		{
			RunFrameSteps();
		}
		catch (const vk::SystemError& error)
		{
			RaiseVulkanError(m_Context->GetGraphicsDevice(), static_cast<VkResult>(error.code().value()),
				std::format("Vulkan error at the frame boundary: {}", error.what()));
		}
	}

	void FrameLoop::RunFrameSteps()
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

		// 4. and 5. The fixed steps: exactly one per ManualClock frame at a time scale of 1 (Alpha = 1), otherwise what the
		// scheduler gives for the clock's delta at the time scale (a ManualClock's delta is FixedDelta).
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "FixedStep");
		const double delta = m_Clock->Delta();
		const bool exactStep = m_Clock->GetKind() == ClockKind::Manual && m_TimeScale == 1.0;
		const FrameSteps steps = exactStep ? m_Scheduler.StepExactly(1) : m_Scheduler.Advance(delta, m_TimeScale);
		{
			ENGINE_PROFILE_SCOPE("FrameLoop::FixedSteps");
			for (uint32_t step = 0; step < steps.StepCount; ++step)
				m_Client->OnFrameFixedStep(m_Scheduler.GetSimStep(steps.FirstTick + step));
		}

		// 6. The frame update, with the clock's delta scaled by the play session's time scale.
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "Update");
		const FrameTime frame = {
			.DeltaTime = delta * m_TimeScale,
			.UnscaledDeltaTime = delta,
			.Alpha = steps.Alpha,
			.FrameIndex = m_FrameCount,
		};
		{
			ENGINE_PROFILE_SCOPE("FrameLoop::Update");
			m_Client->OnFrameUpdate(frame);
		}

		// 7. Rendering: OnRender, ImGui, present.
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, "Render");
		{
			ENGINE_PROFILE_SCOPE("FrameLoop::Render");
			m_Client->OnFrameRender(frame);
		}

		m_LastFrameTime = frame;
		++m_FrameCount;
		if (m_Specification.MaxFrames.has_value() && m_FrameCount >= *m_Specification.MaxFrames)
			RequestExit(ExitCode::Success);
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::FramePhase, {});

		// A frame that ends the loop does not wait for its slot. While the throttle is suspended (a running play.step), frames
		// follow each other at once, and the next throttled frame starts a new slot when it starts.
		if (m_Specification.ThrottleToFixedHz && !m_ExitRequested)
		{
			if (m_ThrottleSuspended)
				m_FrameSlotEnd.reset();
			else
				WaitForFrameSlot(frameStart);
		}
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

	void FrameLoop::SetTimeScale(double timeScale)
	{
		ENGINE_CORE_ASSERT(std::isfinite(timeScale) && timeScale >= 0.0 && timeScale <= PlaySession::MaxTimeScale,
			"FrameLoop::SetTimeScale: {} is not a time scale in [0, {}]", timeScale, PlaySession::MaxTimeScale);
		m_TimeScale = timeScale;
	}

	double FrameLoop::GetTimeScale() const
	{
		return m_TimeScale;
	}

	void FrameLoop::SetThrottleSuspended(bool suspended)
	{
		m_ThrottleSuspended = suspended;
	}

	bool FrameLoop::IsThrottleSuspended() const
	{
		return m_ThrottleSuspended;
	}

	void FrameLoop::SetLoopConfig(const FrameLoopConfig& config)
	{
		// The editor applies its config every frame: an unchanged one keeps the scheduler's tick and accumulator.
		if (Utils::IsSameLoopConfig(config, m_Scheduler.GetConfig()))
			return;

		// FixedStepScheduler asserts the ranges; the project loader validated them. Throttled frames are paced by the
		// scheduler's FixedDelta (WaitForFrameSlot), so the current frame's slot already ends at the new rate.
		m_Scheduler = FixedStepScheduler(config);
		m_Specification.Loop = config;
		// A ManualClock advances exactly one FixedDelta per frame, so it is replaced by one at the new rate.
		if (m_Clock->GetKind() == ClockKind::Manual)
			m_Clock = CreateScope<ManualClock>(m_Scheduler.GetFixedDelta());
		ENGINE_CORE_INFO("Frame loop: {} Hz, at most {} steps per frame", config.FixedHz, config.MaxStepsPerFrame);
	}

	const FrameLoopConfig& FrameLoop::GetLoopConfig() const
	{
		return m_Scheduler.GetConfig();
	}

}
