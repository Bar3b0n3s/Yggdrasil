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
//
// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 40, R-HANG): a hang that the driver or the operating system
// resets (a Windows TDR, a Linux kernel GPU reset) is recovered like a device loss. When the 10 s budget is spent, the wait
// goes on for the reset wait (GraphicsSpecification::ResetWaitMilliseconds, --gpu-reset-wait) in the same slices, waiting
// for a loss report: VK_ERROR_DEVICE_LOST or the device-lost flag becomes the device-loss path, which the frame boundary
// recovers; a submission that completes in the reset wait was slow, not hung, and the wait returns; only a hang with no loss
// report after the reset wait stays FatalError(GpuHang) (Vulkan cannot cancel submitted work, a device with pending work
// cannot be destroyed, and a second device on the same GPU would queue behind it).

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

	// M18 (C18.4, decisions 40 and 41): the slices of a reset wait of `resetWaitMilliseconds`, whole GpuWaitSliceNanoseconds
	// slices rounded up (15,000 ms is 150 slices; 1 ms is 1; 0 is none). Like the budget, a number of slices, never wall time.
	[[nodiscard]] constexpr uint32_t GetGpuResetWaitSliceCount(uint32_t resetWaitMilliseconds)
	{
		constexpr uint64_t SliceMilliseconds = GpuWaitSliceNanoseconds / 1'000'000;
		return static_cast<uint32_t>((uint64_t{ resetWaitMilliseconds } + SliceMilliseconds - 1) / SliceMilliseconds);
	}

	// M18 (C18.4, decision 40): a bounded wait's outcome with its reset wait (RunGpuWaitWithReset).
	struct GpuWaitOutcome
	{
		// Signaled, DeviceLost or Failed as the first slice that did not time out reported it; Hang when every slice of the
		// budget and of the reset wait timed out.
		GpuWaitResult Result = GpuWaitResult::Signaled;
		// Whether the budget had timed out: that slice was one of the reset wait's (always true for Hang, unless the reset
		// wait has no slice, when the budget alone timed out).
		bool InResetWait = false;
	};

	// M18 (C18.4, decision 40): the wait state machine with the reset wait, pure like RunBoundedGpuWait and unit-tested
	// without a GPU or a clock: RunBoundedGpuWait(slice, maxSlices); when that is a Hang and `resetSlices` is not 0, calls
	// `slice` up to `resetSlices` more times until it reports anything but Timeout (RunBoundedGpuWait(slice, resetSlices)),
	// and returns that with InResetWait. `maxSlices` >= 1 (asserted); `resetSlices` 0 is no reset wait (the M5 behaviour).
	[[nodiscard]] GpuWaitOutcome RunGpuWaitWithReset(const std::function<GpuWaitSliceResult()>& slice, uint32_t maxSlices, uint32_t resetSlices);

	// Waits until the graphics queue's timeline semaphore reaches `submissionID` (GraphicsDevice::ExecuteCommandList's
	// result) with RunBoundedGpuWait over vkWaitSemaphores slices (through the dispatcher's C entry point, so nothing
	// throws). Returns at once for submission 0 (nothing submitted) and when the submission already completed
	// (GraphicsDevice::GetCompletedSubmissionID). A Hang ends the process through FatalError(GpuHang) naming `context` and
	// the submission; DeviceLost through GraphicsDevice::RaiseDeviceLost(context); Failed through FatalError with
	// GetFatalErrorKind of the failed slice's VkResult (OutOfMemory for the out-of-memory results, otherwise Gpu).
	// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 28): DeviceLost reports the loss
	// (GraphicsDevice::ReportDeviceLost("<context>: waiting for GPU submission <id>")) and returns, and on a device already
	// lost the wait returns at once; a caller that reads what the submission wrote checks GraphicsDevice::IsLost first
	// (Readback::ReadTexture then fails with Gpu).
	//
	// M18 (C18.4, decision 40): the wait is RunGpuWaitWithReset with GpuWaitMaxSlices and
	// GetGpuResetWaitSliceCount(device.GetSpecification().ResetWaitMilliseconds), whose reset-wait slices also report
	// DeviceLost when the device-lost flag is set (GraphicsDevice::IsLost: NVRHI's "Device Removed!" from another thread's
	// report). When the budget is spent, one Warn line names `context`, the submission and the reset wait it goes on for.
	// Outcomes: Signaled in the reset wait returns (one Warn line: the submission completed after the budget, a slow GPU
	// rather than a hang); DeviceLost in the reset wait reports the loss (GraphicsDevice::ReportDeviceLost("<context>: GPU
	// submission <id> did not complete within the 10 s budget, and the device was reset during the reset wait")) and
	// returns, the frame boundary recovering the device like any loss; a Hang (no loss report after the reset wait) ends the
	// process through FatalError(GpuHang) naming both waits, as before: the editor's autosave, a crash report, exit code 4.
	// Without a reset wait (--gpu-reset-wait 0) the budget's hang is fatal at once, and the message says the reset wait is
	// off instead of naming one that did not run.
	//
	// Under --gpu-inject-fault=hang the wait skips the completed-submission early return and goes straight to its slices,
	// every one of which reports Timeout without calling vkWaitSemaphores, so the process ends with FatalError(GpuHang) after
	// the budget's GpuWaitMaxSlices slices (and, from M18, the reset wait's) however fast the GPU is (§8.14 item 8, the
	// `hang` spelling). The injected slices do not wait, so the fault test takes neither wait in
	// wall time; their lengths are RunGpuWaitWithReset's unit-tested slice counts. M18 (C18.4, decision 46): the
	// wait takes its injected fault from GpuDiagnostics::TakeBoundedWaitFault once, at its start: Hang times out every slice
	// of the budget and of the reset wait without waiting (the M5 fault, now through both waits); HangReset times out the
	// budget's slices without waiting, then marks the device lost as an injected loss (GpuDiagnostics::SetDeviceLost(true),
	// so DescribeDeviceFault never queries the healthy Vulkan device) and reports DeviceLost at the reset wait's first slice.
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
		// objects created at startup), except on a lost device (M18, decision 28), where the slot stays without a query: its
		// frames then wait for their submission without polling, and the pacer's owner replaces it in the recovery.
		FramePacer(GraphicsDevice& device, uint32_t framesInFlight);
		~FramePacer();

		FramePacer(const FramePacer&) = delete;
		FramePacer& operator=(const FramePacer&) = delete;

		// Advances to the next frame and waits until its slot is free: pollEventQuery on the slot's event query, and when it
		// has not signaled, WaitForSubmission on the slot's recorded submission ID (a fatal error on hang, so this returns
		// only when the slot is free or the device is lost). M18 (C18.3, decision 28): on a lost device it waits for nothing and
		// returns, and a loss its wait meets is reported (WaitForSubmission); the pacer's owner destroys it in the recovery's
		// release and creates a new one (decision 33). The first FramesInFlight frames never wait. Under
		// --gpu-inject-fault=hang, every frame whose slot has a recorded submission skips pollEventQuery and calls
		// WaitForSubmission, which then never returns (see WaitForSubmission), so a headless run ends with
		// FatalError(GpuHang) at its first such frame even when the GPU finished long ago. M18 (C18.4, decision 46): likewise
		// whenever GpuDiagnostics::HasBoundedWaitFault (the hang-reset faults once armed), so the injected reset strikes the
		// frame's wait even when the GPU finished.
		void BeginFrame();

		// Records `lastSubmissionID` (the frame's last GraphicsDevice::ExecuteCommandList(s) result, or
		// GraphicsDevice::GetLastSubmissionID for a frame that submitted nothing, such as one the swapchain skipped) for the
		// current slot and sets the slot's event query on the graphics queue, which a lost device skips (M18). Called once per
		// BeginFrame, after the frame's submissions.
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
