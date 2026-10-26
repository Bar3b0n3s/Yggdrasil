#include "EnginePCH.h"
#include "Engine/Graphics/FramePacer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <vulkan/vulkan.hpp>

#include <format>

namespace Engine {

	namespace Utils {

		// One slice of a bounded wait: vkWaitSemaphores through the dispatcher's C entry point, so nothing throws and the
		// VkResult is mapped here (§8.1). `failure` receives the result of a Failed slice.
		static GpuWaitSliceResult WaitSemaphoreSlice(VkDevice device, VkSemaphore semaphore, uint64_t value, VkResult& failure)
		{
			const VkSemaphoreWaitInfo waitInfo = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				.pNext = nullptr,
				.flags = 0,
				.semaphoreCount = 1,
				.pSemaphores = &semaphore,
				.pValues = &value,
			};
			const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(device, &waitInfo, GpuWaitSliceNanoseconds);
			switch (result)
			{
				case VK_SUCCESS:           return GpuWaitSliceResult::Signaled;
				case VK_TIMEOUT:           return GpuWaitSliceResult::Timeout;
				case VK_ERROR_DEVICE_LOST: return GpuWaitSliceResult::DeviceLost;
				default:
				{
					failure = result;
					return GpuWaitSliceResult::Failed;
				}
			}
		}

	}

	GpuWaitResult RunBoundedGpuWait(const std::function<GpuWaitSliceResult()>& slice, uint32_t maxSlices)
	{
		ENGINE_CORE_ASSERT(maxSlices >= 1, "RunBoundedGpuWait needs a budget of at least one slice");
		for (uint32_t index = 0; index < maxSlices; ++index)
		{
			switch (slice())
			{
				case GpuWaitSliceResult::Signaled:   return GpuWaitResult::Signaled;
				case GpuWaitSliceResult::Timeout:    break;
				case GpuWaitSliceResult::DeviceLost: return GpuWaitResult::DeviceLost;
				case GpuWaitSliceResult::Failed:     return GpuWaitResult::Failed;
			}
		}
		return GpuWaitResult::Hang;
	}

	GpuWaitOutcome RunGpuWaitWithReset(const std::function<GpuWaitSliceResult()>& /*slice*/, uint32_t /*maxSlices*/, uint32_t /*resetSlices*/)
	{
		ENGINE_CONTRACT_STUB();
		return GpuWaitOutcome{ .Result = GpuWaitResult::Failed };
	}

	void WaitForSubmission(GraphicsDevice& device, uint64_t submissionID, std::string_view context)
	{
		// Submission 0 is "nothing submitted": the timeline starts there, so there is never anything to wait for. A lost
		// device runs nothing more (decision 28): nothing in flight will complete, so nothing is waited for.
		if (submissionID == 0 || device.IsLost())
			return;

		const bool injectHang = device.GetDiagnostics().ShouldInjectHang();
		if (injectHang)
			ENGINE_CORE_WARN("Injected GPU fault (hang): {}: the wait for submission {} times out", context, submissionID);
		else if (device.GetCompletedSubmissionID() >= submissionID || device.IsLost())
			return;

		const VkDevice vulkanDevice = device.GetVulkanDevice();
		const VkSemaphore timeline = device.GetGraphicsQueueTimelineSemaphore();
		VkResult failure = VK_SUCCESS;
		const GpuWaitResult result = RunBoundedGpuWait([injectHang, vulkanDevice, timeline, submissionID, &failure]()
		{
			if (injectHang)
				return GpuWaitSliceResult::Timeout;
			return Utils::WaitSemaphoreSlice(vulkanDevice, timeline, submissionID, failure);
		});

		switch (result)
		{
			case GpuWaitResult::Signaled:
			{
				return;
			}
			case GpuWaitResult::Hang:
			{
				const uint64_t sliceMilliseconds = GpuWaitSliceNanoseconds / 1'000'000;
				FatalError(FatalErrorKind::GpuHang, std::format("{}: GPU submission {} did not complete within {} waits of {} ms", context, submissionID, GpuWaitMaxSlices, sliceMilliseconds));
			}
			case GpuWaitResult::DeviceLost:
			{
				// Reported and recovered at the frame boundary; the caller finishes without the GPU (decision 28).
				device.ReportDeviceLost(std::format("{}: waiting for GPU submission {}", context, submissionID));
				return;
			}
			case GpuWaitResult::Failed:
			{
				// Out-of-memory results are OutOfMemory, anything else Gpu (§4.6).
				FatalError(GetFatalErrorKind(failure),
					std::format("{}: waiting for GPU submission {} failed with {}", context, submissionID, VkResultToString(failure)));
			}
		}

		ENGINE_CORE_ASSERT(false, "Unknown GpuWaitResult {}", std::to_underlying(result));
		FatalError(FatalErrorKind::Gpu, std::format("{}: waiting for GPU submission {} ended unexpectedly", context, submissionID));
	}

	FramePacer::FramePacer(GraphicsDevice& device, uint32_t framesInFlight)
		: m_Device(&device), m_FramesInFlight(framesInFlight)
	{
		// The slot is the frame index modulo this count.
		ENGINE_CORE_VERIFY(framesInFlight >= 1, "FramePacer needs at least one frame in flight");
		m_Slots.resize(framesInFlight);
		for (uint32_t slot = 0; slot < framesInFlight; ++slot)
		{
			// Objects created at startup: a failure means the device is out of memory (§8.14 item 7), unless the device is
			// lost, which a failed creation then is (decision 28): the slot stays without a query, and the pacer's owner
			// replaces the pacer when it recovers the device.
			Result<nvrhi::EventQueryHandle> query = device.CreateEventQuery();
			if (!query.has_value() && device.IsLost())
				continue;
			if (!query.has_value())
				FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot create the frame pacer's event query {}: {}", slot, query.error().ToString()));
			m_Slots[slot].EventQuery = std::move(*query);
		}
	}

	FramePacer::~FramePacer() = default;

	void FramePacer::BeginFrame()
	{
		ENGINE_CORE_ASSERT(!m_IsInFrame, "FramePacer::BeginFrame called twice without EndFrame");
		m_FrameIndex = m_BegunFrames++;
		m_FrameSlot = static_cast<uint32_t>(m_FrameIndex % m_FramesInFlight);
		m_IsInFrame = true;

		// The frame that last used this slot must have completed before its resources are reused (frame N waits for frame
		// N - FramesInFlight). The first FramesInFlight frames find no submission and never wait, and on a lost device no
		// frame waits: it runs nothing more (decision 28).
		const Slot& slot = m_Slots[m_FrameSlot];
		if (slot.SubmissionID == 0 || m_Device->IsLost())
			return;
		const bool injectHang = m_Device->GetDiagnostics().ShouldInjectHang();
		if (injectHang || slot.EventQuery == nullptr || !m_Device->GetNvrhiDevice()->pollEventQuery(slot.EventQuery))
			WaitForSubmission(*m_Device, slot.SubmissionID, "FramePacer::BeginFrame");
	}

	void FramePacer::EndFrame(uint64_t lastSubmissionID)
	{
		ENGINE_CORE_ASSERT(m_IsInFrame, "FramePacer::EndFrame called without BeginFrame");
		m_IsInFrame = false;
		Slot& slot = m_Slots[m_FrameSlot];
		slot.SubmissionID = lastSubmissionID;
		// A lost device records no event (decision 28): its owner replaces the pacer when it recovers the device, and a slot
		// created on a lost device has no query.
		if (m_Device->IsLost() || slot.EventQuery == nullptr)
			return;
		// NVRHI asserts that an event query is reset before it is set again.
		nvrhi::IDevice* nvrhiDevice = m_Device->GetNvrhiDevice();
		nvrhiDevice->resetEventQuery(slot.EventQuery);
		nvrhiDevice->setEventQuery(slot.EventQuery, nvrhi::CommandQueue::Graphics);
	}

	uint32_t FramePacer::GetFrameSlot() const
	{
		return m_FrameSlot;
	}

	uint64_t FramePacer::GetFrameIndex() const
	{
		return m_FrameIndex;
	}

	uint32_t FramePacer::GetFramesInFlight() const
	{
		return m_FramesInFlight;
	}

	std::string_view GpuWaitResultToString(GpuWaitResult result)
	{
		switch (result)
		{
			case GpuWaitResult::Signaled:   return "Signaled";
			case GpuWaitResult::Hang:       return "Hang";
			case GpuWaitResult::DeviceLost: return "DeviceLost";
			case GpuWaitResult::Failed:     return "Failed";
		}

		ENGINE_CORE_ASSERT(false, "Unknown GpuWaitResult {}", std::to_underlying(result));
		return "Unknown";
	}

}
