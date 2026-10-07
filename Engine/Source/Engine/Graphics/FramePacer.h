#pragma once

#include "Engine/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

// Frame pacing that never waits unboundedly (Architecture §8.1). nvrhi::IDevice::waitEventQuery waits with an infinite
// timeout and asserts its result (NDEBUG is set only in Dist), so the engine never calls it. Instead each frame in
// flight records the submission ID of its last executeCommandLists; BeginFrame first polls the slot's event query and
// then waits on the graphics queue's timeline semaphore with vkWaitSemaphores in 100 ms slices. VK_TIMEOUT retries until
// a 10 s budget is spent, which becomes FatalError(GpuHang); VK_ERROR_DEVICE_LOST becomes the device-loss path.

namespace Engine {

	class GraphicsDevice;

	// One wait slice's outcome: what vkWaitSemaphores returned, mapped.
	enum class GpuWaitSliceResult : uint8_t
	{
		Signaled,   // VK_SUCCESS
		Timeout,    // VK_TIMEOUT (also every slice under --gpu-inject-fault=hang)
		DeviceLost, // VK_ERROR_DEVICE_LOST
		Failed      // any other result (out of memory, ...)
	};

	// A bounded wait's outcome.
	enum class GpuWaitResult : uint8_t
	{
		Signaled,
		Hang,       // every slice of the budget timed out
		DeviceLost, // a slice reported device loss
		Failed      // a slice failed otherwise
	};

	// The slice and the budget of every bounded GPU wait (§8.1): 100 slices of 100 ms, 10 s in all.
	inline constexpr uint64_t GpuWaitSliceNanoseconds = 100'000'000;
	inline constexpr uint32_t GpuWaitMaxSlices = 100;

	// The bounded-wait state machine, separated from Vulkan so it is unit-tested without a GPU or a clock: calls `slice`
	// until it reports anything but Timeout, at most `maxSlices` times (>= 1, asserted). Returns Signaled, DeviceLost or
	// Failed as the first non-timeout slice reports, or Hang when all `maxSlices` slices timed out. The budget is a
	// number of slices, not wall time: each real slice waits up to GpuWaitSliceNanoseconds on the GPU, so the default
	// budget is 10 s of GPU time without the engine ever reading a clock. Pure; `slice` is called synchronously.
	[[nodiscard]] GpuWaitResult RunBoundedGpuWait(const std::function<GpuWaitSliceResult()>& slice, uint32_t maxSlices = GpuWaitMaxSlices);

	// Waits until the graphics queue's timeline semaphore reaches `submissionID` (GraphicsDevice::ExecuteCommandList's
	// result) with RunBoundedGpuWait over vkWaitSemaphores slices (through the dispatcher's C entry point, so nothing
	// throws). Returns at once for submission 0 (nothing submitted) and when the submission already completed
	// (GraphicsDevice::GetCompletedSubmissionID). A Hang ends the process through FatalError(GpuHang) naming `context` and
	// the submission; DeviceLost through GraphicsDevice::RaiseDeviceLost(context); Failed through FatalError with
	// GetFatalErrorKind of the failed slice's VkResult (OutOfMemory for the out-of-memory results, otherwise Gpu).
	//
	// Under --gpu-inject-fault=hang (GpuDiagnostics::ShouldInjectHang) the wait skips the completed-submission early return
	// and goes straight to RunBoundedGpuWait, whose every slice reports Timeout without calling vkWaitSemaphores, so the
	// process ends with FatalError(GpuHang) after the budget's GpuWaitMaxSlices slices however fast the GPU is (§8.14 item 8:
	// "a timeline wait that always reports VK_TIMEOUT"). The injected slices do not wait, so the fault test does not take
	// 10 s of wall time; the budget's length is RunBoundedGpuWait's unit-tested slice count.
	//
	// Readback and screenshots use it to wait for their copy; main thread only.
	void WaitForSubmission(GraphicsDevice& device, uint64_t submissionID, std::string_view context);

	// Paces the frames in flight. Not copyable or movable; main thread only. Owned by the application's frame
	// (Application, B) next to the swapchain or the offscreen target.
	//
	// A frame: BeginFrame waits (bounded) until the frame that last used the next slot completed, then the frame records
	// and submits, and EndFrame records the frame's last submission ID for its slot. With FramesInFlight 2, frame N waits
	// for frame N-2 (§8.2).
	class FramePacer
	{
	public:
		// `device` is a documented back-reference and must outlive the pacer. `framesInFlight` >= 1 (asserted). Creates one
		// event query per slot through GraphicsDevice::CreateEventQuery; a failure is FatalError(OutOfMemory) (§8.14 item 7:
		// objects created at startup).
		FramePacer(GraphicsDevice& device, uint32_t framesInFlight);
		~FramePacer();

		FramePacer(const FramePacer&) = delete;
		FramePacer& operator=(const FramePacer&) = delete;

		// Advances to the next frame and waits until its slot is free: pollEventQuery on the slot's event query, and when it
		// has not signaled, WaitForSubmission on the slot's recorded submission ID (a fatal error on hang or device loss, so
		// this returns only when the slot is free). The first FramesInFlight frames never wait. Under
		// --gpu-inject-fault=hang, every frame whose slot has a recorded submission skips pollEventQuery and calls
		// WaitForSubmission, which then never returns (see WaitForSubmission), so a headless run ends with
		// FatalError(GpuHang) at its first such frame even when the GPU finished long ago.
		void BeginFrame();

		// Records `lastSubmissionID` (the frame's last GraphicsDevice::ExecuteCommandList(s) result, or
		// GraphicsDevice::GetLastSubmissionID for a frame that submitted nothing, such as one the swapchain skipped) for the
		// current slot and sets the slot's event query on the graphics queue. Called once per BeginFrame, after the frame's
		// submissions.
		void EndFrame(uint64_t lastSubmissionID);

		// The slot of the current frame, in [0, FramesInFlight).
		[[nodiscard]] uint32_t GetFrameSlot() const;
		// The number of BeginFrame calls so far minus one: 0 during the first frame.
		[[nodiscard]] uint64_t GetFrameIndex() const;
		[[nodiscard]] uint32_t GetFramesInFlight() const;
	private:
		// One frame in flight: the event query EndFrame sets and the submission it recorded (0: none yet).
		struct Slot
		{
			nvrhi::EventQueryHandle EventQuery{};
			uint64_t SubmissionID = 0;
		};
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference: outlives the pacer
		std::vector<Slot> m_Slots;
		uint64_t m_BegunFrames = 0;
		uint64_t m_FrameIndex = 0;
		uint32_t m_FramesInFlight = 1;
		uint32_t m_FrameSlot = 0;
		bool m_IsInFrame = false; // between BeginFrame and EndFrame
	};

	// "Signaled", "Hang", "DeviceLost" or "Failed".
	[[nodiscard]] std::string_view GpuWaitResultToString(GpuWaitResult result);

}
